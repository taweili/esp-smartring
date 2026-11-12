# ringkit

The generic Smart Ring Plus skeleton. `ringkit` wraps the whole
`viewesmart/smartring_plus` BSP so applications never include
`bsp/smartring_plus.h` or call `bsp_*` directly, and it owns the shared
infrastructure the voice modules would otherwise copy: JSON field extraction,
the HTTPS client, the SNTP sync, the NVS helpers, the byte ring and the
polyphase resampler.

Applications include exactly one header — `ringkit/ringkit.h` — and implement
the two-symbol example contract (see `examples/README.md`).

## Module map

### Board wrappers (thin, out of scope for restructuring)

| Header | Wraps | Notes |
|---|---|---|
| `ring_board.h` | `bsp_*` bring-up | Encodes the required init order; `ring_board_init()` |
| `ring_display.h` | ST77916 QSPI panel | backlight, LVGL display handle, RGB565 swap |
| `ring_touch.h` | CST816 capacitive touch | already wired to LVGL as an indev |
| `ring_battery.h` | AXP2101 (V2) / GPIO1 ADC (V1) | thread-safe getters |
| `ring_power.h` | soft power-off GPIO47 | `ring_power_shutdown()` normally does not return |
| `ring_button.h` | BOOT button on GPIO0 | press / long-press / click / double-click (`ring_button_*`) |
| `ring_audio.h` | ES8311 + I2S 48 kHz / 16-bit / stereo | read/write/mute + stereo↔mono |
| `ring_imu.h` | QMI8658A accel + gyro + temp | thread-safe getters, level calibration |
| `ring_sd.h` | SDMMC + FAT `/sdcard` | no card-detect pin: "no card" == "mount failed" |
| `ring_ui.h` | LVGL lock + helpers | `RING_UI_LOCKED`, label creation, exact centring |
| `ring_theme.h` | colours / geometry / motion | `RING_PANEL_*`, `RING_SAFE_RADIUS`, `RING_IN_CIRCLE()`, `RING_COLOR_*`, `RING_MS_*` |
| `ring_rand.h` | deterministic xorshift32 | **not** `esp_random()` — determinism is the point |

### Connectivity

| Header | Wraps | Notes |
|---|---|---|
| `ring_wifi.h` | ESP-IDF Wi-Fi STA | NVS-persisted credentials, thread-safe state |
| `ring_bt.h` | NimBLE BLE peripheral | declarative GATT DB, GAP state, notifications |
| `ring_secret.h` | NVS + build-time secrets | `ring_secret_get()` |

### Voice / realtime

| Header | Transport | Notes |
|---|---|---|
| `ring_chat.h` | HTTPS REST cascade (STT→LLM→TTS) | blocking, no realtime; `examples/06` |
| `ring_ws.h` | WebSocket, PCM in JSON | server_vad / smart_turn_v2 / PTT; `examples/10` |
| `ring_net.h` | WebRTC, Opus over RTP (Qianwen) | lowest latency, needs UDP/DTLS; `examples/07` |
| `ring_geodis.h` | WebRTC, Opus over RTP (GEODIS / Azure Realtime) | ephemeral token + Azure SDP, client-executed RAG tool call; **no app yet** (see `notes/geodis-push-to-talk-plan.md`) |

### Shared infrastructure

| Header | Purpose |
|---|---|
| `ring_ca.h` | the embedded Qianwen CA PEM |
| `ring_time.h` | SNTP clock sync |
| `ring_nvs.h` | NVS init + get-or-seed |
| `ring_json.h` | field extraction + overflow-checked append |
| `ring_byte_ring.h` | newest-wins byte ring with a mutex |
| `ring_resample.h` | 48k→16k decimator, 24k→48k interpolator |
| `ring_http.h` | HTTPS client with the pinned CA + buffered SSE reader |

## Board bring-up order

`ring_board_init()` encodes an order that is not obvious from the individual
`bsp_*` calls:

1. `bsp_display_init()` — creates the shared I2C bus **and** starts the LVGL
   task, so it must be first.
2. `bsp_battery_init()` — probes AXP2101 (V2) or GPIO1 ADC (V1).
3. `bsp_power_init()` — arms the soft power-off GPIO.
4. `bsp_audio_init()` — ES8311 + I2S.
5. `bsp_sd_init()` — optional (no card-detect pin).
6. `bsp_imu_init()` — optional.

Display/battery/power/audio are mandatory; the first failure is returned and
later stages are skipped. SD and IMU fail soft and are reported in
`ring_board_t`.

## Task / thread model

Only the **LVGL adapter task** (started by `bsp_display_init()`) may touch LVGL
objects without the lock. Every other task that wants to update the UI must
take `RING_UI_LOCKED(-1) { ... }`.

Tasks created by ringkit:

| Task | Stack | Prio | Owner | Purpose |
|---|---|---|---|---|
| `lvgl_adapter` | BSP | — | BSP | LVGL timerhandler + flush |
| BSP IMU sampler | BSP | — | BSP | 250 Hz accel/gyro/temp |
| `ring-button` | 3 KB | 5 | `ring_button` | 10 ms BOOT-button sample + event decode |
| `ring-net-signal` | 4 KB | 4 | `ring_net` | SDP exchange / peer bring-up |
| `ring-net-event` | 6 KB | 4 | `ring_net` | state + transcript callbacks |
| `ring-net-peer` | 16 KB | 5 | `ring_net` | `esp_peer` main loop |
| `ring-geodis-signal` | 4 KB | 4 | `ring_geodis` | token + SDP exchange / peer bring-up |
| `ring-geodis-event` | 6 KB | 4 | `ring_geodis` | state + transcript callbacks |
| `ring-geodis-peer` | 16 KB | 5 | `ring_geodis` | `esp_peer` main loop |
| `ring-geodis-tool` | 12 KB | 4 | `ring_geodis` | client-executed `/tool-call` HTTPS POST |
| `ring-geodis-mock` | 4 KB | 5 | `ring_geodis` (mock) | stand-in for the peer: canned turns, no network |
| `ring-ws-worker` | 8 KB | 5 | `ring_ws` | WS transport / reassembly / base64 |
| `ring-ws-event` | 6 KB | 4 | `ring_ws` | state + transcript + tool callbacks |
| NimBLE host | NimBLE | — | `ring_bt` | GAP + GATT access callbacks |

Consequences:

- The voice **event** callbacks run on `ring-*-event`, not on a peer/WS thread,
  so they may take the LVGL lock.
- The BT **GATT access** callbacks run on the NimBLE host task — keep them
  short and never touch LVGL directly; copy the value and let a UI timer
  render it (`ring_bt_notify()` is safe from any task).
- Blocking voice calls (`ring_chat_stt/llm/tts`, `ring_net_start`,
  `ring_ws_start`) must run on an app worker task, never on the LVGL task.
- `ring_wifi` uses event callbacks, not its own task.

## State machines

```
ring_wifi:  OFF ──start()──▶ DISCONNECTED ──connect──▶ CONNECTING ──IP──▶ CONNECTED
ring_bt:    OFF ──init()──▶ IDLE ──advertising()──▶ ADVERTISING ──peer──▶ CONNECTED
ring_net:   OFF ──start()──▶ CONNECTING ──READY──▶ LISTENING ──reply──▶ SPEAKING
                                      └──fail──▶ ERROR
ring_geodis: OFF ──start()──▶ CONNECTING ──READY──▶ LISTENING ──reply──▶ SPEAKING
                                      └──fail──▶ ERROR
ring_ws:    OFF ──start()──▶ CONNECTING ──READY──▶ LISTENING ──reply──▶ SPEAKING
                                      └──fail──▶ ERROR
ring_imu_calib: IDLE ──start()──▶ RUNNING ──stable──▶ PASSED
                                          └──30 s──▶ FAILED
```

ring_button is a small state machine fed by its poll task:

```
ring_button: IDLE ──press──▶ PRESSED ──hold──▶ LONG_PRESS
                      │          └──release──▶ WAIT_DOUBLE ──timeout──▶ (CLICK)
                      │                                   └──press──▶ PRESSED2
                      │                                                   └──release──▶ DOUBLE_CLICK
                      └──release──▶ CLICK (immediate)
```

All state getters are thread safe. `ring_net`/`ring_ws` guard their state with
a mutex; `ring_wifi`/`ring_bt` do the same.

## API contract

- Include only `ringkit/ringkit.h`.
- Never include `bsp/smartring_plus.h` and never call `bsp_*`; do not touch
  `board_config.h` pins.
- All LVGL object creation/updates happen inside `RING_UI_LOCKED(-1) { ... }`.
  The macro uses a `cleanup` handler, so it unlocks on `return` / `break` /
  `goto` — a hand-written `ring_ui_lock()`/`ring_ui_unlock()` pair will leak
  the lock on an early exit. Do not "simplify" the macro: the unlock lives in
  the loop **increment**, and setting `.ok = false` there instead leaks the
  mutex on the normal path, which freezes the `lvgl_adapter` worker task and
  stops all repaints without any error (see `README.md` § Architecture —
  "Lock discipline").
- Mandatory init uses `ESP_ERROR_CHECK`; optional peripherals (SD, IMU) fail
  soft and are reported in `ring_board_t`.
- Log with `ESP_LOGI/LOGW/LOGE` and a `TAG`.
- Use `ring_rand_*`, not `esp_random()` — the deterministic RNG is the point.
- Use `ring_secret_get()` for credentials; NVS wins, else the build-time value
  from `ring_secrets.h` (which is then persisted).
