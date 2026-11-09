# 10_qianwen_ws_voice_chat — WebSocket voice conversation

`examples/10_qianwen_ws_voice_chat/` is a **push-to-talk voice assistant** for
the Smart Ring Plus, connected via **WebSocket** (TCP/TLS 443) to the Qianwen
realtime API — the only realtime transport that works on a UDP-blocked network.

```
        ┌─────────────────────────────┐
        │  QIANWEN WS                   │ ← long-press = settings
        │  Pretty-Donkey-1 · 192.168…   │ ← Wi-Fi + WS state
        │      ┌───────────────┐      │
        │      │               │      │
        │      │  HOLD TO TALK │      │  ← round button, solid colour fill
        │      │               │      │
        │      └───────────────┘      │
        │    ready (ptt) — hold to    │  ← status
        │      "你好，帮我…"            │  ← user transcript (orange)
        │      "Sure — …"             │  ← model reply (white)
        │   ↑0 ↓0 B/s · turn 0 · ptt   │  ← wire stats + battery
        │      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁      │  ← level bar
        └─────────────────────────────┘
```

| State | Button | Status | What happens |
|---|---|---|---|
| Idle | **green fill**, `HOLD TO TALK` | "ready (ptt) — hold to talk" | session open, mic closed |
| Talking | **red fill**, `TALK` | "listening…" + level | 48k mic → 16k PCM → base64 → WS |
| Waiting | **red fill**, `WAIT` | "waiting…" | released, server processing |
| Replying | **blue fill**, `REPLY` | "replying… · mic off" + level | WS → base64 → 24k PCM → 48k → speaker |
| Connecting | grey fill, `···` | "connecting…" | Wi-Fi → SNTP → WS → session.update |
| Error | red fill, `!` | "error: \<type\> \<param\>" or "connection lost" | retries every 5 s |

## Why WebSocket?

- **UDP is blocked on this bench.** The WebRTC path (07) fails here because
  DTLS handshake packets cannot leave the LAN. WebSocket uses TCP/TLS 443 which
  works.
- **No Opus, no esp_peer, no DTLS/SRTP.** Raw PCM in JSON over a single
  persistent TCP socket. The only ESP-IDF dependency beyond the baseline is
  `esp_websocket_client`.
- **Three turn-taking modes:** server_vad, smart_turn_v2, and push-to-talk.
  Push-to-talk is the default; all three are selectable in the settings panel.

## Architecture

```
touch ──► LVGL task ──► 10_qianwen_ws_voice_chat
                          │  PRESSED / RELEASED → flags + worker
                          │  long-press title   → settings panel
                          │  ui_*() → RING_UI_LOCKED
                          ▼
                     ring_ws  (components/ringkit)
                          │  Wi-Fi (ring_wifi) + SNTP (ring_time_sync)
                          │  Resampler: 48k↔16k send / 24k↔48k recv
                          │  base64 (mbedtls_base64_encode/decode)
                          │  Session protocol (session.update → response.done)
                          ▼
            wss://maas.qianwenaiapi.com/api-ws/v1/realtime
                   ?model=qwen-audio-3.1-realtime-flash
```

| File | Role |
|---|---|
| `qianwen_ws_voice_chat.c` | UI, button state machine, stereo↔mono, talk turn, settings |
| `components/ringkit/src/ring_ws.c` | Public API, state machine, event task |
| `components/ringkit/src/ring_ws_transport.c` | WS lifecycle, reassembly, base64, playback |
| `components/ringkit/src/ring_ws_protocol.c` | Session/turn JSON, uplink |
| `components/ringkit/include/ringkit/ring_ws.h` | `ring_ws_*` API |
| `components/ringkit/src/ring_resample.c` | Shared polyphase FIR resampler (48k↔16k / 24k↔48k) |
| `components/ringkit/src/ring_resample_coeffs.c` | Generated coefficient tables |

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
`ring_example_start()` calls `ring_nvs_init()` before reading the key, so a
value already persisted in NVS is honoured rather than overwritten.

## Transcript & stats

- **Transcript.** The user's utterance and the model's reply are shown on
  separate lines (user orange, model white), so a conversation stays readable.
  Both are capped to the round panel's width and truncate with an ellipsis;
  the full text is also logged (`heard: …`, `reply: …`).
