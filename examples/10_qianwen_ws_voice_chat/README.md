# 10_qianwen_ws_voice_chat — WebSocket voice conversation

`examples/10_qianwen_ws_voice_chat/` is a **push-to-talk voice assistant** for
the Smart Ring Plus, connected via **WebSocket** (TCP/TLS 443) to the Qianwen
realtime API — the only realtime transport that works on a UDP-blocked network.

```
        ┌─────────────────────────────┐
        │ ≡  QIANWEN WS          ⚙    │ ← Talk · chat · settings
        │      ╭───────────────╮      │
        │     │ ╭───────────╮ │      │ ← radial level ring
        │     │ │  ◉        │ │      │   (green mic / blue reply)
        │     │ │ HOLD      │ │      │
        │     │ │ TO TALK   │ │      │
        │     │ ╰───────────╯ │      │
        │      ╰───────────────╯      │
        │    ready (ptt) — hold to    │  ← state line
        │    "你好，帮我…"             │  ← last exchange
        │    "Sure — …"               │    (user orange / model white)
        └─────────────────────────────┘
```

Three screens, switched from the top bar:

- **Talk** (home) — the orb (button + radial level ring), the state line and
  the last exchange. The ring is green while capturing, blue while playing.
- **Chat** (`≡`) — the full scrolling transcript, plus wire/battery stats and
  the Wi-Fi/WS state.
- **Set** (`⚙`) — turn mode / voice / rate / language (see below).

| State | Orb | Status | What happens |
|---|---|---|---|
| Idle | **green fill/ring**, `HOLD TO TALK` | "ready (ptt) — hold to talk" | session open, mic closed |
| Talking | **red fill/ring**, `TALK` | "listening…" + ring | 48k mic → 16k PCM → base64 → WS |
| Waiting | **red fill/ring**, `WAIT` | "waiting…" | released, server processing |
| Replying | **blue fill/ring**, `REPLY` | "replying… · mic off" + ring | WS → base64 → 24k PCM → 48k → speaker |
| Connecting | grey fill/ring, `···` | "connecting…" | Wi-Fi → SNTP → WS → session.update |
| Error | red fill/ring, `!` | "error: \<type\> \<param\>" or "connection lost" | retries every 5 s |

## Why WebSocket?

- **UDP is blocked on this bench.** The WebRTC path (07) fails here because
  DTLS handshake packets cannot leave the LAN. WebSocket uses TCP/TLS 443 which
  works. This is the only way to demo a realtime voice conversation on the
  bench.
- **It is the smallest realtime path.** `esp_websocket_client` + `esp-tls` +
  base64; no `esp_peer`, no DTLS-SRTP, no ICE/STUN/TURN, no signaling server,
  no codec.
- **It can do things WebRTC cannot.** Manual turn-taking (`turn_detection:
  null` + `input_audio_buffer.commit` + `response.create`), semantic turn
  detection (`smart_turn_v2`), barge-in (`response.cancel`), and function
  calling (`response.function_call_arguments.done` →
  `conversation.item.create` → `response.create`). WebRTC supports only
  `server_vad`/`semantic_vad`.

Non-goals: acoustic echo cancellation (the ring has none and the WS transport
has none — half-duplex), bandwidth efficiency (~120 KB/s vs ~32 kbps Opus),
and the QUIC comparison (that is the planned `11_quic_voice_chat`).

## Protocol

### Endpoint and audio format

```
wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash
Authorization: Bearer <key>        (handshake header)
```

The endpoint speaks **raw PCM**, not Opus:

| Direction | Format |
|---|---|
| Input (client → server) | PCM, **16 kHz**, 16-bit, mono, base64 in `input_audio_buffer.append` |
| Output (server → client) | PCM, **24 kHz**, 16-bit, mono, base64 in `response.audio.delta` |

The ring's audio path is fixed at **48 kHz / 16-bit / stereo** (`ring_audio`),
so `ring_ws` resamples at the boundary:

```
mic 48k stereo ──fold──► 48k mono ──decimate 3:1──► 16k mono ──base64──► JSON
speaker 48k stereo ◄─expand── 48k mono ◄─interp 1:2── 24k mono ◄─base64── JSON
```

Both ratios are exact integers, so a small polyphase FIR is enough
(`ring_resample.c` + generated `ring_resample_coeffs.c`): decimate-by-3
anti-alias lowpass (~7.5 kHz cutoff) and interpolate-by-2 half-band lowpass
(~11 kHz cutoff). Group delay is ~1 ms, negligible against the 300–800 ms
playback jitter buffer. The app-facing API stays **48 kHz mono**, identical to
`ring_net`, so 06/07-style app code ports almost unchanged.

