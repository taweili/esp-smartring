#!/usr/bin/env python3
"""
test_geodis_api.py — host-side verification of the GEODIS Voice Agent API.

The GEODIS UAT agent is Azure OpenAI Realtime (`gpt-realtime-2.1`) reached
over WebRTC, fronted by an unauthenticated gateway. There is no REST chat API,
so "verifying the API" means two things:

  1. the HTTP surface — `/token`, `/tool-call`, `/api/v1/save-message`,
     `/api/v1/transcripts/{id}`, `/api/v1/info`, and the absence of a
     WebSocket route;
  2. a real WebRTC session — SDP exchange, `session.update` (STT opt-in),
     a text or voice turn, the client-executed RAG tool call, and the spoken
     reply.

This is an M0 spike for the GEODIS voice work: it captures the exact event
shapes a ring `ring_geodis` module has to parse.

Usage:
    # HTTP surface only (stdlib only — no venv needed)
    python3 scripts/test_geodis_api.py
    python3 scripts/test_geodis_api.py --mode http

    # TLS trust for both hosts (stdlib only)
    python3 scripts/test_geodis_api.py --mode tls

    # token lifetime probe (stdlib only; waits --token-wait seconds)
    python3 scripts/test_geodis_api.py --mode token --token-wait 60

    # full WebRTC session, deterministic text turn (needs scripts/.venv)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode webrtc

    # WebRTC session with a spoken question (file input, no microphone)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode voice
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode voice --clip /tmp/q.wav

    # push-to-talk: turn_detection=null, stream a clip, commit + response.create
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode ptt
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode ptt --clip /tmp/q.wav

    # live microphone -> agent -> speaker (interactive, multiple turns)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode live
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode live --list-devices

    # everything (HTTP + TLS + WebRTC text turn)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode all -v

Exit codes:
    0  every check passed
    1  one or more checks failed
    2  setup error (aiortc missing, clip unreadable, …)

The endpoint is unauthenticated and metered: every `--mode webrtc` /
`voice` / `live` run starts a real Azure Realtime session. Do not run it in a
loop.
"""
import argparse
import asyncio
import contextlib
import ctypes
import fractions
import json
import math
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave
from urllib.parse import urlparse
from array import array

# The HTTP mode is stdlib-only; the audio/WebRTC modes need these. Import
# them lazily so `--mode http` still runs where aiortc/av/sounddevice are
# absent, and the live/webrtc entry points report the missing dependency.
try:
    import sounddevice as sd
    _SD_IMPORT_ERROR = None
except Exception as e:  # noqa: BLE001
    sd = None
    _SD_IMPORT_ERROR = e

try:
    from aiortc import AudioStreamTrack
    from aiortc.mediastreams import MediaStreamError
    from av import AudioFrame, AudioResampler
    _AV_IMPORT_ERROR = None
except Exception as e:  # noqa: BLE001
    AudioStreamTrack = object
    AudioFrame = None
    MediaStreamError = Exception
    _AV_IMPORT_ERROR = e

BASE = os.environ.get(
    "GEODIS_BASE", "https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1"
)
AZURE_SDP = os.environ.get(
    "GEODIS_SDP_URL",
    "https://geodisopenai.openai.azure.com/openai/v1/realtime/calls",
)
AZURE_WS = (
    "https://geodisopenai.openai.azure.com/openai/v1/realtime?model=gpt-realtime-2.1"
)
MODEL = "gpt-realtime-2.1"
HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print(f"  {msg}", flush=True)


def cue(msg):
    print(f"\n{'─' * 72}\n  {msg}\n{'─' * 72}\n", flush=True)


def device_arg(value):
    """argparse type for --mic-device / --speaker-device.

    sounddevice treats a *string* as a space-separated substring query over
    device names, not as an index, so `--mic-device 0` matched every device
    whose name contains a "0". A numeric value is an index; anything else
    is passed through as a name substring.
    """
    text = value.strip()
    try:
        return int(text)
    except ValueError:
        return text


# ------------------------------------------------------------------ report

class Report:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((bool(ok), name, detail))
        if ok:
            self.passed += 1
            print(f"  \033[32mPASS\033[0m  {name}" + (f"  — {detail}" if detail else ""))
        else:
            self.failed += 1
            print(f"  \033[31mFAIL\033[0m  {name}" + (f"  — {detail}" if detail else ""))
        return bool(ok)

    def section(self, title):
        print(f"\n\033[1m{title}\033[0m")

    def summary(self):
        total = self.passed + self.failed
        print("\n" + "─" * 72)
        print(f"  {self.passed}/{total} checks passed, {self.failed} failed")
        print("─" * 72)
        return 0 if self.failed == 0 else 1


# ------------------------------------------------------------------ HTTP

def http_request(method, url, body=None, headers=None, timeout=30, insecure=False,
                 retries=3):
    """Return (status, body_text, headers). status is -1 on transport error.

    Transport errors are retried (default 3 attempts): the GEODIS gateway
    intermittently resets TLS connections (~1 in 12 from the bench). HTTPError
    is a real response and is never retried.
    """
    ctx = ssl._create_unverified_context() if insecure else ssl.create_default_context()
    data = None
    if body is not None:
        data = bytes(body) if isinstance(body, (bytes, bytearray)) else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
                return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)
        except Exception as e:  # noqa: BLE001 - report the transport error verbatim
            if attempt + 1 >= retries:
                return -1, f"{type(e).__name__}: {e}", {}
            time.sleep(0.5 * (attempt + 1))
    return -1, "unreachable", {}


def get_token(body=None, timeout=30, insecure=False, retries=3):
    """Fetch an ephemeral token, retrying transport errors.

    The GEODIS gateway intermittently resets TLS connections (~1 in 12 from
    the bench), so a single attempt is not reliable. The POST is safe to retry:
    each attempt mints a fresh token.
    """
    last = (None, None, -1, "no attempt")
    for attempt in range(retries):
        status, text, _ = http_request(
            "POST", f"{BASE}/token", body=body if body is not None else {},
            headers={"Content-Type": "application/json"}, timeout=timeout,
            insecure=insecure,
        )
        if status == 200:
            try:
                d = json.loads(text)
                return d.get("token"), d.get("session_id"), status, text
            except Exception as e:  # noqa: BLE001
                last = (None, None, status, f"parse error: {e}: {text[:200]}")
        else:
            last = (None, None, status, text)
        if attempt + 1 < retries:
            time.sleep(0.5 * (attempt + 1))
    return last


def parse_tool_call(text):
    """The /tool-call response is a JSON-RPC wrapper whose `text` is itself a
    JSON string (double-escaped). Return the inner object."""
    outer = json.loads(text)
    inner_text = outer["result"]["result"]["content"][0]["text"]
    return json.loads(inner_text)