- **Stats.** A 1 Hz timer refreshes a line with the wire rate
  (`↑… ↓… B/s`, computed over the last second), the turn count, the negotiated
  turn mode, and the battery percentage.
- **Errors.** A protocol `error` event's `type` and `param` are shown in the
  status line instead of a generic "connection lost".

## Settings

**Long-press the title** to open the settings panel. Each row cycles through
its values on tap:

| Row | Values | Default |
|---|---|---|
| Mode | `ptt`, `server_vad`, `smart_turn_v2` | `ptt` |
| Voice | `longanqian_v3.1`, `longanhuan_v3.1`, `longanlingxin_v3.1`, `longanfengyue_v3.1`, `xunanchuan_v3.1` | `longanqian_v3.1` |
| Rate | `0.5`, `1.0`, `1.5`, `2.0` | `1.0` |
| Lang | `auto`, `zh`, `en` | `auto` |

The values persist in NVS (`qianwen`/`turn_mode`, `/voice`, `/rate`, `/lang`).
Changing **Mode** or **Voice** requires a reconnect — the Qianwen API accepts
them only while IDLE. `ring_ws` sends `session.update` only at session setup,
so **every** change restarts the session; the connect task notices the dirty
flag and reconnects.

## Bench self-test (no touch needed)

The example can stream an embedded speech clip through one complete turn on
boot, so the whole path (resampler → base64 → WS → session → reply → speaker)
can be verified without a finger on the button. It is **off by default** —
comment in the two lines at the bottom of `CMakeLists.txt`:

```cmake
set(RING_EXAMPLE_DEFINES     "QW_SELFTEST=1")
set(RING_EXAMPLE_EMBED_FILES "assets/qw_selftest.pcm")
```

The clip is 48 kHz / 16-bit mono PCM (`assets/qw_selftest.pcm`, ~1.7 s), the
format `ring_ws_send_audio()` expects. The self-test waits for `RING_WS_READY`,
streams the clip in 20 ms frames, commits, and plays the reply. Disabling it
also drops the embedded clip from the image (~163 KB of flash).

## Keep-alive and the send buffer

Two failure modes produced "session lost" and are now handled:

- **Mid-turn abort.** `esp_websocket_client` aborts the whole connection when a
  send times out (`transport_poll_write` → `abort_connection(TCP_TRANSPORT)`).
  The default 5.7 KB TCP send buffer is smaller than the uplink
  bandwidth-delay product (~45 KB/s), so it fills during a turn and the 20 ms
  send timeout killed the session. Fixed by raising
  `RING_WS_SEND_TIMEOUT_MS` to 100 ms and `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`
  to 16384 (in `sdkconfig.defaults`).
- **180 s idle timeout.** The Qianwen server closes a session that has not
  generated a response for 180 s (`response_idle_timeout`). The connect task
  re-sends `session.update` every 60 s while the session is idle, which resets
  the timer without producing a reply. Verified with
  `scripts/test_qianwen_ws_keepalive.py`.

## Limitations

- **Half-duplex.** The ring has no acoustic echo cancellation. The mic is muted
  during playback (the half-duplex gate) and for 500 ms after. A loud utterance
  during playback triggers a client-side barge-in (cancels the current response).
- **Bandwidth ~120 KB/s** (16 kHz PCM + 33% base64 overhead) vs ~32 kbps for
  Opus. The stats line makes the cost visible.
- **No function calling yet** (planned for a later phase).
- **Transcript is single-line per speaker.** Long utterances truncate with an
  ellipsis; the full text is in the log.

## Verification checklist

1. The 00 / 06 / 07 / 09 apps still build.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
3. Boot log: `ring_ws: session.updated`, `state -> ready`, no panic/assert.
4. One PTT turn works: press-and-hold → TALK → release → WAIT → REPLY → audio.
   (Or enable the bench self-test and check the boot log for a full turn.)
5. The transcript shows both the user utterance and the model reply.
6. The stats line shows a non-zero wire rate during a turn.
7. Long-press the title → settings panel opens; changing Mode reconnects.
8. Touch and voice need a human — no host-side way to synthesise.
