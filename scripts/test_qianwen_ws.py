#!/usr/bin/env python3
"""
test_qianwen_ws.py — Host-side probe for the Qianwen realtime WebSocket API.

Connects to wss://maas.qianwenaiapi.com/api-ws/v1/realtime, sends
session.update (with configurable turn_detection), streams a PCM clip
as input_audio_buffer.append, and saves the server's reply.

Usage:
    scripts/.venv/bin/python scripts/test_qianwen_ws.py
    scripts/.venv/bin/python scripts/test_qianwen_ws.py --mode smart_turn_v2
    scripts/.venv/bin/python scripts/test_qianwen_ws.py --mode null  # PTT

The API key is read from --api-key, $QIANWEN_API_KEY, or ./secrets.local.

Requirements: aiohttp (already in scripts/.venv).
"""
import argparse
import asyncio
import base64
import json
import os
import struct
import sys
import time
import wave

if not hasattr(os, 'chdir') or 'aiohttp' not in sys.modules:
    import aiohttp

# Try to import av for resampling (optional, for the test clip)
try:
    import av
    from av import AudioResampler
except ImportError:
    av = None

ENDPOINT = "wss://maas.qianwenaiapi.com/api-ws/v1/realtime"
MODEL = "qwen-audio-3.1-realtime-flash"
SAMPLE_RATE = 48000  # ring_audio rate
CLIP_PATH = os.path.join(os.path.dirname(__file__), "assets", "test_clip.pcm")

def load_secrets():
    """Load API key from environment or secrets.local."""
    key = os.environ.get("QIANWEN_API_KEY", "")
    if not key:
        local = os.path.join(os.path.dirname(__file__), "..", "secrets.local")
        if os.path.exists(local):
            with open(local) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("export "):
                        line = line[len("export "):]
                    if line.startswith("QIANWEN_API_KEY="):
                        key = line.split("=", 1)[1].strip("\"'")
    return key

def format_log(msg):
    ts = time.strftime("%H:%M:%S")
    return f"[{ts}] {msg}"