### Session lifecycle

```
connect ──► session.created ──► session.update ──► session.updated ──► READY
                                                                  │
                       input_audio_buffer.append (16k PCM b64) ◄──┤ LISTENING
                                                                  │
                       response.audio.delta (24k PCM b64) ───────►│ SPEAKING
                       response.audio_transcript.delta           │
                       response.done ─────────────────────────────► READY
```

`turn_detection`, `voice` and the audio formats are **IDLE-only** — they can
only be changed before the first audio is sent (or by reconnecting). A session
is either push-to-talk or VAD for its lifetime.

### Turn-taking modes

| Mode | Config | Use |
|---|---|---|
| Push-to-talk | `null` | button gates the mic; `input_audio_buffer.commit` + `response.create` |
| Acoustic VAD | `{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}` | hands-free, latency-tolerant |
| Semantic | `{"type":"smart_turn_v2"}` | hands-free, ignores "嗯/啊"; **recommended by the platform** |

If `smart_turn_v2` is rejected, `ring_ws` retries `session.update` once with
`server_vad` (preserving all other fields) before any audio is sent, and
records the negotiated mode for the UI.

### Events handled

| Server event | Action |
|---|---|
| `session.created` | send `session.update` |
| `session.updated` | → READY |
| `input_audio_buffer.speech_started` | barge-in: `response.cancel()`, flush downlink |
| `input_audio_buffer.speech_stopped` / `.committed` | log only |
| `response.created` | → SPEAKING |
| `response.audio.delta` | base64 → 24 kHz PCM ring |
| `response.audio_transcript.delta` / `.done` | transcript callback (assistant) |
| `response.done` | → READY (status `completed`/`cancelled`) |
| `conversation.item.input_audio_transcription.completed` | transcript callback (user) |
| `response.function_call_arguments.done` | tool callback → app writes `function_call_output` → `response.create` |
| `error` | log; `server_error` → ERROR (fatal), `invalid_request_error` → log; `param` mentioning `turn_detection` triggers the `server_vad` fallback |

`session.update` fields used: `modalities: ["text","audio"]`, `voice`,
`speech_rate`, `instructions` (system prompt), `input_audio_transcription.
language`, `output_audio.language`, `max_history_turns`, `enable_search`,
`tools`, `turn_detection`.

## Architecture

