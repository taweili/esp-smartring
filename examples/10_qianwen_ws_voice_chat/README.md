# 10_qianwen_ws_voice_chat — WebSocket voice conversation

`examples/10_qianwen_ws_voice_chat/` is a **push-to-talk voice assistant** for
the Smart Ring Plus, connected via **WebSocket** (TCP/TLS 443) to the Qianwen
realtime API — the only realtime transport that works on a UDP-blocked network.

```
        ┌─────────────────────────────┐
        │        QIANWEN WS           │
        │      ┌───────────────┐      │
        │      │               │      │
        │      │  HOLD TO TALK │      │  ← round button, solid colour fill
        │      │               │      │
        │      └───────────────┘      │
        │    ready (ptt) — hold to    │
        │      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁      │
        │    hold = talk · release    │
        └─────────────────────────────┘
```

| State | Button | Status | What happens |
|---|---|---|---|
| Idle | **green fill**, `HOLD TO TALK` | "ready (ptt) — hold to talk" | session open, mic closed |
| Talking | **red fill**, `TALK` | "listening…" + level | 48k mic → 16k PCM → base64 → WS |
| Waiting | **red fill**, `WAIT` | "waiting…" | released, server processing |
| Replying | **blue fill**, `REPLY` | "replying…" + level | WS → base64 → 24k PCM → 48k → speaker |
| Connecting | grey fill, `···` | "connecting…" | Wi-Fi → SNTP → WS → session.update |
| Error | red fill, `!` | "connection lost" | retries every 5 s |

## Why WebSocket?

- **UDP is blocked on this bench.** The WebRTC path (07) fails here because
  DTLS handshake packets cannot leave the LAN. WebSocket uses TCP/TLS 443 which
  works.
- **No Opus, no esp_peer, no DTLS/SRTP.** Raw PCM in JSON over a single
  persistent TCP socket. The only ESP-IDF dependency beyond the baseline is
  `esp_websocket_client`.
- **Three turn-taking modes:** server_vad, smart_turn_v2 (recommended), and
  push-to-talk. PTT is not available over WebRTC.

## Architecture

```
touch ──► LVGL task ──► 10_qianwen_ws_voice_chat
                          │  PRESSED / RELEASED → flags + worker
                          │  ui_*() → RING_UI_LOCKED
                          ▼
                     ring_ws  (components/ringkit, new module)
                          │  Wi-Fi (ring_wifi) + SNTP (ring_net_sync_clock)
                          │  Resampler: 48k↔16k send / 24k↔48k recv
                          │  base64 (mbedtls_base64_encode/decode)
                          │  Session protocol (session.update → response.done)
                          ▼
            wss://maas.qianwenaiapi.com/api-ws/v1/realtime
                   ?model=qwen-audio-3.1-realtime-flash
```

| File | Role |
|---|---|
| `qianwen_ws_voice_chat.c` | UI, button state machine, stereo↔mono, talk turn |
| `components/ringkit/src/ring_ws.c` | WS client + resampler + base64 + session protocol |
| `components/ringkit/include/ringkit/ring_ws.h` | `ring_ws_*` API |
| `components/ringkit/src/ring_ws_resample.c` | Generated polyphase FIR coefficient tables |

## Build & flash

```bash
idf.py -DRING_APP=10_qianwen_ws_voice_chat -DQIANWEN_API_KEY="sk-ws-..." \
       -B build/10_qianwen_ws_voice_chat build
idf.py -B build/10_qianwen_ws_voice_chat -p /dev/ttyACM0 flash
idf.py -B build/10_qianwen_ws_voice_chat -p /dev/ttyACM0 monitor
```

Use the **same `-B`** for build/flash/monitor.

## Configuration

See `examples/06_qianwenai_push_to_talk/README.md` §7 — the same mechanism
applies (NVS key `qianwen`/`api_key`, seeded from build-time `QIANWEN_API_KEY`).

## Limitations

- **Half-duplex.** The ring has no acoustic echo cancellation. The mic is muted
  during playback (the half-duplex gate) and for 500 ms after. A loud utterance
  during playback triggers a client-side barge-in (cancels the current response).
- **Bandwidth ~120 KB/s** (16 kHz PCM + 33% base64 overhead) vs ~32 kbps for
  Opus. Wire stats are visible in future UI versions.
- **No function calling yet** (planned for a later phase).

## Verification checklist

1. The 00 / 06 / 07 / 09 apps still build.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
3. Boot log: `ring_ws: session.updated`, `state -> ready`, no panic/assert.
4. One PTT turn works: press-and-hold → TALK → release → WAIT → REPLY → audio.
5. Touch and voice need a human — no host-side way to synthesise.