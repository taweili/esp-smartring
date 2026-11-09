#!/usr/bin/env python3
"""Probe the Qianwen realtime 180 s response-idle timeout and keep-alive options.

Usage:
    scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy none
    scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy session_update
    scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy response_create
    scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy silence
"""
import argparse
import asyncio
import json
import os
import sys
import time

import aiohttp

ENDPOINT = "wss://maas.qianwenaiapiapi.com/api-ws/v1/realtime"
ENDPOINT = "wss://maas.qianwenaiapi.com/api-ws/v1/realtime"
MODEL = "qwen-audio-3.1-realtime-flash"


def load_key():
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


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


async def run(strategy, duration):
    key = load_key()
    if not key:
        print("no API key")
        return

    async with aiohttp.ClientSession() as session:
        url = f"{ENDPOINT}?model={MODEL}"
        ws = await session.ws_connect(url, headers={"Authorization": f"Bearer {key}"}, ssl=False)
        log("connected")

        async def send(msg):
            await ws.send_json(msg)

        # Wait for session.created, then configure PTT.
        async for msg in ws:
            if msg.type == aiohttp.WSMsgType.TEXT:
                data = json.loads(msg.data)
                if data.get("type") == "session.created":
                    log("session.created")
                    await send({"type": "session.update",
                                "session": {"modalities": ["text", "audio"],
                                            "voice": "longanqian_v3.1",
                                            "turn_detection": None}})
                    break
            elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                log("closed during setup")
                return

        # Wait for session.updated.
        async for msg in ws:
            if msg.type == aiohttp.WSMsgType.TEXT:
                data = json.loads(msg.data)
                t = data.get("type")
                if t == "session.updated":
                    log("session.updated")
                    break
                if t == "error":
                    log(f"error: {json.dumps(data)[:200]}")
            elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                log("closed during setup")
                return

        start = time.time()
        next_ka = start + 60.0
        stopped = False

        async def keeper():
            nonlocal next_ka, stopped
            while not stopped:
                await asyncio.sleep(1.0)
                now = time.time()
                if now < next_ka:
                    continue
                next_ka = now + 60.0
                elapsed = now - start
                if strategy == "session_update":
                    await send({"type": "session.update",
                                "session": {"modalities": ["text", "audio"],
                                            "voice": "longanqian_v3.1",
                                            "turn_detection": None}})
                    log(f"keep-alive @{elapsed:.0f}s: session.update")
                elif strategy == "response_create":
                    await send({"type": "response.create"})
                    log(f"keep-alive @{elapsed:.0f}s: response.create")
                elif strategy == "silence":
                    # 20 ms of 16 kHz silence, base64.
                    import base64
                    await send({"type": "input_audio_buffer.append",
                                "audio": base64.b6encode(b"\x00" * 640).decode()})
                    log(f"keep-alive @{elapsed:.0f}s: silence append")
                else:
                    log(f"idle @{elapsed:.0f}s (no keep-alive)")

        task = asyncio.create_task(keeper())

        try:
            async for msg in ws:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    data = json.loads(msg.data)
                    t = data.get("type")
                    if t == "error":
                        log(f"ERROR: {json.dumps(data)[:300]}")
                    elif t in ("response.created", "response.done",
                               "input_audio_buffer.committed"):
                        log(f"{t}")
                    elif t == "response.audio_transcript.done":
                        log(f"reply: {data.get('transcript', '')[:80]}")
                elif msg.type == aiohttp.WSMsgType.CLOSED:
                    log(f"closed after {time.time() - start:.0f}s")
                    break
                elif msg.type == aiohttp.WSMsgType.ERROR:
                    log(f"ws error after {time.time() - start:.0f}s")
                    break
                if time.time() - start > duration:
                    log(f"survived {duration:.0f}s")
                    break
        finally:
            stopped = True
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            await ws.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--strategy", default="none",
                   choices=["none", "session_update", "response_create", "silence"])
    p.add_argument("--duration", type=float, default=210.0)
    a = p.parse_args()
    asyncio.run(run(a.strategy, a.duration))


if __name__ == "__main__":
    main()