```
touch ──► LVGL task ──► 10_qianwen_ws_voice_chat
                          │  PRESSED / RELEASED → flags + worker
                          │  ≡ / ⚙ → screen switch
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
| `qianwen_ws_voice_chat.c` | Three screens, orb, chat log, talk turn, settings |
| `components/ringkit/src/ring_ws.c` | Public API, state machine, event task |
| `components/ringkit/src/ring_ws_transport.c` | WS lifecycle, reassembly, base64, playback |
| `components/ringkit/src/ring_ws_protocol.c` | Session/turn JSON, uplink |
| `components/ringkit/include/ringkit/ring_ws.h` | `ring_ws_*` API |
| `components/ringkit/src/ring_resample.c` | Shared polyphase FIR resampler (48k↔16k / 24k↔48k) |
| `components/ringkit/src/ring_resample_coeffs.c` | Generated coefficient tables |

### `ring_ws` internals

- **Tasks.** The app owns `qw-connect` (Wi-Fi + SNTP + WS session + reconnects,
  8 KB) and `qw-talk` (the PTT capture/play worker, 8 KB). `ring_ws` adds a
  `ring-ws-worker` (8 KB) and a `ring-ws-event` (6 KB) task; the
  `esp_websocket_client` has its own task (8 KB stack, priority 5).
- **The WS event callback runs on the WebSocket client task.** It does *only*
  copy fragments into a PSRAM reassembly buffer and push completed messages to
  the worker queue — no JSON, no base64, no LVGL, no flash/NVS. While the
  callback runs, the client task is not reading the socket, so anything heavier
  would fill the TCP window and stall the server.
- **Reassembly.** One pre-allocated 64 KB PSRAM buffer; only `op_code == 1`
  (text) frames are reassembled. On `fin` the message is queued and the WS task
  waits on an ack semaphore so the worker can parse without the next fragment
  overwriting the buffer. Oversized messages are dropped and logged.
- **Downlink ring.** 4 s / **192 KB** PSRAM of 24 kHz mono PCM, newest-wins
  overflow. A reply arriving faster than playback drops old audio instead of
  growing unbounded.
- **Half-duplex gate.** The ring has no AEC, so `ring_ws_send_audio()` drops
  uplink frames while `responding || buffered > 0 || now - last_playback <
  500 ms`. The app never sends a frame the model should not hear. A tap while
  the model replies is a barge-in (`response.cancel()` + flush); in hands-free
  modes `input_audio_buffer.speech_started` does the same.
- **Playback cushion.** `ring_ws_read_audio()` waits for ~200 ms of downlink
  before starting playback (only when the ring is empty), so bursty
  `response.audio.delta` arrival does not stutter.
- **Watchdog.** If `response.created` is never followed by `response.done`
  within 20 s, the gate is force-cleared so the mic cannot stay muted forever.
- **Keep-alive.** The server closes a session idle for 180 s. The app calls
  `ring_ws_keepalive()` every 60 s while READY, which re-sends `session.update`.

### `ring_ws` API (summary)

`ring_ws_init/start/stop`, `ring_ws_state/state_name/turn_mode/get_stats`,
`ring_ws_set_listening`, `ring_ws_send_audio` (48 kHz mono, 960 samples =
20 ms), `ring_ws_read_audio` (48 kHz mono), `ring_ws_is_speaking`,
`ring_ws_playback_pending`, `ring_ws_buffered_ms`, `ring_ws_commit`,
`ring_ws_response_create`, `ring_ws_cancel`, `ring_ws_send_json`,
`ring_ws_keepalive`. State, transcript and tool callbacks run on the
`ring-ws-event` task, so they may take the LVGL lock. See
`components/ringkit/include/ringkit/ring_ws.h`.

### Stack and memory budget

| | 07 (`ring_net`) | 10 (`ring_ws`) |
|---|---|---|
| Audio task stack | **64 KB**, PSRAM (`xTaskCreateWithCaps`) — stereo Opus | **8 KB**, internal RAM — no codec |
| Downlink ring | 4 s / 384 KB PSRAM (`RING_NET_DL_BYTES`), newest-wins | 4 s / 192 KB PSRAM (24 kHz mono), newest-wins |
| Uplink scratch | 20 ms mono→stereo up-mix | 20 ms 48k→16k decimate |
| Reassembly | n/a | 64 KB PSRAM |
| WS client buffer | n/a | 4 KB (internal RAM) |

FreeRTOS stacks come from internal RAM only, which is why 07 needs
`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`. Without Opus, a plain 8 KB
`xTaskCreate` suffices.

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

`sdkconfig.defaults` carries the two options that matter for this transport:

- `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y` — uplink and downlink run
  concurrently; with a single lock, `esp_websocket_client_send_text()` can
  block the RX path.
- `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384` — see "Keep-alive and the send
  buffer" below.

The WS client is configured with `buffer_size = 4096` (internal RAM),
`disable_auto_reconnect = true` (the `ring_ws` state machine owns retries, and
a reconnect needs a fresh `session.update`), `task_stack = 8192`,
`ping_interval_sec = 15`, and the embedded Qianwen CA (`ring_ws_ca_pem()`).

## Transcript, chat and stats

- **Talk screen.** The last exchange: the user's utterance (orange) above the
  model's reply (white), each wrapped to two lines.
- **Chat screen.** The full conversation as a scrolling log (user orange,
  model blue), auto-scrolled to the latest message, plus:
  - a 1 Hz wire line — `↑… ↓… B/s` (computed over the last second), the turn
    count, the negotiated turn mode and the battery percentage;
  - the Wi-Fi/WS state line (`ssid · ip │ WS state`).
- **Errors.** A protocol `error` event's `type` and `param` are shown in the
  state line instead of a generic "connection lost".

## Settings

Open the **Set** screen (`⚙`). Each row cycles through its values on tap:

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

## Implementation notes

Decisions that are easy to get wrong when modifying this path:

- **The event callback runs on the WS client task.** Keep it to fragment copies
  and queue pushes. Never parse JSON, take the LVGL lock, write audio, or touch
  flash/NVS there.
- **Turn mode / voice are IDLE-only.** There is no runtime
  `ring_ws_set_turn_mode()`. A mode change means `ring_ws_stop()` +
  `ring_ws_start()`; the UI shows "reconnecting…".
- **The `smart_turn_v2` fallback must preserve the rest of the config.** The
  retry reuses the same `ring_ws_session_cfg_t` and replaces only
  `turn_detection`.
- **`ring_ws_start()` owns Wi-Fi + SNTP.** The TLS handshake validates the
  server certificate against the wall clock, so an unset clock fails the
  handshake. `ring_ws_start()` reuses `ring_wifi_*` and `ring_time_sync()`.
- **Tear down before re-init.** `ring_ws_start()` stops and destroys any live
  client before creating a new one; otherwise the old transport/buffers leak.
- **The 24k→48k interpolator is streaming.** `response.audio.delta` decodes to
  variable-size chunks, so the filter carries history across
  `ring_ws_read_audio()` calls. The 48k→16k decimator is block-aligned
  (960→320) and stateless.
- **Base64-decoded PCM must be converted to `int16`.** The filter output is
  `float`; sending the raw floats ships IEEE-754 bit patterns as "PCM" and the
  model only hears noise.
- **`response.cancel` is guarded.** It is a no-op unless a response is in
  progress; cancelling with nothing running returns an `invalid_request_error`.
- **Reassembly is pre-allocated and filtered by `op_code`.** No per-fragment
  heap churn; non-text frames are ignored.

## Limitations

- **Half-duplex.** The ring has no acoustic echo cancellation. The mic is muted
  during playback (the half-duplex gate) and for 500 ms after. A tap while the
  model is replying is a barge-in (cancels the current response); in hands-free
  modes `input_audio_buffer.speech_started` does the same when the server hears
  the user.
- **Bandwidth ~120 KB/s** (16 kHz PCM + 33% base64 overhead) vs ~32 kbps for
  Opus. The Chat screen makes the cost visible.
- **No function calling yet** (planned for a later phase).
- **Transcript buffers are 256 B.** Longer replies are truncated on screen;
  the full text is in the log.

## Verification checklist

1. The 00 / 06 / 07 / 09 / 12 apps still build; `esp_websocket_client` resolves
   from `managed_components/`.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
3. New image size measured against the 3000K partition (07 has ~360 KB free).
4. Boot log: `ring_ws: session.updated`, `state -> ready`, no panic/assert; the
   negotiated turn mode is logged (`smart_turn_v2` or the fallback).
5. On this bench network (UDP blocked): a full turn works — the key advantage
   over 06/07.
6. Wire stats on screen roughly match 32 KB/s up / 48 KB/s down + base64
   overhead.
7. Stack high-water marks logged for `ring-ws-worker`, `ring-ws-event`, the app
   capture/play task, and the WS client task after `WEBSOCKET_EVENT_CONNECTED`.
8. The orb shows a ring; it fills green while talking, blue while replying.
9. `≡` opens the Chat screen (scrolling transcript + stats); `⚙` opens Set.
10. One PTT turn works: press-and-hold → TALK → release → WAIT → REPLY → audio.
    (Or enable the bench self-test and check the boot log for a full turn.)
11. The last exchange stays on the Talk screen after the reply.
12. Changing Mode on the Set screen reconnects (`session.update` in the log).
13. Touch and voice need a human — no host-side way to synthesise. The log
    proves the pipeline, not the conversation.

## Alternatives considered

1. **WebRTC (07)** — built-in AEC, but UDP-blocked here and heavier. Keep as
   the other transport.
2. **QUIC/AOQ** — best weak-network behaviour, but no C SDK; gateway project.
   See `examples/11_quic_voice_chat/README.md`.
3. **HTTP chunked PCM** — no full-duplex, no turn events. No.
4. **Opus over WS** — the endpoint only accepts PCM; not possible.
5. **Reuse `ring_net` with a WS backend** — a facade is the end state, but a
   separate `ring_ws` is simpler first and keeps 06/07 stable.

## Sources

* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/websocket-access`
  — endpoint, auth header, interaction modes (server_vad / smart_turn_v2 /
  push-to-talk).
* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/client-events`
  — `session.update` fields, `input_audio_buffer.append/commit/clear`,
  `response.create/cancel`, `conversation.item.create`.
* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/server-events`
  — `session.created/updated`, `response.audio.delta`,
  `response.audio_transcript.delta/done`, `response.done`, `error`,
  function-calling events.
* `platform.qianwenai.com/docs/developer-guides/speech/qwen-audio-realtime`
  — 16 kHz in / 24 kHz out, context limits, echo-suppression demo.
* `docs.espressif.com/projects/esp-protocols/esp_websocket_client` — config
  fields (`headers`, `buffer_size`, `ping_interval_sec`), fragmentation
  (`payload_offset`/`payload_len`/`fin`), `esp_websocket_register_events`.