def run_http_checks(r, args):
    r.section("HTTP surface")
    sid = f"hermes-test-{uuid.uuid4().hex[:8]}"

    # 1. agent metadata
    status, body, _ = http_request("GET", f"{BASE}/api/v1/info", insecure=args.insecure)
    ok = status == 200
    detail = f"HTTP {status}"
    if status == 200:
        try:
            info = json.loads(body)
            caps = info.get("capabilities", [])
            ok = (
                info.get("agent_type") == "voice"
                and "webrtc" in caps
                and "rag" in caps
            )
            detail = (
                f"agent_type={info.get('agent_type')} capabilities={caps} "
                f"kb_count={info.get('kb_count')}"
            )
        except Exception as e:  # noqa: BLE001
            detail = f"parse error: {e}"
    r.check("GET /api/v1/info", ok, detail)

    # 2. ephemeral token, empty body (the security finding)
    token, session_id, status, body = get_token({}, insecure=args.insecure)
    ok = status == 200 and token and token.startswith("ek_") and session_id
    r.check(
        "POST /token (empty body, no auth)",
        ok,
        f"HTTP {status} token={token[:14] + '…' if token else None} "
        f"session_id={session_id}",
    )
    if not token:
        r.check("POST /token (with system_prompt + chat_session_id)", False,
                "no token from the empty-body call; skipping")
        r.check("POST /tool-call (arguments as JSON string)", False,
                "no token; skipping")
        r.check("POST /tool-call (object arguments -> HTTP 500)", False,
                "no token; skipping")
        r.check("POST /api/v1/save-message + GET /transcripts", False,
                "no token; skipping")
        return

    # 3. token with parameters
    _, _, status, body = get_token(
        {"system_prompt": "You are a test harness.", "chat_session_id": sid},
        insecure=args.insecure,
    )
    r.check(
        "POST /token (system_prompt + chat_session_id)",
        status == 200 and '"token"' in body,
        f"HTTP {status}",
    )

    # 4. RAG tool call — arguments MUST be a JSON string
    args_str = json.dumps({"query": "geodisxx2026"})
    status, body, _ = http_request(
        "POST", f"{BASE}/tool-call",
        body={"name": "rechercher_base_geodis", "arguments": args_str,
              "session_id": session_id},
        headers={"Content-Type": "application/json"}, insecure=args.insecure,
    )
    ok = status == 200
    detail = f"HTTP {status}"
    chunks = []
    if status == 200:
        try:
            inner = parse_tool_call(body)
            chunks = inner.get("retrievedChunks") or []
            ok = len(chunks) > 0
            src = chunks[0].get("relativefilepath", "") if chunks else ""
            detail = f"{len(chunks)} chunk(s), top score={chunks[0].get('score') if chunks else '—'} src={src[:60]}"
        except Exception as e:  # noqa: BLE001
            detail = f"parse error: {e}; body={body[:200]}"
    r.check("POST /tool-call (arguments as JSON string)", ok, detail)

    # 5. the documented quirk: an object instead of a string is a 500
    status, body, _ = http_request(
        "POST", f"{BASE}/tool-call",
        body={"name": "rechercher_base_geodis",
              "arguments": {"query": "geodisxx2026"},
              "session_id": session_id},
        headers={"Content-Type": "application/json"}, insecure=args.insecure,
    )
    r.check(
        "POST /tool-call (object arguments -> HTTP 500)",
        status == 500,
        f"HTTP {status}: {body[:100]}",
    )

    # 6. transcript persistence round-trip
    text = "Bonjour, je cherche mon colis"
    status, body, _ = http_request(
        "POST", f"{BASE}/api/v1/save-message",
        body={"chat_session_id": sid, "role": "user", "text": text},
        headers={"Content-Type": "application/json"}, insecure=args.insecure,
    )
    save_ok = status == 200 and '"ok"' in body
    status, body, _ = http_request(
        "GET", f"{BASE}/api/v1/transcripts/{sid}", insecure=args.insecure
    )
    tx_ok = False
    detail = f"save HTTP {status}"
    if status == 200:
        try:
            d = json.loads(body)
            msgs = d.get("messages", [])
            tx_ok = d.get("count", 0) >= 1 and any(m.get("text") == text for m in msgs)
            detail = f"count={d.get('count')} messages={[m.get('text') for m in msgs]}"
        except Exception as e:  # noqa: BLE001
            detail = f"parse error: {e}"
    r.check("POST /api/v1/save-message + GET /transcripts", save_ok and tx_ok, detail)

    # 7. the WebSocket route is not exposed by the gateway (documents the
    #    architecture decision: WebRTC is the only transport)
    import base64
    key = base64.b64encode(os.urandom(16)).decode()
    status, body, _ = http_request(
        "GET", AZURE_WS,
        headers={"Connection": "Upgrade", "Upgrade": "websocket",
                 "Sec-WebSocket-Key": key, "Sec-WebSocket-Version": "13"},
        insecure=args.insecure,
    )
    r.check(
        "WebSocket route unavailable (expect 404)",
        status == 404,
        f"HTTP {status}: {body[:80]}",
    )

    # 8. the SDP route exists: a bogus offer is a 400, not a 404
    status, body, _ = http_request(
        "POST", AZURE_SDP,
        body=b"v=0",
        headers={"Authorization": f"Bearer {token}",
                 "Content-Type": "application/sdp"},
        insecure=args.insecure,
    )
    r.check(
        "SDP route exists (bogus offer -> 400)",
        status == 400,
        f"HTTP {status}: {body[:100]}",
    )


# ------------------------------------------------------------------ TLS / token

def run_tls_checks(r, args):
    """Verify both hosts against the Mozilla bundle (certifi) or the system store.

    The firmware uses esp_crt_bundle_attach (a Mozilla-derived bundle), so a
    host that validates here is very likely to validate on the ring. This is a
    proxy, not a guarantee: the ESP bundle is a subset of Mozilla's roots.
    """
    import socket

    r.section("TLS trust")
    try:
        import certifi
        cafile = certifi.where()
        store = "certifi (Mozilla)"
    except ImportError:
        cafile = None
        store = "system"

    hosts = []
    for url in (BASE, AZURE_SDP):
        host = urlparse(url).hostname
        if host and host not in hosts:
            hosts.append(host)

    def common_name(cert, field):
        for rdn in cert.get(field, []):
            for key, value in rdn:
                if key == "commonName":
                    return value
        return "?"

    for host in hosts:
        try:
            ctx = ssl.create_default_context(cafile=cafile)
            with socket.create_connection((host, 443), timeout=10) as sock:
                with ctx.wrap_socket(sock, server_hostname=host) as ssock:
                    cert = ssock.getpeercert() or {}
                    version = ssock.version()
                    cipher = ssock.cipher()[0]
            r.check(
                f"TLS {host}",
                True,
                f"{version} {cipher} leaf={common_name(cert, 'subject')} "
                f"issuer={common_name(cert, 'issuer')} [{store}]",
            )
        except Exception as e:  # noqa: BLE001
            r.check(f"TLS {host}", False, f"{type(e).__name__}: {e} [{store}]")


