#!/usr/bin/env python3
"""
test_qianwen_realtime.py — host-side test of the Qianwen realtime WebRTC API.

This is the same interaction the ring's qianwenai-button performs, but from a
PC, with verbose logging. By default it uses the real microphone and speaker:
speak to the model and hear its spoken reply. A known clip can be streamed
instead for a deterministic test.

  1. Build an SDP offer (Opus audio track + a "txt" SCTP data channel).
  2. POST it to  https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=...
     with  Authorization: Bearer <QIANWEN_API_KEY>.
  3. Apply the answer, wait for ICE / DTLS / SRTP.
  4. Send session.update, wait for session.updated.
  5. Stream live microphone audio (or --clip) as 48 kHz / 16-bit Opus.
  6. Collect the model's spoken reply, play it through the speaker, save it
     to a WAV file, and print the data-channel transcript.

Usage:
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip my_clip.pcm
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py --no-play --out reply.wav
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py --list-devices

The API key is read from --api-key, $QIANWEN_API_KEY, or ./secrets.local.
Without --clip the default input device is used. A known clip (e.g.
examples/qianwen-realtime/assets/test_clip.pcm, 48 kHz / 16-bit / mono s16le,
or any WAV/MP3 that PyAV can decode) can be streamed with --clip for a
deterministic test.

The reply is played back in one of three modes (--playback): `buffer`
(the default) waits for the whole reply and plays it at the sound card's
rate, `live` streams as the frames arrive, and `adaptive` starts after
--cushion seconds and re-cushions after a stall. `buffer` exists because a
player fed at the delivery rate stretches the speech — g-buddy measured a
1.9 s answer arriving over 4.8 s and played back at ~40% speed.

Requirements: scripts/.venv (see scripts/README.md).
"""
import argparse
import asyncio
import ctypes
import fractions
import json
import os
import sys
import threading
import time
import wave
from array import array

import aiohttp
import av
from av import AudioFrame, AudioResampler
from aiortc import (RTCIceServer, RTCPeerConnection, RTCConfiguration,
                    RTCSessionDescription)
from aiortc.mediastreams import AudioStreamTrack, MediaStreamError

try:
    import sounddevice as sd
except ImportError as exc:  # only needed for live mic / speaker
    sd = None
    _SD_IMPORT_ERROR = exc

ENDPOINT = "https://maas.qianwenaiapi.com/api/v1/webrtc/realtime"
MODEL = "qwen-audio-3.1-realtime-flash"

SAMPLE_RATE = 48000
FRAME_SAMPLES = 960  # 20 ms

VOICE = "longanqian_v3.1"
STUN_SERVER = "stun:stun.l.google.com:19302"
PLAYBACK_MODES = ("buffer", "live", "adaptive")
TURN_DETECTION = {
    "type": "server_vad",
    "threshold": 0.5,
    "silence_duration_ms": 800,
}


