# Examples

Each directory here is a self-contained application for the VIEWE Smart Ring
Plus. Exactly **one** is compiled into a firmware image, selected by the
`RING_APP` CMake cache variable (default `00_glyph_grid_reveal`).

| App | Description |
|-----|-------------|
| `00_glyph_grid_reveal` | Full-screen glyph grid with a layered center-out reveal and hue-cycling accents. Touch to open/close. |
| `01_gbuddy_face` | The GEODIS buddy face: four expressions cut out of `medias/gbuddy-00N.png` as alpha sprites — blinks, IMU gaze, twist look, sleep on hold, battery mood. See `01_gbuddy_face/README.md`. |
| `02_imu_dashboard` | Live IMU dashboard: bubble level (tilt) + accel (m/s²) + gyro (°/s) + temperature + level calibration. Tap to calibrate. See `02_imu_dashboard/README.md`. |
| `03_gesture_probe` | Touch gesture probe: shows the name of every single-finger gesture (tap / hold / swipe left/right/up/down, plus press/release) the moment it happens, with a dot following the finger. See `03_gesture_probe/README.md`. |
| `04_voice_loopback` | Mic + speaker loop-back: one round button — hold to record (ES8311 mic), release to play the captured PCM back through the speaker. Captures up to 8 s into PSRAM. See `04_voice_loopback/README.md`. |
| `05_wifi_https_demo` | Wi-Fi station bring-up: connects to the NVS-stored SSID, shows state/SSID/IP on the round screen, then probes `https://baidu.com` over HTTPS. Credentials are seeded from `secrets.local` on first boot, then persisted to NVS. See `05_wifi_https_demo/README.md`. |
| `06_qianwenai_push_to_talk` | Push-to-talk voice assistant: one green round button, hold to talk; the ring runs STT → LLM → TTS as three HTTPS calls (`ring_chat`) and speaks the reply. Works on UDP-blocked networks. See `06_qianwenai_push_to_talk/README.md`. |
| `07_qianwen_realtime_harness` | Hands-free voice loop: Wi-Fi → SNTP → HTTPS probe → WebRTC session, then mic → Opus → `qwen-audio-3.1-realtime-flash` → Opus → speaker, with server-VAD turn-taking. See `07_qianwen_realtime_harness/README.md`. |
| `08_ble_peripheral` | Bluetooth LE peripheral: advertises as `SmartRing`, exposes a vendor service (0xFFE0) with a readable name/battery (READ/NOTIFY) and a writable command pipe (WRITE/NOTIFY). Pairable/bondable; scan + connect with a phone (nRF Connect / LightBlue). See `08_ble_peripheral/README.md`. |
| `09_hello_smoke_test` | Bring-up smoke test: white screen, one large "Hello" glyph. See `09_hello_smoke_test/README.md`. |
| `10_qianwen_ws_voice_chat` | **WebSocket voice chat** with `qwen-audio-3.1-realtime-flash`: PCM 16 kHz in / 24 kHz out over `esp_websocket_client`, push-to-talk / `server_vad` / `smart_turn_v2` turn-taking (Set screen, NVS-persisted), barge-in, orb with radial level ring, Talk/Chat/Set screens, wire/battery stats. The only realtime transport that works where UDP is blocked. |
| `12_ringkit_selftest` | Bring-up diagnostic: probes display/touch/audio/IMU/SD/battery, then init/stop for Wi-Fi, BLE and the voice modules. See `12_ringkit_selftest/README.md`. |

> `11_quic_voice_chat/` is **planned** (design only, no buildable app yet) —
> see `11_quic_voice_chat/README.md`.

## Building an app

```bash
# default app (00_glyph_grid_reveal)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor

# a specific app — ALWAYS use its own build directory
idf.py -DRING_APP=09_hello_smoke_test -B build/09_hello_smoke_test build
idf.py -DRING_APP=09_hello_smoke_test -B build/09_hello_smoke_test -p /dev/ttyACM0 flash monitor
```

> **Use a per-app build directory (`-B build/<app>`).** Switching `RING_APP`
> inside one build tree leaves stale objects and produces confusing link
> errors. If it happens, `idf.py fullclean` and rebuild.

An unknown name fails fast at configure time with the list of valid choices.

## The contract every example implements

Each example directory is a **CMake fragment** (not an ESP-IDF component) with
three files, and exports exactly two symbols via its own `include/ring_example.h`:

```c
esp_err_t   ring_example_start(void);  /* build the UI; takes the LVGL lock */
const char *ring_example_name(void);   /* e.g. "09_hello_smoke_test" */
```

`main/main.c` is application-agnostic: it calls `ring_board_init()` and then
`ring_example_start()`. `main/CMakeLists.txt` `include()`s the selected
example's `CMakeLists.txt` and compiles its sources into the `main` component,
so every example can use the same `ring_example.h` name.

> **Why not a component?** ESP-IDF registers `EXTRA_COMPONENT_DIRS` components
> *after* `main`, and `__component_set_dependencies()` silently drops a
> `REQUIRES` entry whose component isn't registered yet — losing both the
> include path and the link, with a misleading error. Detail in
> `notes/architecture-rationale.md` §6.1.

## Adding an example

```
examples/10_my_app/
├── CMakeLists.txt          # sets RING_EXAMPLE_SRCS / _INCLUDE_DIRS / _REQUIRES
├── include/ring_example.h  # copy from an existing example (identical)
└── my_app.c                # ring_example_start() + ring_example_name()

## Adding a dependency

If your app needs a component that ringkit doesn't already require (e.g.
`esp_websocket_client`), add it to `main/idf_component.yml` — the single
manifest used by every app. Do **not** add an `idf_component.yml` under
examples/.
```

`examples/10_my_app/CMakeLists.txt` is three lines:

```cmake
set(RING_EXAMPLE_SRCS         "my_app.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")   # extra component deps, e.g. "esp_driver_ledc"
```

Then `idf.py -DRING_APP=10_my_app -B build/10_my_app build`.

Rules:

- Use the directory name for the app: `NN_short_description` (e.g.
  `10_my_app`), `[a-z0-9_]+`. The two-digit prefix fixes the order in this
  README and in `ls`; pick the next free number.
- **Do not** add an `idf_component.yml` per example. Managed dependencies live
  in `main/idf_component.yml`.
- An app may carry its own `README.md` for a design/spec (see
  `00_glyph_grid_reveal/README.md`) — it is inert to the build, so it never
  touches the CMake fragment.
- **Do not** include `bsp/smartring_plus.h` and **do not** call `bsp_*`
  directly — everything goes through `ringkit/ringkit.h`.
- All LVGL work happens under `RING_UI_LOCKED(-1) { ... }`.

## Skeleton

`components/ringkit/` wraps the whole BSP: `ring_board`, `ring_display`,
`ring_touch`, `ring_battery`, `ring_power`, `ring_audio`, `ring_imu`,
`ring_sd`, `ring_wifi`, `ring_bt`, the three voice transports (`ring_chat`
HTTPS cascade, `ring_ws` WebSocket, `ring_net` WebRTC), `ring_secret`, plus
`ring_ui` (lock/helpers), `ring_theme` (colours, geometry, timings) and
`ring_rand` (deterministic xorshift32).