def run_token_checks(r, args):
    """Probe an ephemeral token's lifetime with a bogus SDP offer.

    A bogus offer (`v=0`) is rejected with 400 once the token is accepted; an
    expired token should be 401/403. The probe does not start a session.
    """
    r.section("Token lifetime")
    token, session_id, status, body = get_token(
        {"chat_session_id": args.session}, insecure=args.insecure
    )
    ok = status == 200 and bool(token) and token.startswith("ek_")
    r.check("POST /token", ok,
            f"HTTP {status} token={token[:14] + '…' if token else None}")
    if not token:
        return

    def probe(tag):
        st, bd, _ = http_request(
            "POST", AZURE_SDP, body=b"v=0",
            headers={"Authorization": f"Bearer {token}",
                     "Content-Type": "application/sdp"},
            insecure=args.insecure,
        )
        r.check(f"token accepted {tag}", st == 400, f"HTTP {st}: {bd[:100]}")

    probe("immediately (bogus offer -> 400)")
    if args.token_wait > 0:
        print(f"  waiting {args.token_wait:g}s …")
        time.sleep(args.token_wait)
        probe(f"after {args.token_wait:g}s")


# ------------------------------------------------------------------ audio I/O

SAMPLE_RATE = 48000
FRAME_SAMPLES = 960          # 20 ms at 48 kHz
PLAYBACK_MODES = ("buffer", "live", "adaptive")


class GatedAudioTrack(AudioStreamTrack):
    """Base for audio tracks that produce no frames until the session is ready.

    The server rejects a `session.update` that changes `turn_detection` once
    it has started processing audio, so no frame may be produced until the
    session has been configured.
    """

    kind = "audio"

    def __init__(self):
        super().__init__()
        self._enabled = asyncio.Event()
        self.frames_sent = 0
        self.peak = 0

    def enable(self):
        """Start producing frames (call after session.updated)."""
        self._enabled.set()

    async def _wait_enabled(self):
        if self.readyState != "live":
            raise MediaStreamError
        await self._enabled.wait()
        if self.readyState != "live":
            raise MediaStreamError

    def note_frame(self, pcm):
        """Record what was actually put on the wire (frames + peak level)."""
        self.frames_sent += 1
        peak = max((abs(v) for v in array("h", pcm)), default=0)
        if peak > self.peak:
            self.peak = peak

    def stats(self):
        return (self.frames_sent,
                self.frames_sent * FRAME_SAMPLES / SAMPLE_RATE,
                self.peak)

    def stop(self):
        self._enabled.set()
        super().stop()


class MicTrack(GatedAudioTrack):
    """An audio track that streams a live input device (the microphone).

    Half-duplex: `pause()` / `resume()` gate the uplink while the assistant
    is speaking, so the model does not hear itself through the speaker.
    """

    def __init__(self, sample_rate=SAMPLE_RATE, frame_samples=FRAME_SAMPLES,
                 device=None):
        super().__init__()
        self.sample_rate = sample_rate
        self.frame_samples = frame_samples
        self._frame_bytes = frame_samples * 2
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._start = None
        self._timestamp = 0
        self._closed = False
        self._gate = asyncio.Event()
        self._gate.set()
        self._stream = sd.RawInputStream(
            samplerate=sample_rate,
            channels=1,
            dtype="int16",
            blocksize=frame_samples,
            device=device,
            callback=self._callback,
        )
        self._stream.start()

    def _callback(self, indata, frames, time_info, status):
        if status:
            log(f"mic status: {status}")
        with self._lock:
            self._buf.extend(bytes(indata))
            # If the event loop falls behind, drop the oldest audio rather
            # than growing without bound.
            max_bytes = self._frame_bytes * 10
            if len(self._buf) > max_bytes:
                del self._buf[:len(self._buf) - max_bytes]

    def _take_frame(self):
        with self._lock:
            if len(self._buf) < self._frame_bytes:
                data = bytes(self._buf) + b"\x00" * (self._frame_bytes - len(self._buf))
                self._buf.clear()
            else:
                data = bytes(self._buf[:self._frame_bytes])
                del self._buf[:self._frame_bytes]
            return data

    def pause(self):
        """Mute the uplink (called when the assistant starts replying)."""
        self._gate.clear()

    def resume(self):
        """Re-open the uplink, re-basing the real-time schedule so the paused
        interval does not become a burst of catch-up frames."""
        self._gate.set()
        self._start = time.monotonic() - self._timestamp / self.sample_rate

    async def recv(self):
        await self._wait_enabled()
        await self._gate.wait()
        if self.readyState != "live":
            raise MediaStreamError

        # Pace the frames to real time, like a live microphone.
        if self._start is None:
            self._start = time.monotonic()
        else:
            wait = self._start + (self._timestamp / self.sample_rate) - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)

        data = self._take_frame()
        frame = AudioFrame(format="s16", layout="mono", samples=self.frame_samples)
        frame.planes[0].update(data)
        frame.pts = self._timestamp
        frame.sample_rate = self.sample_rate
        frame.time_base = fractions.Fraction(1, self.sample_rate)
        self._timestamp += self.frame_samples
        self.note_frame(data)
        return frame

    def stop(self):
        if not self._closed:
            self._closed = True
            self._gate.set()
            try:
                self._stream.stop()
                self._stream.close()
            except Exception as e:
                log(f"mic close error: {e}")
        super().stop()