def log(msg):
    print(f"[qianwen-rt] {msg}", flush=True)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_api_key(args):
    if args.api_key:
        return args.api_key
    if os.environ.get("QIANWEN_API_KEY"):
        return os.environ["QIANWEN_API_KEY"]
    secrets = os.path.join(repo_root(), "secrets.local")
    if os.path.exists(secrets):
        with open(secrets, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("export "):
                    line = line[len("export "):].strip()
                if line.startswith("QIANWEN_API_KEY="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
    return None


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

    def enable(self):
        """Start producing frames (call after session.updated)."""
        self._enabled.set()

    async def _wait_enabled(self):
        if self.readyState != "live":
            raise MediaStreamError
        await self._enabled.wait()
        if self.readyState != "live":
            raise MediaStreamError

    def stop(self):
        # Unblock a recv() that is waiting for enable().
        self._enabled.set()
        super().stop()


class ClipTrack(GatedAudioTrack):
    """An audio track that streams a 48 kHz / 16-bit mono PCM buffer."""

    def __init__(self, pcm, sample_rate=SAMPLE_RATE, frame_samples=FRAME_SAMPLES):
        super().__init__()
        self.pcm = pcm
        self.sample_rate = sample_rate
        self.frame_samples = frame_samples
        self.idx = 0
        self._start = None
        self._timestamp = 0

    async def recv(self):
        await self._wait_enabled()

        # Pace the frames to real time, like a live microphone.
        if self._start is None:
            self._start = time.monotonic()
        else:
            wait = self._start + (self._timestamp / self.sample_rate) - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)

        start = self.idx * self.frame_samples * 2
        if start < len(self.pcm):
            data = self.pcm[start:start + self.frame_samples * 2]
            if len(data) < self.frame_samples * 2:
                data += b"\x00" * (self.frame_samples * 2 - len(data))
            self.idx += 1
        else:
            # End of clip: keep the track alive with silence.
            data = b"\x00" * (self.frame_samples * 2)

        frame = AudioFrame(format="s16", layout="mono", samples=self.frame_samples)
        frame.planes[0].update(data)
        frame.pts = self._timestamp
        frame.sample_rate = self.sample_rate
        frame.time_base = fractions.Fraction(1, self.sample_rate)
        self._timestamp += self.frame_samples
        return frame


class MicTrack(GatedAudioTrack):
    """An audio track that streams a live input device (the microphone)."""

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
        # The PortAudio thread and the event loop both touch the buffer.
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

    async def recv(self):
        await self._wait_enabled()

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
        return frame

    def stop(self):
        if not self._closed:
            self._closed = True
            try:
                self._stream.stop()
                self._stream.close()
            except Exception as e:
                log(f"mic close error: {e}")
        super().stop()


class Speaker:
    """Plays 48 kHz / 16-bit mono PCM through an output device.

    g-buddy's lesson (`gbuddy/rtc.py` `LivePlayer`): a raw player fed as the
    frames arrive plays at the *delivery* rate, so a reply whose frames trickle
    in comes out slow (measured ~40% speed — "the speaker at half speed").
    `mode="buffer"` waits for the whole reply, then plays it back-to-back at
    the card's rate; `mode="live"` streams as the frames arrive;
    `mode="adaptive"` starts once `cushion` seconds are queued and re-cushions
    after a stall.
    """

    def __init__(self, sample_rate=SAMPLE_RATE, frame_samples=FRAME_SAMPLES,
                 device=None, mode="buffer", cushion=0.6):
        self.sample_rate = sample_rate
        self.frame_samples = frame_samples
        self._frame_bytes = frame_samples * 2
        self.mode = mode
        self.cushion = max(0.0, float(cushion or 0.0))
        self._cushion_bytes = int(self.cushion * sample_rate * 2)
        self._buf = bytearray()         # unplayed PCM
        self._pos = 0                   # read offset into _buf
        self._lock = threading.Lock()
        self._max_bytes = self._frame_bytes * 200   # ~4 s live backlog cap
        self._go = threading.Event()    # buffer mode: released by finish()
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
            # Reclaim the consumed prefix, but not on every block: a memmove
            # per 20 ms over a whole buffered reply is O(n^2).
            if self._pos == len(self._buf):
                self._buf.clear()
                self._pos = 0
            elif self._pos > 1 << 16:
                del self._buf[:self._pos]
                self._pos = 0

    def play(self, pcm):
        """Buffer mono s16 PCM for playback."""
        if self._closed or not pcm:
            return
        with self._lock:
            self._buf.extend(pcm)
            if self.mode != "buffer":
                # Drop the oldest unplayed audio if the callback falls behind.
                if len(self._buf) - self._pos > self._max_bytes:
                    del self._buf[:self._pos]
                    self._pos = 0
                    if len(self._buf) > self._max_bytes:
                        del self._buf[:len(self._buf) - self._max_bytes]

    def finish(self):
        """No more input: release a buffered reply so it starts playing."""
        if self._closed:
            return
        self._finished = True
        self._go.set()

    def empty(self):
        """True when nothing is waiting to be played.

        In buffer mode this is True until `finish()` releases the reply, so a
        drain that runs before playback has started is not mistaken for a
        finished one.
        """
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

    g-buddy's `frame_bytes` lesson: PyAV's AudioPlane has no `to_bytes()`
    in the wheels it uses, and numpy is an unnecessary dependency on the
    host too, so read the plane buffer through ctypes and average the
    channels by hand. The Opus decoder outputs interleaved stereo, which
    must be deinterleaved rather than written out as-is (that doubles the
    duration and halves the pitch).
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
    # Interleaved: samples of all channels in one plane.
    values = array("h", ctypes.string_at(
        frame.planes[0].buffer_ptr, frame.samples * channels * 2))
    if sys.byteorder == "big":
        values.byteswap()
    out = array("h", bytes(frame.samples * 2))
    for i in range(0, len(values), channels):
        out[i // channels] = sum(values[i:i + channels]) // channels
    return out.tobytes()


def load_clip(path):
    """Read a clip as 48 kHz / 16-bit mono s16le PCM.

    A raw PCM file (no header) is read as-is; anything else (WAV, MP3, ...)
    is decoded and resampled with PyAV — g-buddy's `iter_wav_chunks` does
    the same, so the same clip can be handed to either client.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF":
        return data
    container = av.open(path)
    resampler = AudioResampler(format="s16", layout="mono",
                               rate=SAMPLE_RATE)
    out = bytearray()
    try:
        for frame in container.decode(audio=0):
            for resampled in resampler.resample(frame) or []:
                out += frame_to_pcm(resampled)
        for resampled in resampler.resample(None) or []:
            out += frame_to_pcm(resampled)
    finally:
        container.close()
    return bytes(out)


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
            # Flush the resampler so no tail samples are lost.
            for out in resampler.resample(None) or []:
                w.writeframes(frame_to_pcm(out))


async def drain_reply(state, speaker, max_seconds, silence_seconds=2.0):
    """Wait for the reply to stop arriving and finish playing.

    The reply is done when no frame has arrived for `silence_seconds` and the
    speaker has no queued audio left; `max_seconds` is a hard cap. In buffer
    playback mode the reply has not started playing yet, so this only waits
    for the audio to stop arriving — `finish()` releases it afterwards.
    """
    if state["count"] == 0:
        await asyncio.sleep(0.5)
        return
    deadline = time.monotonic() + max_seconds
    while time.monotonic() < deadline:
        quiet = time.monotonic() - state["last"] >= silence_seconds
        playing = speaker is not None and not speaker.empty()
        if quiet and not playing:
            return
        await asyncio.sleep(0.1)
    log(f"reply still arriving after {max_seconds:.0f} s — cutting off")


async def wait_first(events, timeout):
    """Wait until any of `events` is set. Returns True if `timeout` expired."""
    tasks = [asyncio.ensure_future(event.wait()) for event in events]
    done, pending = await asyncio.wait(tasks, timeout=timeout,
                                       return_when=asyncio.FIRST_COMPLETED)
    for task in pending:
        task.cancel()
    if pending:
        await asyncio.gather(*pending, return_exceptions=True)
    return not done


def log_errors(errors):
    for message in errors:
        log(f"server error: {message}")


async def main(args):
    if args.list_devices:
        if sd is None:
            log(f"sounddevice is not installed ({_SD_IMPORT_ERROR})")
            return 1
        print(sd.query_devices())
        print(f"default input device:  {sd.default.device[0]}")
        print(f"default output device: {sd.default.device[1]}")
        return 0

    use_mic = not args.clip
    if (use_mic or not args.no_play) and sd is None:
        log("sounddevice is required for the microphone/speaker: "
            "scripts/.venv/bin/pip install sounddevice")
        return 1

    api_key = load_api_key(args)
    if not api_key:
        log("no API key: pass --api-key, set QIANWEN_API_KEY, or add it to secrets.local")
        return 1

    # Input track: the live microphone by default, or a PCM clip.
    mic_track = None
    speaker = None
    try:
        if args.clip:
            clip_path = args.clip
            if not os.path.exists(clip_path):
                log(f"clip not found: {clip_path}")
                return 1
            pcm = load_clip(clip_path)
            log(f"input: clip {clip_path} "
                f"({len(pcm)} bytes, {len(pcm) / 2 / SAMPLE_RATE:.2f} s)")
            input_track = ClipTrack(pcm)
        else:
            mic_track = MicTrack(device=args.mic_device)
            input_track = mic_track
            log("input: microphone — speak now")

        speaker = None if args.no_play else Speaker(
            device=args.speaker_device, mode=args.playback,
            cushion=args.cushion)
        log(f"output: {args.playback} playback" if speaker
            else "output: muted (--no-play)")
    except Exception:
        # Don't leak the microphone if the speaker (or the clip) fails.
        if mic_track is not None:
            mic_track.stop()
        if speaker is not None:
            speaker.close()
        raise

    # State machine events.
    session_ready = asyncio.Event()
    session_updated = asyncio.Event()
    response_done = asyncio.Event()
    got_error = asyncio.Event()
    transcript_parts = []
    user_transcript_parts = []
    error_messages = []
    audio_state = {"count": 0, "last": 0.0}
    collect_tasks = []

    ice_servers = [] if args.no_stun else [RTCIceServer(urls=args.stun)]
    pc = RTCPeerConnection(RTCConfiguration(iceServers=ice_servers))

    @pc.on("iceconnectionstatechange")
    def on_ice_state():
        log(f"ice: {pc.iceConnectionState}")

    @pc.on("connectionstatechange")
    def on_conn_state():
        log(f"connection: {pc.connectionState}")

    @pc.on("datachannel")
    def on_datachannel(channel):
        log(f"data channel (remote): {channel.label}")
        channel.on("message", lambda m: handle_message(
            m, channel, session_ready, session_updated, response_done,
            got_error, transcript_parts, user_transcript_parts,
            error_messages, args.voice))

    # The local channel is not needed for this test: the server opens its own
    # "txt" channel and it is bidirectional, so session.update is sent on it
    # (the same thing ring_net does).
    pc.createDataChannel("txt")

    # Incoming audio from the model.
    received_frames = []

    @pc.on("track")
    def on_track(track):
        log(f"track: {track.kind}")
        if track.kind == "audio":
            collect_tasks.append(asyncio.ensure_future(
                collect_audio(track, received_frames, speaker, mic_track,
                              audio_state)))

    # Start streaming our input. Everything below is wrapped so the PC and
    # the audio devices are always torn down, including on error and Ctrl+C.
    try:
        pc.addTrack(input_track)

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        log("offer created")
        if args.dump_sdp:
            with open(args.dump_sdp, "w", encoding="utf-8") as f:
                f.write(pc.localDescription.sdp)
            log(f"offer SDP written to {args.dump_sdp}")

        headers = {
            "Content-Type": "application/sdp",
            "Authorization": f"Bearer {api_key}",
        }
        url = f"{ENDPOINT}?model={args.model}"
        log(f"POST {url}")
        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(url, data=pc.localDescription.sdp,
                                        headers=headers, timeout=30) as resp:
                    answer_sdp = await resp.text()
                    log(f"HTTP {resp.status}, answer {len(answer_sdp)} bytes")
                    if resp.status != 200:
                        log(answer_sdp[:500])
                        return 1
        except aiohttp.ClientError as e:
            log(f"HTTP error: {e}")
            return 1

        await pc.setRemoteDescription(
            RTCSessionDescription(sdp=answer_sdp, type="answer"))
        log("remote answer applied")

        # Wait for the connection to come up.
        log("waiting for connection...")
        for _ in range(600):
            if pc.connectionState in ("connected", "failed", "closed"):
                break
            await asyncio.sleep(0.1)
        log(f"connection state: {pc.connectionState}, ice: {pc.iceConnectionState}")
        if pc.connectionState != "connected":
            log("connection failed")
            return 1

        # Wait for the session to be created (or for the server to error).
        log("waiting for session.created...")
        if await wait_first([session_ready, got_error], timeout=20):
            log("timed out waiting for session.created")
            return 1
        if not session_ready.is_set():
            log_errors(error_messages)
            log("session failed")
            return 1
        log("session created")

        # Wait for session.updated before sending any audio: the server
        # rejects a turn_detection update once it has started processing
        # audio, so the input track stays gated until the session is ready.
        log("waiting for session.updated...")
        if await wait_first([session_updated, got_error], timeout=20):
            log("timed out waiting for session.updated")
            return 1
        if not session_updated.is_set():
            log_errors(error_messages)
            log("session configuration failed")
            return 1
        input_track.enable()
        log("input enabled")

        # The input track is already streaming into the RTP sender. Wait for
        # the model's turn to complete (or for the server to error).
        log("waiting for response.done...")
        if await wait_first([response_done, got_error], timeout=args.timeout):
            log(f"timed out after {args.timeout} s (no response.done)")
        elif not response_done.is_set():
            log_errors(error_messages)
            log("session error before response.done")
            return 1
        elif got_error.is_set():
            log_errors(error_messages)
            log("server reported an error; keeping any audio received")

        # Stop sending microphone audio as soon as the reply starts
        # (collect_audio does this) and let the reply finish arriving and
        # playing out.
        if mic_track is not None:
            mic_track.stop()
        await drain_reply(audio_state, speaker, args.drain, args.silence)
        if speaker is not None:
            # Buffer mode has not played anything yet: release the reply, then
            # wait for the card to finish. Live/adaptive already played.
            speaker.finish()
            await speaker.drain(args.drain)

        transcript = "".join(transcript_parts)
        user_text = "".join(user_transcript_parts)
        if user_text:
            log(f"you: {user_text!r}")
        log(f"transcript: {transcript!r}")
        log(f"received {len(received_frames)} audio frames")

        out_path = args.out or os.path.join(repo_root(), "scripts", "reply.wav")
        if received_frames:
            save_wav(received_frames, out_path)
            log(f"reply saved: {out_path}")
        else:
            log("no reply audio received")

        return 0
    finally:
        await teardown(pc, mic_track, speaker, collect_tasks)


async def teardown(pc, mic_track, speaker, collect_tasks=()):
    """Stop the audio devices and close the peer connection."""
    for task in collect_tasks:
        task.cancel()
    for task in collect_tasks:
        try:
            await task
        except asyncio.CancelledError:
            pass
    if mic_track is not None:
        mic_track.stop()
    if speaker is not None:
        speaker.close()
    try:
        await pc.close()
    except Exception as e:
        log(f"pc close error: {e}")


def handle_message(message, channel, session_ready, session_updated,
                    response_done, got_error, transcript_parts,
                    user_transcript_parts, error_messages, voice):
    if isinstance(message, bytes):
        message = message.decode("utf-8", "replace")
    log(f"dc: {message[:300]}")
    try:
        ev = json.loads(message)
    except json.JSONDecodeError:
        return
    etype = ev.get("type", "")
    if etype == "session.created":
        session_ready.set()
        # Send the session config on the server's channel (bidirectional), like
        # ring_net does.
        channel.send(json.dumps({
            "type": "session.update",
            "session": {
                "modalities": ["text", "audio"],
                "voice": voice,
                "turn_detection": TURN_DETECTION,
            },
        }))
        log("session.update sent")
    elif etype == "session.updated":
        session_updated.set()
        log("session.updated received")
    elif etype == "response.done":
        # `completed` is the normal end; `cancelled` is a barge-in and
        # `failed` carries the reason (g-buddy maps all three explicitly).
        status = (ev.get("response") or {}).get("status") or "completed"
        if status == "failed":
            details = (ev.get("response") or {}).get("status_details") or {}
            error_messages.append(json.dumps(details)[:300])
            got_error.set()
        response_done.set()
    elif etype == "error":
        error_messages.append(message)
        got_error.set()
    elif etype == "conversation.item.input_audio_transcription.completed":
        text = (ev.get("transcript") or "").strip()
        if text:
            user_transcript_parts.append(text)
    elif etype == "input_audio_buffer.speech_started":
        log("server VAD: speech started")
    elif etype in ("response.audio_transcript.delta", "response.text.delta"):
        transcript_parts.append(ev.get("delta", ""))


async def collect_audio(track, frames, speaker, mic_track, state):
    logged = False
    while True:
        try:
            frame = await track.recv()
        except (MediaStreamError, asyncio.TimeoutError):
            break
        except Exception as e:
            log(f"audio collect error: {e}")
            break
        if not logged:
            log(f"audio frame: format={frame.format.name} "
                f"layout={frame.layout.name} rate={frame.sample_rate}")
            logged = True
            if mic_track is not None:
                # The reply has started: stop the microphone so the model does
                # not hear itself through the speaker.
                mic_track.stop()
        state["count"] += 1
        state["last"] = time.monotonic()
        if speaker is not None:
            speaker.play(frame_to_pcm(frame))
        frames.append(frame)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test the Qianwen realtime WebRTC API")
    parser.add_argument("--api-key", help="Qianwen API key")
    parser.add_argument("--clip", metavar="PATH",
                        help="stream this clip instead of the microphone "
                             "(raw 48 kHz / 16-bit mono s16le PCM, or any "
                             "av-decodable audio file such as WAV)")
    parser.add_argument("--out", help="output WAV path for the model reply")
    parser.add_argument("--voice", default=VOICE,
                        help=f"realtime voice (default: {VOICE})")
    parser.add_argument("--model", default=MODEL,
                        help=f"realtime model (default: {MODEL})")
    parser.add_argument("--timeout", type=int, default=60,
                        help="seconds to wait for response.done (default 60)")
    parser.add_argument("--drain", type=float, default=15.0,
                        help="hard cap on seconds to wait for the reply to "
                             "finish playing (default 15)")
    parser.add_argument("--silence", type=float, default=2.0,
                        help="seconds of silence before the reply is "
                             "considered done (default 2)")
    parser.add_argument("--mic-device",
                        help="input device index or name substring "
                             "(default: system default)")
    parser.add_argument("--speaker-device",
                        help="output device index or name substring "
                             "(default: system default)")
    parser.add_argument("--no-play", action="store_true",
                        help="do not play the reply through the speaker")
    parser.add_argument("--playback", choices=PLAYBACK_MODES, default="buffer",
                        help="how to play the reply: buffer waits for it to "
                             "finish arriving and plays at the card's rate "
                             "(default), live streams as frames arrive, "
                             "adaptive starts after --cushion and re-cushions")
    parser.add_argument("--cushion", type=float, default=0.6,
                        help="seconds to buffer before adaptive playback "
                             "(default 0.6)")
    parser.add_argument("--stun", default=STUN_SERVER,
                        help=f"STUN server URL (default: {STUN_SERVER})")
    parser.add_argument("--no-stun", action="store_true",
                        help="do not use a STUN server")
    parser.add_argument("--list-devices", action="store_true",
                        help="list audio devices and exit")
    parser.add_argument("--dump-sdp", help="write the local SDP offer to this file")
    args = parser.parse_args()
    sys.exit(asyncio.run(main(args)))
