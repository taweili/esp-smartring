#!/usr/bin/env python3
"""
test_qianwen_realtime.py — host-side test of the Qianwen realtime WebRTC API.

This is the same interaction the ring's qianwenai-button performs, but from a
PC, with verbose logging and the model reply saved to a WAV file:

  1. Build an SDP offer (Opus audio track + a "txt" SCTP data channel).
  2. POST it to  https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=...
     with  Authorization: Bearer <QIANWEN_API_KEY>.
  3. Apply the answer, wait for ICE / DTLS / SRTP.
  4. Send session.update, wait for session.updated.
  5. Stream the test clip ("the capital of China") as 48 kHz / 16-bit Opus.
  6. Collect the model's spoken reply and the data-channel transcript.
  7. Write the reply to a WAV file and print the transcript.

Usage:
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py --out reply.wav
    scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip my_clip.pcm

The API key is read from --api-key, $QIANWEN_API_KEY, or ./secrets.local.
The clip defaults to examples/qianwen-realtime/assets/test_clip.pcm
(48 kHz / 16-bit / mono s16le).

Requirements: scripts/.venv (see scripts/README.md).
"""
import argparse
import asyncio
import fractions
import json
import os
import sys
import time
import wave

import aiohttp
from av import AudioFrame
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import AudioStreamTrack, MediaStreamError

ENDPOINT = "https://maas.qianwenaiapi.com/api/v1/webrtc/realtime"
MODEL = "qwen-audio-3.1-realtime-flash"

VOICE = "longanqian_v3.1"
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
                if line.startswith("QIANWEN_API_KEY="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
    return None


def default_clip_path():
    return os.path.join(
        repo_root(), "examples", "qianwen-realtime", "assets", "test_clip.pcm"
    )


class ClipTrack(AudioStreamTrack):
    """An audio track that streams a 48 kHz / 16-bit mono PCM buffer."""

    kind = "audio"

    def __init__(self, pcm, sample_rate=48000, frame_samples=960):
        super().__init__()
        self.pcm = pcm
        self.sample_rate = sample_rate
        self.frame_samples = frame_samples
        self.idx = 0
        self._start = None
        self._timestamp = 0

    async def recv(self):
        if self.readyState != "live":
            raise MediaStreamError

        # Pace the frames to real time, like a live microphone.
        if self._start is None:
            self._start = time.time()
        else:
            wait = self._start + (self._timestamp / self.sample_rate) - time.time()
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


def save_wav(frames, path, sample_rate=48000):
    """Write a list of av.AudioFrame (mono, 48 kHz) to a 16-bit WAV."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        for frame in frames:
            if frame.sample_rate != sample_rate:
                log(f"  (frame sample rate {frame.sample_rate}, expected {sample_rate})")
            data = frame.to_ndarray()
            if data.ndim == 2:
                data = data[0]
            w.writeframes(data.astype("int16").tobytes())


async def main(args):
    api_key = load_api_key(args)
    if not api_key:
        log("no API key: pass --api-key, set QIANWEN_API_KEY, or add it to secrets.local")
        return 1

    clip_path = args.clip or default_clip_path()
    if not os.path.exists(clip_path):
        log(f"clip not found: {clip_path}")
        return 1
    with open(clip_path, "rb") as f:
        pcm = f.read()
    clip_seconds = len(pcm) / 2 / 48000
    log(f"clip: {clip_path} ({len(pcm)} bytes, {clip_seconds:.2f} s)")

    # State machine events.
    session_ready = asyncio.Event()
    response_done = asyncio.Event()
    got_error = asyncio.Event()
    transcript_parts = []

    pc = RTCPeerConnection()

    @pc.on("iceconnectionstatechange")
    def on_ice_state():
        log(f"ice: {pc.iceConnectionState}")

    @pc.on("connectionstatechange")
    def on_conn_state():
        log(f"connection: {pc.connectionState}")

    @pc.on("datachannel")
    def on_datachannel(channel):
        log(f"data channel (remote): {channel.label}")
        channel.on("message", lambda m: handle_message(m, channel, session_ready,
                                                      response_done, got_error,
                                                      transcript_parts))

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
            asyncio.ensure_future(collect_audio(track, received_frames))

    # Send our clip.
    pc.addTrack(ClipTrack(pcm))

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
    url = f"{ENDPOINT}?model={MODEL}"
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

    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer_sdp, type="answer"))
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

    # Wait for the session to be created.
    log("waiting for session.created...")
    try:
        await asyncio.wait_for(session_ready.wait(), timeout=20)
    except asyncio.TimeoutError:
        log("timed out waiting for session.created")
        await pc.close()
        return 1
    log("session created")

    # The clip track is already streaming into the RTP sender. Wait for the
    # model's turn to complete.
    log("waiting for response.done...")
    try:
        await asyncio.wait_for(response_done.wait(), timeout=args.timeout)
    except asyncio.TimeoutError:
        log(f"timed out after {args.timeout} s (no response.done)")

    # Give the audio path a moment to drain.
    await asyncio.sleep(0.5)

    transcript = "".join(transcript_parts)
    log(f"transcript: {transcript!r}")
    log(f"received {len(received_frames)} audio frames")

    out_path = args.out or os.path.join(repo_root(), "scripts", "reply.wav")
    if received_frames:
        save_wav(received_frames, out_path)
        log(f"reply saved: {out_path}")
    else:
        log("no reply audio received")

    await pc.close()
    return 0


def handle_message(message, channel, session_ready, response_done, got_error,
                    transcript_parts):
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
                "voice": VOICE,
                "turn_detection": TURN_DETECTION,
            },
        }))
        log("session.update sent")
    elif etype == "session.updated":
        log("session.updated received")
    elif etype == "response.done":
        response_done.set()
    elif etype == "error":
        got_error.set()
    elif etype in ("response.audio_transcript.delta", "response.text.delta"):
        transcript_parts.append(ev.get("delta", ""))


async def collect_audio(track, frames):
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
        frames.append(frame)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test the Qianwen realtime WebRTC API")
    parser.add_argument("--api-key", help="Qianwen API key")
    parser.add_argument("--clip", help="48 kHz / 16-bit mono s16le PCM clip")
    parser.add_argument("--out", help="output WAV path for the model reply")
    parser.add_argument("--timeout", type=int, default=30,
                        help="seconds to wait for response.done (default 30)")
    parser.add_argument("--dump-sdp", help="write the local SDP offer to this file")
    args = parser.parse_args()
    sys.exit(asyncio.run(main(args)))