class Speaker:
    """Plays 48 kHz / 16-bit mono PCM through an output device.

    `mode="buffer"` waits for the whole reply, then plays it back-to-back at
    the card's rate (a raw player fed as the frames arrive plays at the
    *delivery* rate and stretches the speech); `live` streams as the frames
    arrive; `adaptive` starts once `cushion` seconds are queued.
    """

    def __init__(self, sample_rate=SAMPLE_RATE, frame_samples=FRAME_SAMPLES,
                 device=None, mode="buffer", cushion=0.6):
        self.sample_rate = sample_rate
        self.frame_samples = frame_samples
        self._frame_bytes = frame_samples * 2
        self.mode = mode
        self.cushion = max(0.0, float(cushion or 0.0))
        self._cushion_bytes = int(self.cushion * sample_rate * 2)
        self._buf = bytearray()
        self._pos = 0
        self._lock = threading.Lock()
        self._max_bytes = self._frame_bytes * 200
        self._go = threading.Event()
        self._finished = False
        self._closed = False
        self._stream = sd.RawOutputStream(
            samplerate=sample_rate,
            channels=1,
            dtype="int16",
            blocksize=frame_samples,
            device=device,
            callback=self._callback,
        )
        self._stream.start()

    def _callback(self, outdata, frames, time_info, status):
        if status:
            log(f"speaker status: {status}")
        need = len(outdata)
        silence = b"\x00" * need
        if self._closed:
            outdata[:] = silence
            return
        if self.mode == "buffer" and not self._go.is_set():
            outdata[:] = silence
            return
        if self.mode == "adaptive" and not self._finished:
            with self._lock:
                pending = len(self._buf) - self._pos
            if pending < self._cushion_bytes:
                outdata[:] = silence
                return
        with self._lock:
            avail = len(self._buf) - self._pos
            if avail >= need:
                outdata[:] = self._buf[self._pos:self._pos + need]
                self._pos += need
            elif avail > 0:
                outdata[:avail] = self._buf[self._pos:]
                outdata[avail:] = b"\x00" * (need - avail)
                self._pos = len(self._buf)
            else:
                outdata[:] = silence
            if self._pos == len(self._buf):
                self._buf.clear()
                self._pos = 0
            elif self._pos > 1 << 16:
                del self._buf[:self._pos]
                self._pos = 0

    def play(self, pcm):
        if self._closed or not pcm:
            return
        with self._lock:
            self._buf.extend(pcm)
            if self.mode != "buffer":
                if len(self._buf) - self._pos > self._max_bytes:
                    del self._buf[:self._pos]
                    self._pos = 0
                    if len(self._buf) > self._max_bytes:
                        del self._buf[:len(self._buf) - self._max_bytes]

    def finish(self):
        if self._closed:
            return
        self._finished = True
        self._go.set()

    def empty(self):
        if self.mode == "buffer":
            if not self._go.is_set():
                return True
            with self._lock:
                return not self._buf and self._pos == 0
        with self._lock:
            return len(self._buf) == self._pos

    async def drain(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while not self.empty() and time.monotonic() < deadline:
            await asyncio.sleep(0.05)

    def close(self):
        self._closed = True
        self._finished = True
        self._go.set()
        try:
            self._stream.stop()
            self._stream.close()
        except Exception as e:
            log(f"speaker close error: {e}")


def frame_to_pcm(frame):
    """Packed mono s16 PCM of an av.AudioFrame, without numpy.

    PyAV's AudioPlane has no `to_bytes()` in the wheels it uses, so read the
    plane buffer through ctypes and average the channels by hand. The Opus
    decoder outputs interleaved stereo, which must be deinterleaved rather
    than written out as-is.
    """
    channels = len(frame.layout.channels)
    if channels == 1:
        return ctypes.string_at(frame.planes[0].buffer_ptr,
                                frame.samples * 2)
    if len(frame.planes) == channels:  # planar: one plane per channel
        planes = [array("h", ctypes.string_at(p.buffer_ptr, frame.samples * 2))
                  for p in frame.planes]
        if sys.byteorder == "big":
            for plane in planes:
                plane.byteswap()
        out = array("h", bytes(frame.samples * 2))
        for i in range(frame.samples):
            out[i] = sum(plane[i] for plane in planes) // channels
        return out.tobytes()
    values = array("h", ctypes.string_at(
        frame.planes[0].buffer_ptr, frame.samples * channels * 2))
    if sys.byteorder == "big":
        values.byteswap()
    out = array("h", bytes(frame.samples * 2))
    for i in range(0, len(values), channels):
        out[i // channels] = sum(values[i:i + channels]) // channels
    return out.tobytes()


def save_wav(frames, path, sample_rate=SAMPLE_RATE):
    """Write a list of av.AudioFrame to a 16-bit mono WAV at `sample_rate`."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    resampler = None
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        for frame in frames:
            if frame.sample_rate != sample_rate:
                if resampler is None:
                    resampler = AudioResampler(format="s16", layout="mono",
                                               rate=sample_rate)
                resampled = resampler.resample(frame)
                if resampled is None:
                    continue
                for out in resampled:
                    w.writeframes(frame_to_pcm(out))
            else:
                w.writeframes(frame_to_pcm(frame))
        if resampler is not None:
            for out in resampler.resample(None) or []:
                w.writeframes(frame_to_pcm(out))


async def collect_audio(track, speaker, mic, state, frames):
    """Play the assistant's audio and mute the mic while it is speaking.

    The server sends digital-silence keep-alive frames as soon as the media
    path is up (before `session.created`). Those must be ignored: they are not
    the reply, and treating them as one would pause the mic before the user has
    spoken and reset the reply-drain timer forever.
    """
    logged = False
    while True:
        try:
            frame = await track.recv()
        except (MediaStreamError, asyncio.TimeoutError):
            break
        except Exception as e:  # noqa: BLE001
            log(f"audio collect error: {e}")
            break
        pcm = frame_to_pcm(frame)
        peak = max((abs(v) for v in array("h", pcm)), default=0)
        if peak == 0:
            continue
        if not logged:
            logged = True
            log(f"audio frame: format={frame.format.name} "
                f"layout={frame.layout.name} rate={frame.sample_rate}")
            if mic is not None:
                mic.pause()
                cue("REPLY — microphone muted; just listen")
        state["count"] += 1
        state["last"] = time.monotonic()
        if speaker is not None:
            speaker.play(pcm)
        frames.append(frame)


async def drain_reply(state, speaker, max_seconds, silence_seconds=2.0):
    """Wait for the reply to stop arriving and finish playing.

    `response.done` can arrive before the first audio frame (the data channel
    and RTP are separate), so when no frame has arrived yet we give the audio
    a grace period instead of returning immediately.
    """
    if state["count"] == 0:
        grace = time.monotonic() + min(max_seconds, 5.0)
        while state["count"] == 0 and time.monotonic() < grace:
            await asyncio.sleep(0.05)
        if state["count"] == 0:
            return
    deadline = time.monotonic() + max_seconds
    while time.monotonic() < deadline:
        quiet = time.monotonic() - state["last"] >= silence_seconds
        if quiet and speaker is not None:
            speaker.finish()
        playing = speaker is not None and not speaker.empty()
        if quiet and not playing:
            return
        await asyncio.sleep(0.1)
    log(f"reply still arriving after {max_seconds:.0f} s — cutting off")


async def uplink_watch(track, interval=5.0, stall_after=15.0):
    """Narrate the uplink while the user is expected to be talking.

    "The model never answers" has two very different causes: the audio never
    reached it (muted/wrong microphone) or the audio arrived and the server
    VAD rejected it. The peak level separates them.
    """
    silence_polls = 0
    heard = None
    elapsed = 0.0
    while True:
        await asyncio.sleep(interval)
        elapsed += interval
        frames, seconds, peak = track.stats()
        if peak == 0:
            silence_polls += 1
            if silence_polls == 1 or silence_polls % 2 == 1:
                log(f"uplink: {frames} frames ({seconds:.0f} s) sent, all "
                    f"digital silence — the model cannot hear anything. Wrong "
                    f"or muted capture device? (check --list-devices / "
                    f"--mic-device)")
            continue
        dbfs = 20 * math.log10(peak / 32768)
        if heard is None:
            heard = elapsed
            if dbfs < -40:
                log(f"uplink: peak {dbfs:.0f} dBFS — very quiet, the VAD will "
                    f"probably miss it. Speak up, or move closer to the mic")
            else:
                log(f"uplink: peak {dbfs:.0f} dBFS — the model has audio; "
                    f"waiting for your turn to end")
        elif elapsed - heard >= stall_after:
            heard = elapsed
            log(f"uplink: still no reply after audio was heard — the turn may "
                f"have been dropped; speak again")


# ------------------------------------------------------------------ WebRTC

def _wav_duration(path):
    try:
        with wave.open(path, "rb") as w:
            return w.getnframes() / float(w.getframerate())
    except Exception:  # noqa: BLE001 - not a WAV (raw PCM)
        return 0.0


def ensure_wav(path, rate=48000, channels=1):
    """Return a WAV path for `path`. Raw s16le PCM is wrapped in a temp WAV."""
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic == b"RIFF":
        return path
    out = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    out.close()
    with wave.open(out.name, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        with open(path, "rb") as f:
            w.writeframes(f.read())
    return out.name


def make_silence_wav(path, seconds=60, rate=24000):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * rate * seconds)
    return path


def make_question_wav(path):
    """A spoken GEODIS question, generated with espeak (no microphone needed)."""
    text = ("Hello, where is my shipment? "
            "The reference number is geodis x x 2026.")
    try:
        subprocess.run(
            ["espeak", "-v", "en", "-s", "150", "-w", path, text],
            check=True, capture_output=True,
        )
        return path
    except Exception as e:  # noqa: BLE001
        print(f"  (espeak failed: {e}; pass --clip /path/to.wav)")
        return None


def send_json(dc, obj):
    dc.send(json.dumps(obj, ensure_ascii=False))


def parse_sdp_audio(answer):
    """Return the first m=audio payload type and its rtpmap from an SDP answer.

    Returns None when the answer has no audio m-line. `rtpmap` is the codec
    spec (e.g. "opus/48000/2") for the first payload type, or None when it is
    not advertised.
    """
    if not answer:
        return None
    rtpmaps = {}
    m_audio = None
    for ln in answer.splitlines():
        ln = ln.strip()
        if ln.startswith("a=rtpmap:"):
            spec = ln[len("a=rtpmap:"):]
            pt, _, codec = spec.partition(" ")
            rtpmaps[pt] = codec
        elif ln.startswith("m=audio "):
            parts = ln.split()
            if len(parts) >= 4 and m_audio is None:
                m_audio = parts[3:]
    if not m_audio:
        return None
    pt = m_audio[0]
    return {"pt": pt, "rtpmap": rtpmaps.get(pt),
            "all_pts": m_audio, "rtpmaps": rtpmaps}


def send_session_update(dc, enable_stt=True, voice="coral", instructions=None,
                        ptt=False):
    """Configure the session.

    ptt=True sends `audio.input.turn_detection = null` — the GA schema nests
    turn detection under `audio.input`, and null disables server VAD so the
    client can commit manually. The old top-level `session.turn_detection`
    (beta shape) is rejected with `unknown_parameter`; `modalities` is
    likewise read-only.
    """
    session = {"type": "realtime"}
    audio = {}
    audio_input = {}
    if enable_stt:
        # STT is OFF by default; this is the opt-in the ring must send.
        audio_input["transcription"] = {"model": "whisper-1"}
    if ptt:
        audio_input["turn_detection"] = None
    if audio_input:
        audio["input"] = audio_input
    if voice:
        audio["output"] = {"voice": voice}
    if audio:
        session["audio"] = audio
    if instructions:
        session["instructions"] = instructions
    send_json(dc, {"type": "session.update", "session": session})


async def run_webrtc(r, args, voice, ptt=False):
    try:
        from aiortc import (
            RTCConfiguration, RTCIceServer, RTCPeerConnection,
            RTCSessionDescription,
        )
        from aiortc.contrib.media import MediaPlayer, MediaRecorder
    except ImportError as e:
        print(f"\n  aiortc is required for --mode {args.mode}: {e}")
        print("  scripts/.venv/bin/pip install aiortc")
        return 2

    r.section("WebRTC session" + (" (PTT)" if ptt else " (voice)" if voice else " (text)"))

    # 1. token
    token, session_id, status, body = get_token(
        {"chat_session_id": args.session}, insecure=args.insecure
    )
    if not token:
        r.check("POST /token", False, f"HTTP {status}: {body[:200]}")
        return 1
    r.check("POST /token", True, f"session_id={session_id}")

    # 2. peer connection
    ice = [] if args.no_stun else [RTCIceServer(urls=args.stun)]
    pc = RTCPeerConnection(RTCConfiguration(iceServers=ice))

    # 3. audio source
    tmp = None
    if voice:
        clip = args.clip
        if not clip:
            tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
            tmp.close()
            clip = make_question_wav(tmp.name)
            if not clip:
                await pc.close()
                return 2
        wav = ensure_wav(clip)
        clip_dur = _wav_duration(wav) or 8.0
        player = MediaPlayer(wav, format="wav")
        print(f"  audio in: {clip} ({clip_dur:.1f} s)")
    else:
        tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        tmp.close()
        make_silence_wav(tmp.name)
        player = MediaPlayer(tmp.name, format="wav", loop=True)
        clip_dur = 0.0
        print("  audio in: 60 s silence loop (text turn)")
    if player.audio:
        pc.addTrack(player.audio)

    # 4. data channel + event state
    dc = pc.createDataChannel("realtime-channel")
    loop = asyncio.get_event_loop()
    st = {
        "created": asyncio.Event(), "updated": asyncio.Event(),
        "response_done": asyncio.Event(), "speech_stopped": asyncio.Event(),
        "default_stt": None, "stt": None, "default_turn": None,
        "turn": None,
        "user_text": "", "assistant_text": "", "last_transcript": "",
        "fn_calls": 0, "responses_done": 0, "pending_fn": False,
        "errors": [], "tool_status": None, "tool_chunks": 0,
        "audio_frames": 0,
    }

    def log_event(ev):
        if args.verbose:
            print("    dc:", json.dumps(ev, ensure_ascii=False)[:300])

    async def handle_tool_call(ev):
        """The backend does NOT execute the tool; the client must."""
        call_id = ev.get("call_id")
        name = ev.get("name", "rechercher_base_geodis")
        arguments = ev.get("arguments", "{}")
        status, body, _ = await loop.run_in_executor(
            None, lambda: http_request(
                "POST", f"{BASE}/tool-call",
                body={"name": name, "arguments": arguments,
                      "session_id": session_id},
                headers={"Content-Type": "application/json"},
                insecure=args.insecure,
            )
        )
        st["tool_status"] = status
        try:
            inner = parse_tool_call(body)
            st["tool_chunks"] = len(inner.get("retrievedChunks") or [])
            output = json.dumps(inner, ensure_ascii=False)
        except Exception as e:  # noqa: BLE001
            output = json.dumps({"error": f"{type(e).__name__}: {e}"})
        send_json(dc, {
            "type": "conversation.item.create",
            "item": {"type": "function_call_output",
                     "call_id": call_id, "output": output},
        })
        send_json(dc, {"type": "response.create"})
        st["pending_fn"] = False

    @dc.on("open")
    def on_open():
        print("  data channel open")

    @dc.on("message")
    def on_message(msg):
        try:
            ev = json.loads(msg)
        except Exception:  # noqa: BLE001
            return
        log_event(ev)
        t = ev.get("type", "?")
        if t == "session.created":
            sess = ev.get("session", {})
            st["default_stt"] = (
                sess.get("audio", {}).get("input", {}).get("transcription")
            )
            st["default_turn"] = (
                sess.get("audio", {}).get("input", {}).get("turn_detection")
            )
            st["created"].set()
        elif t == "session.updated":
            sess = ev.get("session", {})
            st["stt"] = sess.get("audio", {}).get("input", {}).get("transcription")
            st["turn"] = sess.get("audio", {}).get("input", {}).get("turn_detection")
            st["updated"].set()
        elif t == "conversation.item.input_audio_transcription.completed":
            st["user_text"] = ev.get("transcript", "")
        elif t == "response.output_audio_transcript.delta":
            st["assistant_text"] += ev.get("delta", "")
        elif t == "response.output_audio_transcript.done":
            st["last_transcript"] = ev.get("transcript", "")
        elif t == "response.function_call_arguments.done":
            st["fn_calls"] += 1
            st["pending_fn"] = True
            asyncio.ensure_future(handle_tool_call(ev))
        elif t == "response.done":
            st["responses_done"] += 1
            st["response_done"].set()
        elif t == "input_audio_buffer.speech_stopped":
            st["speech_stopped"].set()
        elif t == "error":
            st["errors"].append(ev)

    # 5. record the spoken reply
    recorder = None

    @pc.on("track")
    def on_track(track):
        nonlocal recorder
        if track.kind == "audio":
            print("  remote audio track received")
            recorder = MediaRecorder(args.out)
            recorder.addTrack(track)

    # 6. SDP exchange (retry transport errors; the gateway resets TLS ~8%)
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    status, answer = -1, ""
    for attempt in range(3):
        status, answer, _ = await loop.run_in_executor(
            None, lambda: http_request(
                "POST", AZURE_SDP,
                body=pc.localDescription.sdp.encode(),
                headers={"Authorization": f"Bearer {token}",
                         "Content-Type": "application/sdp"},
                timeout=40, insecure=args.insecure,
            )
        )
        if status == 201:
            break
        if attempt < 2:
            await asyncio.sleep(0.5 * (attempt + 1))
    r.check("SDP exchange -> HTTP 201", status == 201,
            f"HTTP {status}: {answer[:120]}")
    if status != 201:
        await pc.close()
        return 1
    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer, type="answer"))

    # The ring reuses the ring_net Opus pipeline only if the answer negotiates
    # Opus on m=audio. PCMA/PCMU would mean a different codec path.
    sdp_audio = parse_sdp_audio(answer)
    r.check(
        "SDP answer negotiates Opus",
        sdp_audio is not None and "opus" in (sdp_audio["rtpmap"] or "").lower(),
        f"m=audio pt={sdp_audio['pt'] if sdp_audio else None} "
        f"rtpmap={sdp_audio['rtpmap'] if sdp_audio else None}",
    )

    # 7. wait for the session, then configure it
    try:
        await asyncio.wait_for(st["created"].wait(), timeout=30)
    except asyncio.TimeoutError:
        r.check("session.created received", False, "timeout")
        await pc.close()
        return 1
    r.check(
        "session.created received (STT off by default, server_vad preset)",
        st["default_stt"] is None and
        (st["default_turn"]["type"] == "server_vad"
         if isinstance(st["default_turn"], dict) else False),
        f"default transcription={st['default_stt']} "
        f"turn_detection={st['default_turn']}",
    )

    send_session_update(dc, enable_stt=not args.no_stt,
                        voice=args.voice, instructions=args.system_prompt,
                        ptt=ptt)
    try:
        await asyncio.wait_for(st["updated"].wait(), timeout=15)
    except asyncio.TimeoutError:
        r.check("session.updated received", False,
                f"timeout; errors={st['errors'][:2]}")
        await pc.close()
        return 1
    if ptt:
        # A rejected update never produces session.updated (see the timeout
        # above); an accepted one echoes turn_detection: null.
        r.check(
            "session.updated received (PTT: turn_detection=null)",
            st["turn"] is None,
            f"turn_detection={st['turn']} errors={st['errors'][:1]}",
        )
    else:
        r.check(
            "session.updated received (STT opt-in applied)",
            isinstance(st["stt"], dict) and st["stt"].get("model") == "whisper-1",
            f"transcription={st['stt']}",
        )

    if recorder:
        await recorder.start()

    # 8. send the turn
    if ptt:
        # Hold-to-talk: the clip is the held uplink; release is an explicit
        # commit + response.create (server VAD is off, so nothing else would
        # ever trigger the reply).
        print(f"  PTT: streaming clip as held uplink ({clip_dur:.1f} s) …")
        await asyncio.sleep(clip_dur + 0.5)
        print("  PTT: release -> input_audio_buffer.commit + response.create")
        send_json(dc, {"type": "input_audio_buffer.commit"})
        send_json(dc, {"type": "response.create"})
    elif voice:
        print(f"  streaming clip ({clip_dur:.1f} s) …")
        try:
            await asyncio.wait_for(
                st["speech_stopped"].wait(), timeout=clip_dur + 10
            )
        except asyncio.TimeoutError:
            pass
        await asyncio.sleep(1.0)
        if st["responses_done"] == 0 and not st["pending_fn"]:
            print("  server VAD did not commit; sending commit + response.create")
            send_json(dc, {"type": "input_audio_buffer.commit"})
            send_json(dc, {"type": "response.create"})
    else:
        print(f"  text turn: {args.question}")
        send_json(dc, {
            "type": "conversation.item.create",
            "item": {"type": "message", "role": "user",
                     "content": [{"type": "input_text", "text": args.question}]},
        })
        send_json(dc, {"type": "response.create"})

    # 9. wait for the final response (one more than the function-call turns)
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if st["responses_done"] >= st["fn_calls"] + 1 and not st["pending_fn"]:
            break
        await asyncio.sleep(0.2)
    else:
        r.check("response.done received", False,
                f"timeout; responses={st['responses_done']} fn={st['fn_calls']} "
                f"errors={st['errors'][:2]}")
        await pc.close()
        return 1

    # 10. report
    if voice:
        r.check(
            "STT transcript of the spoken question",
            bool(st["user_text"]),
            f"“{st['user_text'][:120]}”",
        )
    r.check(
        "assistant reply transcript",
        bool(st["last_transcript"] or st["assistant_text"]),
        f"“{(st['last_transcript'] or st['assistant_text'])[:120]}”",
    )
    r.check(
        "RAG tool call executed by the client",
        st["fn_calls"] >= 1 and st["tool_status"] == 200 and st["tool_chunks"] > 0,
        f"fn_calls={st['fn_calls']} tool-call HTTP {st['tool_status']} "
        f"chunks={st['tool_chunks']}",
    )
    if st["errors"]:
        r.check("no session errors", False, json.dumps(st["errors"][:2])[:200])
    else:
        r.check("no session errors", True)

    if recorder:
        await recorder.stop()
        if os.path.exists(args.out) and os.path.getsize(args.out) > 44:
            r.check("spoken reply saved", True,
                    f"{args.out} ({os.path.getsize(args.out)} B)")
        else:
            r.check("spoken reply saved", False, f"{args.out} missing/empty")

    await pc.close()
    if player and player.audio:
        player.audio.stop()
    if tmp and os.path.exists(tmp.name):
        os.unlink(tmp.name)
    return 0


# ------------------------------------------------------------------ live mic/speaker

async def run_live(r, args):
    """Interactive session: host microphone -> GEODIS -> host speaker.

    Half-duplex: the uplink is paused as soon as the assistant's first audio
    frame arrives, and re-opened after the reply has been played. Runs
    `--turns` turns (Ctrl+C stops early).
    """
    if sd is None:
        print(f"\n  sounddevice is required for --mode live: {_SD_IMPORT_ERROR}")
        print("  scripts/.venv/bin/pip install sounddevice")
        return 2
    if _AV_IMPORT_ERROR is not None:
        print(f"\n  aiortc/av are required for --mode live: {_AV_IMPORT_ERROR}")
        return 2

    from aiortc import (
        RTCConfiguration, RTCIceServer, RTCPeerConnection,
        RTCSessionDescription,
    )

    r.section("WebRTC live session (mic + speaker)")

    token, session_id, status, body = get_token(
        {"chat_session_id": args.session}, insecure=args.insecure
    )
    if not token:
        r.check("POST /token", False, f"HTTP {status}: {body[:200]}")
        return 1
    r.check("POST /token", True, f"session_id={session_id}")

    mic = speaker = pc = None
    collect_tasks = []
    try:
        mic = MicTrack(device=args.mic_device)
        r.check("open microphone", True,
                f"device={args.mic_device if args.mic_device is not None else 'default'}")
        if not args.no_play:
            speaker = Speaker(device=args.speaker_device, mode=args.playback,
                              cushion=args.cushion)
            r.check("open speaker", True,
                    f"device={args.speaker_device if args.speaker_device is not None else 'default'} "
                    f"mode={args.playback}")
        else:
            log("speaker: muted (--no-play)")

        ice = [] if args.no_stun else [RTCIceServer(urls=args.stun)]
        pc = RTCPeerConnection(RTCConfiguration(iceServers=ice))
        pc.addTrack(mic)

        dc = pc.createDataChannel("realtime-channel")
        loop = asyncio.get_event_loop()
        st = {
            "created": asyncio.Event(), "updated": asyncio.Event(),
            "response_done": asyncio.Event(), "transcript_done": asyncio.Event(),
            "speech_stopped": asyncio.Event(),
            "default_stt": None, "stt": None, "default_turn": None,
            "user_text": "", "assistant_text": "", "last_transcript": "",
            "fn_calls": 0, "responses_done": 0, "pending_fn": False,
            "errors": [], "tool_status": None, "tool_chunks": 0,
        }
        audio_state = {"count": 0, "last": 0.0}
        received_frames = []

        async def handle_tool_call(ev):
            call_id = ev.get("call_id")
            name = ev.get("name", "rechercher_base_geodis")
            arguments = ev.get("arguments", "{}")
            status, body, _ = await loop.run_in_executor(
                None, lambda: http_request(
                    "POST", f"{BASE}/tool-call",
                    body={"name": name, "arguments": arguments,
                          "session_id": session_id},
                    headers={"Content-Type": "application/json"},
                    insecure=args.insecure,
                )
            )
            st["tool_status"] = status
            try:
                inner = parse_tool_call(body)
                st["tool_chunks"] = len(inner.get("retrievedChunks") or [])
                output = json.dumps(inner, ensure_ascii=False)
            except Exception as e:  # noqa: BLE001
                output = json.dumps({"error": f"{type(e).__name__}: {e}"})
            send_json(dc, {
                "type": "conversation.item.create",
                "item": {"type": "function_call_output",
                         "call_id": call_id, "output": output},
            })
            send_json(dc, {"type": "response.create"})
            st["pending_fn"] = False

        @dc.on("message")
        def on_message(msg):
            try:
                ev = json.loads(msg)
            except Exception:  # noqa: BLE001
                return
            if args.verbose:
                print("    dc:", json.dumps(ev, ensure_ascii=False)[:300])
            t = ev.get("type", "?")
            if t == "session.created":
                sess = ev.get("session", {})
                st["default_stt"] = (
                    sess.get("audio", {}).get("input", {}).get("transcription")
                )
                st["default_turn"] = (
                    sess.get("audio", {}).get("input", {}).get("turn_detection")
                )
                st["created"].set()
            elif t == "session.updated":
                sess = ev.get("session", {})
                st["stt"] = (
                    sess.get("audio", {}).get("input", {}).get("transcription")
                )
                st["updated"].set()
            elif t == "conversation.item.input_audio_transcription.completed":
                st["user_text"] = ev.get("transcript", "")
            elif t == "response.output_audio_transcript.delta":
                st["assistant_text"] += ev.get("delta", "")
            elif t == "response.output_audio_transcript.done":
                st["last_transcript"] = ev.get("transcript", "")
                st["transcript_done"].set()
            elif t == "response.function_call_arguments.done":
                st["fn_calls"] += 1
                st["pending_fn"] = True
                asyncio.ensure_future(handle_tool_call(ev))
            elif t == "response.done":
                st["responses_done"] += 1
                st["response_done"].set()
            elif t == "input_audio_buffer.speech_stopped":
                st["speech_stopped"].set()
            elif t == "error":
                st["errors"].append(ev)

        @pc.on("track")
        def on_track(track):
            if track.kind == "audio":
                log("remote audio track received")
                collect_tasks.append(asyncio.ensure_future(
                    collect_audio(track, speaker, mic, audio_state, received_frames)))

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        status, answer, _ = await loop.run_in_executor(
            None, lambda: http_request(
                "POST", AZURE_SDP,
                body=pc.localDescription.sdp.encode(),
                headers={"Authorization": f"Bearer {token}",
                         "Content-Type": "application/sdp"},
                timeout=40, insecure=args.insecure,
            )
        )
        r.check("SDP exchange -> HTTP 201", status == 201,
                f"HTTP {status}: {answer[:120]}")
        if status != 201:
            return 1
        await pc.setRemoteDescription(
            RTCSessionDescription(sdp=answer, type="answer"))

        try:
            await asyncio.wait_for(st["created"].wait(), timeout=30)
        except asyncio.TimeoutError:
            r.check("session.created received", False, "timeout")
            return 1
        r.check(
            "session.created received (STT off by default, server_vad preset)",
            st["default_stt"] is None and
            (isinstance(st["default_turn"], dict)
             and st["default_turn"].get("type") == "server_vad"),
            f"transcription={st['default_stt']} turn_detection={st['default_turn']}",
        )

        send_session_update(dc, enable_stt=not args.no_stt,
                            voice=args.voice, instructions=args.system_prompt)
        try:
            await asyncio.wait_for(st["updated"].wait(), timeout=15)
        except asyncio.TimeoutError:
            r.check("session.updated received", False, "timeout")
            return 1
        r.check(
            "session.updated received (STT opt-in applied)",
            isinstance(st["stt"], dict) and st["stt"].get("model") == "whisper-1",
            f"transcription={st['stt']}",
        )

        mic.enable()
        for turn in range(1, args.turns + 1):
            cue(f"TURN {turn}/{args.turns} — SPEAK NOW "
                f"(the mic is live; your turn ends after 500 ms of silence)")
            # Reset the per-turn counters; the final response is one more
            # than the number of function-call turns in this turn.
            st["responses_done"] = 0
            st["fn_calls"] = 0
            st["pending_fn"] = False
            st["user_text"] = ""
            st["assistant_text"] = ""
            st["last_transcript"] = ""
            st["transcript_done"] = asyncio.Event()
            deadline = time.time() + args.timeout
            timed_out = True
            watch = asyncio.ensure_future(uplink_watch(mic))
            while time.time() < deadline:
                if (st["responses_done"] >= st["fn_calls"] + 1
                        and not st["pending_fn"]):
                    timed_out = False
                    break
                await asyncio.sleep(0.2)
            watch.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await watch
            frames, seconds, peak = mic.stats()
            log(f"uplink total: {frames} frames ({seconds:.1f} s), peak "
                f"{peak}/32768" + (" — the uplink was silent" if peak == 0 else ""))
            if timed_out:
                r.check(f"turn {turn}: response.done", False,
                        f"timeout; errors={st['errors'][:2]}")
                break
            # response.done can overtake the transcript/audio events; give
            # them a moment before deciding the reply is empty.
            try:
                await asyncio.wait_for(st["transcript_done"].wait(), timeout=5.0)
            except asyncio.TimeoutError:
                pass
            if speaker is not None:
                await drain_reply(audio_state, speaker, args.drain,
                                  args.silence)
                speaker.finish()
                await speaker.drain(args.drain)
            r.check(f"turn {turn}: STT transcript", bool(st["user_text"]),
                    f"“{st['user_text'][:120]}”")
            r.check(
                f"turn {turn}: assistant reply",
                bool(st["last_transcript"] or st["assistant_text"]),
                f"“{(st['last_transcript'] or st['assistant_text'])[:120]}”",
            )
            if st["fn_calls"]:
                r.check(
                    f"turn {turn}: RAG tool call",
                    st["tool_status"] == 200 and st["tool_chunks"] > 0,
                    f"fn_calls={st['fn_calls']} HTTP {st['tool_status']} "
                    f"chunks={st['tool_chunks']}",
                )
            if st["errors"]:
                r.check(f"turn {turn}: no session errors", False,
                        json.dumps(st["errors"][:2])[:200])
            if turn < args.turns:
                mic.resume()
                await asyncio.sleep(0.2)

        r.check("spoken reply captured", audio_state["count"] > 0,
                f"{audio_state['count']} frames")
        if received_frames:
            save_wav(received_frames, args.out)
            r.check("reply saved", True,
                    f"{args.out} ({os.path.getsize(args.out)} B)")
        if st["errors"]:
            r.check("no session errors overall", False,
                    json.dumps(st["errors"][:2])[:200])
        else:
            r.check("no session errors overall", True)
        return 0
    except Exception as e:  # noqa: BLE001
        r.check("live session", False, f"{type(e).__name__}: {e}")
        return 1
    finally:
        for task in list(collect_tasks):
            task.cancel()
        for task in list(collect_tasks):
            with contextlib.suppress(asyncio.CancelledError):
                await task
        if mic is not None:
            mic.stop()
        if speaker is not None:
            speaker.close()
        if pc is not None:
            try:
                await pc.close()
            except Exception:  # noqa: BLE001
                pass


# ------------------------------------------------------------------ main

def main():
    global BASE, AZURE_SDP
    ap = argparse.ArgumentParser(
        description="Verify the GEODIS Voice Agent API (HTTP + WebRTC)."
    )
    ap.add_argument("--mode",
                    choices=["http", "tls", "token", "webrtc", "voice",
                             "ptt", "live", "all"],
                    default="http",
                    help="http = HTTP surface only (default); "
                         "tls = trust both hosts against the Mozilla bundle; "
                         "token = probe ephemeral token lifetime; "
                         "webrtc = WebRTC text turn; "
                         "voice = WebRTC spoken turn from a clip; "
                         "ptt = push-to-talk (turn_detection=null + commit); "
                         "live = interactive mic -> agent -> speaker; "
                         "all = HTTP + TLS + WebRTC text turn")
    ap.add_argument("--token-wait", type=float, default=60.0,
                    help="seconds to wait before re-probing the token "
                         "(--mode token; 0 skips the wait)")
    ap.add_argument("--base", default=BASE, help="GEODIS gateway base URL")
    ap.add_argument("--sdp-url", default=AZURE_SDP, help="Azure SDP exchange URL")
    ap.add_argument("--session", default=f"hermes-test-{uuid.uuid4().hex[:8]}",
                    help="chat_session_id (a fresh id is generated by default)")
    ap.add_argument("--question", default="What is geodisxx2026?",
                    help="text turn question (webrtc mode)")
    ap.add_argument("--clip", help="WAV or raw 48k mono s16le PCM for voice mode")
    ap.add_argument("--out", default=os.path.join(HERE, "geodis_reply.wav"),
                    help="where to save the spoken reply")
    ap.add_argument("--voice", default="coral",
                    help="assistant voice id (coral, martha, …)")
    ap.add_argument("--system-prompt", dest="system_prompt", default=None,
                    help="override the agent persona")
    ap.add_argument("--no-stt", action="store_true",
                    help="do not opt in to whisper-1 transcription")
    ap.add_argument("--no-stun", action="store_true", help="disable STUN")
    ap.add_argument("--stun", default="stun:stun.google.com:19302")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="seconds to wait for the final response")
    ap.add_argument("--insecure", action="store_true",
                    help="disable TLS verification (debugging only)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every DataChannel event")

    # live mic/speaker options
    ap.add_argument("--list-devices", action="store_true",
                    help="list sounddevice input/output devices and exit")
    ap.add_argument("--mic-device", type=device_arg, default=None,
                    help="microphone device index or name substring")
    ap.add_argument("--speaker-device", type=device_arg, default=None,
                    help="speaker device index or name substring")
    ap.add_argument("--no-play", action="store_true",
                    help="do not play the reply through the speaker")
    ap.add_argument("--playback", choices=PLAYBACK_MODES, default="buffer",
                    help="speaker playback mode (default: buffer)")
    ap.add_argument("--cushion", type=float, default=0.6,
                    help="seconds to buffer before adaptive playback (default 0.6)")
    ap.add_argument("--drain", type=float, default=15.0,
                    help="hard cap on seconds to wait for the reply (default 15)")
    ap.add_argument("--silence", type=float, default=2.0,
                    help="seconds of silence before the reply is done (default 2)")
    ap.add_argument("--turns", type=int, default=3,
                    help="number of live turns (default 3; Ctrl+C stops early)")
    args = ap.parse_args()

    BASE = args.base
    AZURE_SDP = args.sdp_url

    if args.list_devices:
        if sd is None:
            print(f"sounddevice is not installed ({_SD_IMPORT_ERROR})")
            return 2
        print(sd.query_devices())
        print(f"default input device:  {sd.default.device[0]}")
        print(f"default output device: {sd.default.device[1]}")
        return 0

    print("GEODIS Voice Agent API test")
    print(f"  base:   {BASE}")
    print(f"  sdp:    {AZURE_SDP}")
    print(f"  mode:   {args.mode}  session: {args.session}")

    r = Report()
    rc = 0
    try:
        if args.mode in ("http", "all"):
            run_http_checks(r, args)
        if args.mode in ("tls", "all"):
            run_tls_checks(r, args)
        if args.mode == "token":
            run_token_checks(r, args)
        if args.mode in ("webrtc", "voice", "ptt", "all"):
            rc = asyncio.run(run_webrtc(
                r, args,
                voice=(args.mode in ("voice", "ptt")),
                ptt=(args.mode == "ptt")))
            if rc == 2:
                return 2
        if args.mode == "live":
            rc = asyncio.run(run_live(r, args))
            if rc == 2:
                return 2
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130

    rc = r.summary() if rc == 0 else rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