class Client:
    def __init__(self, api_key, mode="null"):
        self.api_key = api_key
        self.mode = mode
        self.ws = None
        self.session = None
        self.reply_wav = None
        self.reply_pcm = bytearray()
        self.reply_transcript = ""
        self.user_transcript = ""
        self.event_count = 0
        self.done = None
        self.configured = None

    async def connect(self):
        self.session = aiohttp.ClientSession()
        url = f"{ENDPOINT}?model={MODEL}"
        headers = {"Authorization": f"Bearer {self.api_key}"}

        print(format_log(f"Connecting to {url}"))
        print(format_log(f"  Authorization: Bearer {self.api_key[:12]}..."))
        print(format_log(f"  Mode: {self.mode}"))

        try:
            self.ws = await self.session.ws_connect(url, headers=headers, ssl=False)
        except Exception as e:
            print(format_log(f"  Connection failed: {e}"))
            print(format_log(f"  If it's a TLS error, try passing the CA cert or using ssl=False"))
            raise

        print(format_log(f"  Connected"))

    async def send_json(self, msg):
        if self.ws and not self.ws.closed:
            await self.ws.send_json(msg)

    async def recv_loop(self):
        """Receive and process events until the session ends."""
        async for msg in self.ws:
            if msg.type == aiohttp.WSMsgType.TEXT:
                data = json.loads(msg.data)
                self.event_count += 1
                await self.handle_event(data)
            elif msg.type == aiohttp.WSMsgType.CLOSED:
                break
            elif msg.type == aiohttp.WSMsgType.ERROR:
                break

    async def handle_event(self, data):
        etype = data.get("type", "?")
        if etype == "session.created":
            print(format_log(f"session.created: {data.get('event_id', '?')[:16]}..."))
            # Send session.update
            td = self.mode
            su = {
                "type": "session.update",
                "session": {
                    "modalities": ["text", "audio"],
                    "voice": "longanqian_v3.1",
                }
            }
            if td == "null" or td is None:
                su["session"]["turn_detection"] = None
                print(format_log(f"  -> session.update (turn_detection=null/PTT)"))
            elif td == "server_vad":
                su["session"]["turn_detection"] = {
                    "type": "server_vad",
                    "threshold": 0.5,
                    "silence_duration_ms": 800
                }
                print(format_log(f"  -> session.update (server_vad)"))
            else:
                su["session"]["turn_detection"] = {
                    "type": td,
                    "threshold": 0.5,
                }
                print(format_log(f"  -> session.update ({td})"))
            await self.send_json(su)

        elif etype == "session.updated":
            print(format_log(f"session.updated: turn_detection={data.get('session', {}).get('turn_detection', '?')}"))
            if self.configured is not None:
                self.configured.set()

        elif etype == "response.created":
            print(format_log(f"response.created"))

        elif etype == "response.audio.delta":
            b64 = data.get("delta", "")
            if b64:
                pcm = base64.b64decode(b64)
                self.reply_pcm.extend(pcm)

        elif etype == "response.audio_transcript.delta":
            delta = data.get("delta", "")
            self.reply_transcript += delta

        elif etype == "response.audio_transcript.done":
            text = data.get("text", "")
            print(format_log(f"assistant: {text or self.reply_transcript}"))

        elif etype == "conversation.item.input_audio_transcription.completed":
            text = data.get("transcript", "")
            self.user_transcript = text
            print(format_log(f"user transcript: {text}"))

        elif etype == "response.done":
            status = data.get("response", {}).get("status", "?")
            print(format_log(f"response.done (status={status})"))
            if self.done is not None:
                self.done.set()

        elif etype == "response.function_call_arguments.done":
            print(format_log(f"function_call: {data.get('name','?')} args={data.get('arguments','?')}"))

        elif etype == "error":
            err = data.get("error", {})
            print(format_log(f"ERROR: type={err.get('type','?')} code={err.get('code','?')} param={err.get('param','?')} msg={err.get('message','?')}"))

        elif etype == "input_audio_buffer.speech_started":
            print(format_log(f"speech_started at {data.get('audio_start_ms','?')}ms"))

        elif etype == "input_audio_buffer.speech_stopped":
            print(format_log(f"speech_stopped at {data.get('audio_end_ms','?')}ms reason={data.get('reason','?')}"))

        elif etype == "input_audio_buffer.committed":
            print(format_log(f"committed"))

        else:
            if self.event_count < 5:
                print(format_log(f"event: {etype}"))

    async def stream_clip(self):
        """Read the test clip (48 kHz PCM), resample to 16 kHz, stream it as audio."""
        if not os.path.exists(CLIP_PATH):
            print(format_log(f"Clip not found: {CLIP_PATH}"))
            return

        with open(CLIP_PATH, "rb") as f:
            pcm48 = f.read()

        # Resample 48k -> 16k
        if av:
            import numpy as np
            arr = np.frombuffer(pcm48, dtype=np.int16).reshape(1, -1)
            frame = av.AudioFrame.from_ndarray(arr, format="s16", layout="mono")
            frame.sample_rate = 48000
            resampler = AudioResampler(format="s16", layout="mono", rate=16000)
            frames = resampler.resample(frame)
            pcm16 = b"".join(bytes(f.to_ndarray()) for f in frames)
        else:
            # Simple decimate by 3 (no filtering - aliasing but good enough for a probe)
            samples = struct.unpack(f"<{len(pcm48)//2}h", pcm48)
            pcm16 = struct.pack(f"<{len(samples)//3}h", *samples[::3])
            print(format_log(f"  (no av - decimating by 3 without anti-alias filter)"))

        print(format_log(f"Streaming {len(pcm16)} bytes of 16 kHz PCM ({len(pcm16)//(2*16000)*1000} ms)"))

        # Send in 20ms chunks
        chunk = 16000 * 2 // 50  # 20ms = 640 bytes at 16kHz 16-bit mono
        for i in range(0, len(pcm16), chunk):
            part = pcm16[i:i+chunk]
            b64 = base64.b64encode(part).decode()
            await self.send_json({
                "type": "input_audio_buffer.append",
                "audio": b64
            })
            await asyncio.sleep(0.01)  # throttle slightly

        # For PTT / null mode, send commit + response_create
        if self.mode == "null" or self.mode is None:
            await asyncio.sleep(0.05)
            await self.send_json({"type": "input_audio_buffer.commit"})
            await asyncio.sleep(0.05)
            await self.send_json({"type": "response.create"})

    async def run(self):
        self.done = asyncio.Event()
        self.configured = asyncio.Event()
        await self.connect()
        recv_task = asyncio.create_task(self.recv_loop())
        try:
            # Wait for session.update to be accepted before sending audio:
            # the server rejects a late turn_detection update with
            # "Cannot update 'turn_detection' after session has started
            # processing audio".
            try:
                await asyncio.wait_for(self.configured.wait(), timeout=10)
            except asyncio.TimeoutError:
                print(format_log("timeout waiting for session.updated"))
            await self.stream_clip()
            # The server keeps the session open after the reply, so wait for
            # response.done explicitly rather than for the socket to close.
            try:
                await asyncio.wait_for(self.done.wait(), timeout=30)
            except asyncio.TimeoutError:
                print(format_log("timeout waiting for response.done"))
        finally:
            recv_task.cancel()
            try:
                await recv_task
            except asyncio.CancelledError:
                pass
            await self.session.close()

        # Save reply
        if self.reply_pcm:
            out_name = f"reply_{self.mode or 'ptt'}.wav"
            print(format_log(f"Saving {len(self.reply_pcm)} bytes of 24 kHz PCM -> {out_name}"))
            with wave.open(out_name, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(24000)
                w.writeframes(self.reply_pcm)

def main():
    parser = argparse.ArgumentParser(description="Probe the Qianwen WebSocket realtime API")
    parser.add_argument("--mode", default="smart_turn_v2",
                        choices=["smart_turn_v2", "smart_turn", "server_vad", "null"],
                        help="turn_detection mode (default: smart_turn_v2)")
    parser.add_argument("--api-key", help="Qianwen API key")
    args = parser.parse_args()

    key = args.api_key or load_secrets()
    if not key:
        print("Error: no API key. Set QIANWEN_API_KEY or pass --api-key.")
        sys.exit(1)

    client = Client(key, mode=args.mode)
    asyncio.run(client.run())

    print(f"\n=== Summary ===")
    print(f"Events received: {client.event_count}")
    print(f"Reply PCM bytes: {len(client.reply_pcm)}")
    print(f"Reply transcript: {client.reply_transcript[:100]}")
    print(f"User transcript: {client.user_transcript[:100]}")

if __name__ == "__main__":
    main()