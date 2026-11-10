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

This is the M0 spike from `examples/14_geodis_voice_chat/README.md`: it
captures the exact event shapes the ring's `ring_geodis` module has to parse.

Usage:
    # HTTP surface only (stdlib only — no venv needed)
    python3 scripts/test_geodis_api.py
    python3 scripts/test_geodis_api.py --mode http

    # full WebRTC session, deterministic text turn (needs scripts/.venv)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode webrtc

    # WebRTC session with a spoken question (file input, no microphone)
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode voice
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode voice --clip /tmp/q.wav

    # everything
    scripts/.venv/bin/python scripts/test_geodis_api.py --mode all -v

Exit codes:
    0  every check passed
    1  one or more checks failed
    2  setup error (aiortc missing, clip unreadable, …)

The endpoint is unauthenticated and metered: every `--mode webrtc` / `voice`
run starts a real Azure Realtime session. Do not run it in a loop.
"""
import argparse
import asyncio
import json
import os
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import wave

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

def http_request(method, url, body=None, headers=None, timeout=30, insecure=False):
    """Return (status, body_text, headers). status is -1 on transport error."""
    ctx = ssl._create_unverified_context() if insecure else ssl.create_default_context()
    data = None
    if body is not None:
        data = bytes(body) if isinstance(body, (bytes, bytearray)) else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)
    except Exception as e:  # noqa: BLE001 - report the transport error verbatim
        return -1, f"{type(e).__name__}: {e}", {}


def get_token(body=None, timeout=30, insecure=False):
    status, text, _ = http_request(
        "POST", f"{BASE}/token", body=body if body is not None else {},
        headers={"Content-Type": "application/json"}, timeout=timeout,
        insecure=insecure,
    )
    if status != 200:
        return None, None, status, text
    try:
        d = json.loads(text)
        return d.get("token"), d.get("session_id"), status, text
    except Exception as e:  # noqa: BLE001
        return None, None, status, f"parse error: {e}: {text[:200]}"


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


def send_session_update(dc, enable_stt=True, voice="coral", instructions=None):
    session = {"type": "realtime"}
    audio = {}
    if enable_stt:
        # STT is OFF by default; this is the opt-in the ring must send.
        audio["input"] = {"transcription": {"model": "whisper-1"}}
    if voice:
        audio["output"] = {"voice": voice}
    if audio:
        session["audio"] = audio
    # NOTE: do NOT send `turn_detection` — the GA endpoint rejects it with
    # `unknown_parameter` (the server configures server_vad itself, and it is
    # visible in session.created). `modalities` is likewise read-only.
    if instructions:
        session["instructions"] = instructions
    send_json(dc, {"type": "session.update", "session": session})


async def run_webrtc(r, args, voice):
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

    r.section("WebRTC session" + (" (voice)" if voice else " (text)"))

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

    # 6. SDP exchange
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
        await pc.close()
        return 1
    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer, type="answer"))

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
                        voice=args.voice, instructions=args.system_prompt)
    try:
        await asyncio.wait_for(st["updated"].wait(), timeout=15)
    except asyncio.TimeoutError:
        r.check("session.updated received", False, "timeout")
        await pc.close()
        return 1
    r.check(
        "session.updated received (STT opt-in applied)",
        isinstance(st["stt"], dict) and st["stt"].get("model") == "whisper-1",
        f"transcription={st['stt']}",
    )

    if recorder:
        await recorder.start()

    # 8. send the turn
    if voice:
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


# ------------------------------------------------------------------ main

def main():
    global BASE, AZURE_SDP
    ap = argparse.ArgumentParser(
        description="Verify the GEODIS Voice Agent API (HTTP + WebRTC)."
    )
    ap.add_argument("--mode", choices=["http", "webrtc", "voice", "all"],
                    default="http",
                    help="http = HTTP surface only (default); "
                         "webrtc = WebRTC text turn; "
                         "voice = WebRTC spoken turn; all = everything")
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
    args = ap.parse_args()

    BASE = args.base
    AZURE_SDP = args.sdp_url

    print("GEODIS Voice Agent API test")
    print(f"  base:   {BASE}")
    print(f"  sdp:    {AZURE_SDP}")
    print(f"  mode:   {args.mode}  session: {args.session}")

    r = Report()
    rc = 0
    try:
        if args.mode in ("http", "all"):
            run_http_checks(r, args)
        if args.mode in ("webrtc", "voice", "all"):
            rc = asyncio.run(run_webrtc(r, args, voice=(args.mode == "voice")))
            if rc == 2:
                return 2
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130

    rc = r.summary() if rc == 0 else rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