- `ring_button` callbacks run on the `ring-button` task, never the LVGL task.
- `ring_geodis` callbacks never run on the peer/DTLS task: `state_cb` and
  `text_cb` are delivered in order on `ring-geodis-event` (6 KB, so LVGL is
  fine), `tool_cb` on `ring-geodis-tool` (12 KB, before the blocking RAG POST).
  The event queue stores transitions, not a state cell, so
  `READY → LISTENING → SPEAKING` arrives as three callbacks.
  `ring_geodis_set_listening(false)` closes the uplink but not the model's
  reply: state may go `LISTENING → SPEAKING` without a release.
  Take `RING_UI_LOCKED(-1)` before touching LVGL; `ring_button_is_pressed()` /
  `ring_button_held_ms()` are lock-free snapshots for a UI timer.

## Voice transport decision guide

| | `ring_chat` | `ring_ws` | `ring_net` | `ring_geodis` |
|---|---|---|---|---|
| Transport | HTTPS REST (TCP/443) | WebSocket (TCP/443) | WebRTC (UDP/DTLS + RTP) | WebRTC (UDP/DTLS + RTP) |
| Audio | 48k PCM in/out, resampled internally | 48k PCM in/out, resampled internally | 48k PCM, Opus over RTP | 48k PCM, Opus over RTP |
| Turn-taking | none (3 sequential calls) | server_vad / smart_turn_v2 / PTT | server_vad only | PTT (`turn_detection:null` + commit) / server_vad |
| Works on UDP-blocked nets | yes | yes | **no** (DTLS times out) | **no** (DTLS times out) |
| Latency | highest (3 round trips) | low | lowest | lowest |
| Extra deps | `esp_http_client` | `esp_websocket_client` | `esp_peer`, `esp_audio_codec` | `esp_peer`, `esp_audio_codec` |
| Tool calls | — | app-executed | — | **client-executed RAG** (`/tool-call`) |

- **Default to `ring_ws`** for interactive voice: it is the only realtime
  transport that works where UDP is blocked, and it supports push-to-talk.
- Use **`ring_net`** only when UDP is known to work and the lowest latency
  matters.
- Use **`ring_geodis`** for the GEODIS Voice Agent (Azure OpenAI Realtime).
  Same WebRTC constraints as `ring_net`, but the ephemeral-token signalling,
  the GA session schema and the client-executed RAG tool call are specific to
  it.
- Use **`ring_chat`** for the simplest possible cascade, or when only
  TCP/443 is available and latency is acceptable.

All three share the same 48 kHz / 16-bit / mono PCM interface and the same
CA / bearer plumbing, so switching transports does not change the audio
pipeline at the app boundary.

### Developing `ring_geodis` without a network

`CONFIG_RINGKIT_GEODIS_MOCK=y` swaps the network half of `ring_geodis` (Wi-Fi,
SNTP, `/token`, SDP, DTLS, RTP) for a local mock. The public API, the state
machine, the event queue, the downlink ring, the tool task and the callbacks are
the real code, so an app written against the mock is the app that runs against
the gateway.

```
start()              CONNECTING -> READY     ~300 ms
set_listening(true)  LISTENING
commit()             user transcript -> SPEAKING
                     "Je vérifie votre colis…" -> tool call (fails offline, by design)
                     1.6 s of canned 48 kHz PCM -> READY
cancel()             playback stops at the next frame boundary
```

The `/tool-call` POST fails on purpose: the app exercises the real
`lookup_unavailable` path instead of a fabricated answer. Timing is adjustable
via `ring_geodis_mock_config()`. Costs ~150 KB of flash (the embedded clip) —
never ship it.

## Build hygiene

- `components/ringkit/Kconfig` gates the five optional modules:
  `CONFIG_RINGKIT_NET`, `_WS`, `_CHAT`, `_GEODIS`, `_BT` (all `default y`).
  Disable the ones an app does not use to shrink the build; the shared leaves
  stay compiled so any combination links. Unused module code is dropped by
  the linker's `--gc-sections`, so an app that never calls `ring_geodis_*`
  does not pay for it.
- A component stays in `REQUIRES` only when a ringkit public header exposes
  one of its types. Everything else is `PRIV_REQUIRES`, so dependents do not
  inherit unused include paths. (`ring_http.h` exposes
  `esp_http_client_handle_t`, so `esp_http_client` is public.)

## Tests

`tests/` builds the pure-logic modules on the host against minimal ESP-IDF /
FreeRTOS stubs:

```bash
cd tests && make check
```

Covers `ring_json`, `ring_resample`, `ring_byte_ring` and the
`ring_chat` message builder. See `tests/README.md`.

## Related docs

- `notes/hardware-bsp-reference.md` — hardware + BSP reference, pin map.
- `examples/README.md` — the example contract and app table.
