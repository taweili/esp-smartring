# AGENTS.md

Guidance for AI coding agents and contributors working in this repository.

## Project overview

This is an **ESP-IDF** project for the **VIEWE "Smart Ring Plus"** — a round, 1.8-inch **360×360 px** capacitive-touch terminal (ST77916 IPS over QSPI) based on the **ESP32-S3-N16R8** (8 MB octal PSRAM / 16 MB flash). The app boots all board peripherals through the official **`viewesmart/smartring_plus`** Board Support Package and renders a UI with **LVGL 9** via `esp_lvgl_adapter`.

Today it is structured as a **generic skeleton plus interchangeable applications**: `components/ringkit/` wraps the whole BSP, and each app under `examples/<name>/` is selected at build time with `RING_APP`.

Default firmware (`00_glyph_grid_reveal`): a full-screen 13×13 glyph grid with a layered center-out reveal and hue-cycling accents.

## Environment

- **ESP-IDF:** v6.1 (`/home/david/.espressif/v6.1/esp-idf`, `IDF_PATH` already exported)
- **Target:** `esp32s3`
- **Toolchain:** Xtensa ESP32-S3 (installed under `~/.espressif/tools`)
- `idf.py` is on `PATH` (`~/.espressif/tools/shims/v6.1`)
- Device serial port: **`/dev/ttyACM0`** (Espressif USB-JTAG, VID `303a`/PID `1001`). `/dev/ttyUSB0` is an unrelated CH340 device — do not use it for the ring.
- Serial console speed: **115200 baud**

## Commands

```bash
idf.py set-target esp32s3          # first time / after deleting build dir
idf.py reconfigure                 # after changing idf_component.yml or sdkconfig.defaults

# default app (00_glyph_grid_reveal), default build dir
idf.py build
idf.py -p /dev/ttyACM0 flash       # flash + reset the ring
idf.py -p /dev/ttyACM0 monitor     # follow serial log (Ctrl-] to exit)

# any other app — ALWAYS its own build dir (see examples/README.md)
idf.py -DRING_APP=09_hello_smoke_test -B build/09_hello_smoke_test build
idf.py -DRING_APP=09_hello_smoke_test -B build/09_hello_smoke_test -p /dev/ttyACM0 flash
idf.py -B build/09_hello_smoke_test -p /dev/ttyACM0 monitor

# host-side unit tests (no hardware, no idf.py)
cd tests && make check
```

> **Use a per-app build directory (`-B build/<app>`).** Switching `RING_APP` inside one
> build tree leaves stale objects and produces confusing link errors. Pass the **same**
> `-B` to `build`, `flash` and `monitor` — `flash` reads the image from that build dir,
> so a mismatched `-B` silently flashes something other than what you just built.

> **Which app is on the device?** Read the boot log — it prints `app: <name>`:
> ```
> I (963) smartring: Smart Ring Plus starting (ringkit 1.0.0, app: 09_hello_smoke_test)
> I (1929) smartring: app '09_hello_smoke_test' is on screen
> ```

> **An invalid `RING_APP` fails fast** at configure time and lists the choices:
> `RING_APP='nope' is not an example: no examples/nope/CMakeLists.txt.` → `Available: 00_glyph_grid_reveal, 09_hello_smoke_test`

> **`sdkconfig` and `sdkconfig.defaults` are shared by every build dir** — they live at the
> project root, not per app. So a config change affects all apps; there is no per-app config.
>
> **`sdkconfig.defaults` is only applied when `sdkconfig` is (re)created.** If you change
> `sdkconfig.defaults`, run `rm -f sdkconfig && idf.py reconfigure` so the new defaults take effect.

To capture the boot log non-interactively (e.g. from a script), reset the board and read the port.

> **⚠️ You must drain the port continuously.** The console is the USB-Serial-JTAG port,
> and device log writes **block when the host is not reading**. A script that sleeps
> (e.g. `time.sleep(10)`) *before* reading lets the TX FIFO fill, which stalls the
> firmware mid-boot and looks exactly like a hang/crash. Nesting `time.sleep()` inside
> `lv_*` work has caused hours of phantom bugs here.
> Always read in a background thread (or use `idf.py monitor`):

```bash
python3 - <<'EOF'
import serial, time, threading
p = serial.Serial("/dev/ttyACM0", 115200, timeout=0.1)
buf = bytearray(); stop = False

def reader():                      # drain continuously — do NOT sleep first
    while not stop:
        try:
            d = p.read(4096)
            if d: buf.extend(d)
        except Exception:
            pass

threading.Thread(target=reader, daemon=True).start()

p.setDTR(False); p.setRTS(True); time.sleep(0.1); p.setRTS(False)
time.sleep(0.2); p.setDTR(True); time.sleep(0.1); p.setDTR(False)
time.sleep(5)
stop = True; time.sleep(0.3)
print(buf.decode("utf-8", "replace"))
EOF
```

## Architecture

```
.
├── CMakeLists.txt               # RING_APP selection, then project(smartring)
├── README.md                    # project entry: overview, quick start, apps, docs map
├── sdkconfig.defaults           # known-good config (see below)
├── main/
│   ├── CMakeLists.txt           # includes examples/${RING_APP}/CMakeLists.txt
│   ├── idf_component.yml        # the ONLY dependency manifest
│   └── main.c                   # app-agnostic: board init + ring_example_start()
├── components/
│   ├── ringkit/                 # GENERIC SKELETON — wraps the whole BSP (README.md, Kconfig)
│   ├── espressif__esp_codec_dev/ # VENDORED OVERRIDE (see "Critical fixes")
│   └── viewesmart__smartring_plus/ # VENDORED OVERRIDE (gyro+temp, 02_imu_dashboard)
├── examples/
│   ├── README.md                # app contract + the app table
│   ├── 00_glyph_grid_reveal/     # default app: layered glyph-grid reveal (README)
│   ├── 01_gbuddy_face/          # animated buddy face (sprite assets in assets/, README)
│   ├── 02_imu_dashboard/        # live IMU dashboard (accel/gyro/temp/tilt, README)
│   ├── 03_gesture_probe/        # single-finger gesture probe (tap/hold/swipe, README)
│   ├── 04_voice_loopback/       # mic+speaker loop-back (hold to record, README)
│   ├── 05_wifi_https_demo/      # Wi-Fi STA + HTTPS probe (README)
│   ├── 06_qianwenai_push_to_talk/   # hold-to-talk STT→LLM→TTS cascade (Qianwen, HTTPS; README)
│   ├── 07_qianwen_realtime_harness/ # hands-free WebRTC voice loop (README)
│   ├── 08_ble_peripheral/       # Bluetooth LE peripheral (README)
│   ├── 09_hello_smoke_test/     # bring-up smoke test (white screen + label, README)
│   ├── 10_qianwen_ws_voice_chat/ # WebSocket voice chat: PCM 16k in / 24k out,
│   │                            #   esp_websocket_client, PTT + hands-free modes,
│   │                            #   orb + chat/settings screens (README)
│   ├── 11_quic_voice_chat/      # PLANNED (design only) — notes/design-quic-voice-chat.md
│   └── 12_ringkit_selftest/     # subsystem diagnostic: probe + voice init/stop (README)
├── scripts/
│   ├── test_qianwen_realtime.py # host-side Qianwen WebRTC test client
│   └── assets/test_clip.pcm     # clip for its --clip mode (no mic needed)
├── tests/                       # HOST UNIT TESTS — pure-logic ringkit modules (README)
├── tools/
│   ├── dev.sh                   # build/flash/monitor wrapper (secrets + IDF env)
│   └── lvgl_img_pack.py         # RGB888(+A8) -> lv_image_dsc_t C file (gbuddy assets)
└── notes/
    ├── hardware-bsp-reference.md     # hardware + BSP reference (product, BSP API, pin map)
    ├── architecture-rationale.md     # architecture rationale (§6 = the two IDF traps)
    ├── networking-https-webrtc.md    # ESP-IDF v6.1 networking/HTTPS/WebRTC reference
    ├── qianwen-ai-platform.md        # 千问 AI platform model catalog + API surface
    ├── design-ws-voice-chat.md       # design: examples/10_qianwen_ws_voice_chat (no code yet)
    ├── design-quic-voice-chat.md     # design: examples/11_quic_voice_chat (no code yet)
    ├── research-meta-muse.md         # research: Meta Muse gadget path (no code yet)
    ├── research-xiaozhi.md           # research: open-source Xiaozhi assistant (no code yet)
    └── plan-ringkit-refactor.md      # ringkit refactor plan + progress log (phases 1–4)
```

### Skeleton (`components/ringkit/`)

Applications **never** include `bsp/smartring_plus.h` and never call `bsp_*` directly.
Everything goes through `ringkit/ringkit.h`:

- `ring_board_*` — bring-up in the required order + a `ring_board_t` summary
- `ring_display_*`, `ring_touch_*`, `ring_battery_*`, `ring_power_*`,
  `ring_audio_*`, `ring_imu_*`, `ring_sd_*` — one-to-one BSP wrappers
- `ring_wifi_*` — Wi-Fi STA with NVS-persisted credentials
- `ring_bt_*` — NimBLE BLE peripheral (declarative GATT DB, GAP state, notify)
- `ring_chat_*` / `ring_ws_*` / `ring_net_*` — the three voice transports
  (HTTPS cascade / WebSocket / WebRTC+Opus); see `components/ringkit/README.md`
- `ring_ca_*`, `ring_time_*`, `ring_nvs_*`, `ring_json_*`, `ring_byte_ring_*`,
  `ring_resample_*`, `ring_http_*` — shared infrastructure used by the above
- `ring_ui_*` — LVGL lock + `RING_UI_LOCKED(-1) { ... }` scoped lock, label
  creation, exact centring
- `ring_theme.h` — colours, panel geometry, motion timings
- `ring_rand_*` — deterministic xorshift32
- `ring_secret_*` — NVS-backed secrets with a build-time fallback

### Example contract

Each `examples/<name>/` provides `include/ring_example.h` and exports:

```c
esp_err_t   ring_example_start(void);  /* build UI; takes the LVGL lock itself */
const char *ring_example_name(void);
```

**Why examples are CMake fragments, not components:** ESP-IDF registers
`EXTRA_COMPONENT_DIRS` components *after* `main`, and
`__component_set_dependencies()` silently drops any `REQUIRES` entry whose
component target is not yet registered — which loses both the include path and
the link. So `main/CMakeLists.txt` `include()`s the selected example's
`CMakeLists.txt` (which just sets `RING_EXAMPLE_SRCS` / `_INCLUDE_DIRS` /
`_REQUIRES`) and compiles it into the `main` component.

> That `main/CMakeLists.txt` is evaluated **twice** (a requirements-gathering
> pass where cache vars are unset and `CMAKE_SOURCE_DIR` is the build dir, then
> the real configure), so it resolves the project root from
> `CMAKE_CURRENT_LIST_DIR` and falls back to the default app when `RING_APP` is
> empty. Keep it that way.

Board bring-up order lives in `ring_board.c`, not in `app_main`.

### Required init order (`ring_board_init()`)

`bsp_display_init()` creates the shared I2C bus and starts the LVGL task, so it **must be first**:

1. `bsp_display_init()`  — I2C + CST816 touch + ST77916 QSPI panel + `esp_lvgl_adapter` + backlight 100%
2. `bsp_battery_init()`  — probes AXP2101 (V2) or GPIO1 ADC (V1)
3. `bsp_power_init()`    — soft power-off GPIO
4. `bsp_audio_init()`    — ES8311 + I2S (48 kHz / 16-bit / stereo)
5. `bsp_sd_init()`       — SDMMC + FAT (`/sdcard`); non-fatal (no card-detect pin)
6. `bsp_imu_init()`      — QMI8658A; non-fatal

Display/battery/power/audio are mandatory (first failure is returned); SD and
IMU fail soft and are reported in `ring_board_t`.

All LVGL object creation/updates **must** happen inside
`RING_UI_LOCKED(-1) { ... }` (or an explicit `ring_ui_lock()`/`ring_ui_unlock()` pair).

### Display geometry (360×360 round panel)

The panel is **360×360 px**, 1.8" round IPS (ST77916 over QSPI), centre
`(180, 180)`, radius `180`. It is a **square display with a circular active
area**: the four square corners are outside the round glass and are never
visible. UI design must account for this:

- Keep text, icons and touch targets inside the **safe circle**
  (`RING_SAFE_RADIUS`, ~172 px) — never in the corners, and not flush against
  the edge.
- Never assume a rectangle maps to the screen: a full-width row at the
  top/bottom loses its ends, and a four-corner status bar is mostly invisible.
- Use the shared geometry in `components/ringkit/include/ringkit/ring_theme.h`
  (`RING_PANEL_W/H`, `RING_CX/CY`, `RING_RADIUS`, `RING_SAFE_RADIUS`,
  `RING_IN_CIRCLE()`) instead of hardcoding 360/180.
- Design in panel coordinates; `ring_ui_place_centered()` centres exactly.

### Key ringkit APIs

- Board: `ring_board_init()`, `ring_board_log_summary()`, `ring_board_i2c_bus()`
- Display: `ring_display_backlight()`, `ring_display_handle()`, `ring_display_set_swap_bytes()`
- Touch: `ring_touch_handle()`
- Battery: `ring_battery_get()`, `ring_battery_percent()`, `ring_battery_voltage()`, `ring_battery_is_charging()`, `ring_battery_pmic_type()`
- Power: `ring_power_shutdown()` (holds GPIO47 ~3.5 s; normally doesn't return)
- Audio: `ring_audio_codec()`, `ring_audio_read()`, `ring_audio_write()`, `ring_audio_set_mute()`,
  `ring_audio_stereo_to_mono()`, `ring_audio_mono_to_stereo()` (the codec is stereo; voice work is mono)
- IMU: `ring_imu_accel()`, `ring_imu_tilt()`, `ring_imu_calib_start/poll/info()`,
  `ring_imu_gyro()`, `ring_imu_temp()` (the last two come from the vendored BSP override)
- SD: `ring_sd_list()`, `ring_sd_free_list()`, `ring_sd_is_mounted()`, `ring_sd_card()`, `ring_sd_deinit()`
- UI: `ring_ui_screen()`, `ring_ui_add_label()`, `ring_ui_place_centered()`, `ring_ui_lock()`, `ring_ui_unlock()`
- Wi-Fi: `ring_wifi_init()`, `ring_wifi_start()`, `ring_wifi_wait_connected()`, `ring_wifi_get_ip()`,
  `ring_wifi_get_credentials()`, `ring_wifi_state()`
- BT: `ring_bt_init()`, `ring_bt_start_advertising()`, `ring_bt_stop_advertising()`,
  `ring_bt_notify()`, `ring_bt_state()`, `ring_bt_is_connected()`, `ring_bt_set_name()`
- WS/voice (`ring_ws.h`): `ring_ws_init()`, `ring_ws_start(model, key, cfg)`, `ring_ws_stop()`,
  `ring_ws_set_listening()`, `ring_ws_send_audio()`, `ring_ws_read_audio()`, `ring_ws_commit()`,
  `ring_ws_state()`, `ring_ws_get_stats()`
- Chat/voice (`ring_chat.h`): `ring_chat_init()`, `ring_chat_stt()`, `ring_chat_llm_complete()`,
  `ring_chat_llm_stream()`, `ring_chat_tts()`, `ring_chat_add_user()`, `ring_chat_add_assistant()`
- Net/voice (`ring_net.h`): `ring_net_init()`, `ring_net_start(model, key)`, `ring_net_sync_clock()`,
  `ring_net_probe(url)`, `ring_net_set_listening()`, `ring_net_send_audio()`, `ring_net_read_audio()`,
  `ring_net_state()`, `ring_net_send_event()`, `ring_net_ca_pem()`
- Shared infra: `ring_ca_pem()`, `ring_time_sync()`, `ring_nvs_init()`, `ring_nvs_get_str()`,
  `ring_nvs_set_str()`, `ring_json_get_str()`, `ring_json_append()`, `ring_json_append_string()`,
  `ring_byte_ring_*()`, `ring_resample_*()`, `ring_http_init()`, `ring_http_read_body()`,
  `ring_http_sse_line()`
- RNG: `ring_rand_seed()`, `ring_rand_u32()`, `ring_rand_below()`, `ring_rand_bool()` — do **not**
  replace with `esp_random()`; determinism is the point (a plain LCG breaks `% n` / `& 1`).
- Secrets: `ring_secret_get(ns, key, build_fallback, out, len)` — NVS wins, else the
  build-time value from `ring_secrets.h`, which is then persisted. Replaces the
  per-app `load_api_key()` copies.

## Critical fixes already applied — DO NOT revert

Three things are **required** for this project to build and run. Keep them intact.

### 1. Vendored `esp_codec_dev` override (ESP-IDF v6 compatibility)

The BSP pins `espressif/esp_codec_dev ==1.3.4`, which targets ESP-IDF v5. In **ESP-IDF v6** the `driver/gpio.h`, `driver/i2c_master.h`, `driver/i2s_std.h`, `driver/spi_common.h` headers moved into split `esp_driver_*` components, so v1.3.4 fails to compile:

```
fatal error: driver/gpio.h: No such file or directory
```

The repo carries a **local override** at `components/espressif__esp_codec_dev/` (a copy of 1.3.4). Project `components/` outrank `managed_components/` per the IDF build-system docs, so this is used even if the manager re-downloads the original. Two patches:

- `CMakeLists.txt`: `PRIV_REQUIRES freertos esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi`
- `platform/audio_codec_ctrl_spi.c`: added `#include "esp_idf_version.h"` (the `ESP_IDF_VERSION` macros are no longer pulled in transitively)

Verify the override is the one compiled (use the same `-B` dir you build in):

```bash
grep -oE "(components|managed_components)/espressif__esp_codec_dev" \
    build/<app>/compile_commands.json | sort | uniq -c
# expect:  204 components/espressif__esp_codec_dev   (0 managed_components/)
```

### 2. `CONFIG_FREERTOS_HZ=1000` (boot crash fix)

The BSP's IMU sample task runs at 250 Hz via `vTaskDelayUntil(&last, pdMS_TO_TICKS(4))` (`bsp_imu.c:89`). At the default **100 Hz** FreeRTOS tick that rounds to **0 ticks**, asserting `xTaskDelayUntil tasks.c:1499 ((xTimeIncrement > 0U))` and causing a boot loop right after IMU init. This must stay `1000`:

```ini
# sdkconfig.defaults
CONFIG_FREERTOS_HZ=1000
```

### 3. Vendored `smartring_plus` BSP override (gyro + temperature exposure)

The stock BSP's IMU sample task reads **only the accelerometer** registers
(`qmi8658_read_accel`, 6 bytes at `0x35..0x3A`); the gyroscope
(`0x3B..0x40`) and temperature (`0x33..0x34`) are enabled in hardware but never
read or exposed. The `02_imu_dashboard` example needs all of them, so the repo carries
an **override copy** at `components/viewesmart__smartring_plus/` (project
`components/` outrank `managed_components/`). One patch to `bsp_imu.c`:

- the 250 Hz sample task now uses `qmi8658_read_sensor_data()` — a single
  12-byte burst returns accel + gyro (consistent snapshot) plus temperature —
  and stores all of them under the existing lock;
- `bsp_imu_get_gyro(float*,float*,float*)` and `bsp_imu_get_temp(float*)` were
  added to `smartring_plus.h`/`bsp_imu.c`; ringkit wraps them as
  `ring_imu_gyro()` / `ring_imu_temp()`.

Gyro is ±512 dps (driver default, unit `°/s`); temperature is `raw/256` °C
(QMI8658A: 1 LSB = 1/256 °C, no offset).

Verify the override is the one compiled (use the same `-B` dir you build in):

```bash
grep -oE "(components|managed_components)/viewesmart__smartring_plus" \
    build/<app>/compile_commands.json | sort | uniq -c
# expect:  34 components/viewesmart__smartring_plus   (0 managed_components/)
```

## sdkconfig essentials

`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n`, `CONFIG_LV_USE_CLIB_MALLOC=y`, `CONFIG_FREERTOS_HZ=1000`, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, and Montserrat fonts (`14`, `16`, `20`, `24`, `48`).

> `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` matters: the board's primary console used to be
> UART0 on GPIO43/44, which is **not exposed**, so panic/WDT backtraces went nowhere and
> crashes were invisible. With USB-JTAG as the console, panics appear on `/dev/ttyACM0`.

## Conventions

- **C code** in `main/` or `examples/<app>/`; keep it in small helpers. No C++.
- Applications talk to hardware **only** through `ringkit` (`ringkit/ringkit.h`).
  Do **not** include `bsp/smartring_plus.h` and do **not** call `bsp_*` in app code, and
  don't touch `board_config.h` pins.
- Wrap all LVGL calls in `RING_UI_LOCKED(-1) { ... }` — a hand-written
  `esp_lv_adapter_lock()`/`unlock()` pair leaks the lock on early `return`.
- Log with `ESP_LOGI/LOGW/LOGE` using a `TAG`.
- Prefer `ESP_ERROR_CHECK` for mandatory init; soft-fail optional peripherals (SD, IMU) —
  `ring_board_init()` already encodes this split.
- **Design for the round panel.** 360×360 px with a circular active area —
  keep critical content inside `RING_SAFE_RADIUS` (see "Display geometry").
- **Do not** add an `idf_component.yml` under `examples/`; the single manifest is
  `main/idf_component.yml`.
- Host-side unit tests for the pure-logic modules live in `tests/`; run `make check`
  after changing `ring_json`, `ring_resample`, `ring_byte_ring` or the `ring_chat`
  message builder.
- Keep `notes/hardware-bsp-reference.md` in sync when you change pin/API assumptions.
- Adding an app: copy an existing `examples/<name>/` (three files) and build with
  `idf.py -DRING_APP=<name> -B build/<name> build`. See `examples/README.md`.

## Verifying a change

Minimum bar before you claim something works — do all of it, from clean dirs when
you touched CMake, `sdkconfig.defaults`, or `ringkit` headers:

1. **The apps build** — at minimum the two that bracket the dependency graph
   (`00_glyph_grid_reveal` has no extras, `09_hello_smoke_test` pulls the 48 px font):
   ```bash
   rm -rf build/00_glyph_grid_reveal build/09_hello_smoke_test
   idf.py -B build/00_glyph_grid_reveal -DRING_APP=00_glyph_grid_reveal build
   idf.py -B build/09_hello_smoke_test     -DRING_APP=09_hello_smoke_test       build
   ```
   If you touched `ringkit`, `sdkconfig.defaults` or `main/idf_component.yml`, also build the
   apps that exercise the changed surface — `01_gbuddy_face` (LVGL assets/IMU),
   `05_wifi_https_demo` (TLS), `06` (the `ring_chat` HTTPS cascade), and `07`
   (the only `ring_net`/Opus consumer).
   If you touched `ringkit`'s pure-logic modules, also run `cd tests && make check`.
2. **Both vendored overrides still win** — `espressif__esp_codec_dev` and
   `viewesmart__smartring_plus` (see "Critical fixes").
3. **Flash and read the boot log** — expect the `ringkit:` summary line with all
   peripherals `ok`, then the app's own `ready` line, and **no** `panic` /
   `assert` / `Guru Meditation`.
4. **Touch paths need a human.** There is no way to synthesise a tap from the
   host: animations and gestures must be checked on the device. Do not report a
   UI interaction as verified from the log alone — say it needs a visual check.
5. **Voice paths need a network.** `07` cannot complete a call where outbound UDP is
   dropped (DTLS times out — see `notes/networking-https-webrtc.md` §8). `06`/`10` are
   TCP/443-only and work on such a LAN, but TCP/443 working is not sufficient evidence
   that a voice turn works.

## Repo hygiene

Tracked: source, `CMakeLists.txt`, `sdkconfig.defaults`, `AGENTS.md`, `notes/`,
`components/` (incl. the vendored `esp_codec_dev` and `smartring_plus` overrides),
`examples/`, `tests/`.

Git-ignored on purpose: `build/`, `sdkconfig`, `sdkconfig.old`, `dependencies.lock`,
`managed_components/`, `tests/test_ring_*` (host test binaries). Never commit a resolved
`sdkconfig` — `sdkconfig.defaults` is the source of truth.

`managed_components/` is regenerated by the component manager; the only things
that must stay committed are the override *copies* in `components/`
(`espressif__esp_codec_dev`, `viewesmart__smartring_plus`).

## Gotchas

- **Console back-pressure looks like a hang (most important).** `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
  is set (see below), so `ESP_LOG*` writes block whenever the host stops reading `/dev/ttyACM0`.
  Drain the port continuously — a capture script that `sleep()`s before reading will stall the
  firmware mid-boot and mimic a crash. This has produced false "hangs" in SD init, IMU init,
  `bsp_power_init()`, and the LVGL render path.
- **Full-screen LVGL flush** (`buffer_height` = full panel) exhausts internal RAM → `ESP_ERR_NO_MEM`/garbage. The BSP uses a 40-line buffer; don't increase it in app code.
- **Audio pop/click:** always mute after playback (`set_mute(true)`); `esp_codec_dev_write()` returns `0` on success (inverted `esp_err_t`).
- **Panel revision:** the BSP uses the vendor "B" init table (`BOARD_LCD_VERSION_A`); a mismatch gives vertical stripes.
- **SD "no card" vs "mount failure"** can't be distinguished in software.
- **Reserved GPIOs:** 0 (BOOT), 19/20 (USB), 43/44 (UART0), 33–37 (PSRAM). Never reconfigure.
- **`09_hello_smoke_test` is bigger than `00_glyph_grid_reveal`** (~804 KB vs ~733 KB). Not a regression: it is
  the only app using `lv_font_montserrat_48`, which pulls in the 48 px font tables — but
  think before adding another large font.
- **Image sizes are large and the partition is 3000K.** Measured: `12` 2638 KB, `07` 2640 KB,
  `06` 1493 KB, `05` 1558 KB, `01` 1127 KB, `08` 1056 KB. `07`/`12` have only ~360 KB of
  headroom, so check `idf.py -B build/<app> size` before adding a dependency that every app
  links (WebRTC, Opus, a TLS stack).
- **A `ringkit` change reaches every app.** `ringkit`'s `PRIV_REQUIRES` still links `esp_peer`,
  `esp_audio_codec`, `esp_http_client` and `esp-tls` into every app (phase 3 moved them out of
  `REQUIRES`, but they are still linked); adding to it grows every image.
- **Stale `build/` after the rename.** The project used to be `smartring_hello`; an old
  `build/` may still hold that artifact. If CMake behaves strangely, `rm -rf build build/*`
  and reconfigure.

## Flashing / reset notes

- Hold **BOOT (GPIO0)** at power-up to enter download mode if the console doesn't auto-enumerate.
- The device uses **native USB** (GPIO19/20), so the port is the USB-Serial/JTAG (`/dev/ttyACM0`).
- If the port is not writable, add the user to `uucp` (`sudo usermod -aG uucp $USER`) or `sudo chmod 666 /dev/ttyACM0` for a one-off.

## Deep-dive docs

- `README.md` — project entry point: overview, quick start, applications, docs map.
- `components/ringkit/README.md` — ringkit module map, task/thread model, state machines, API
  contract and the voice-transport decision guide.
- `tests/README.md` — host-side unit tests: what they cover and how to add one.
- `examples/12_ringkit_selftest/README.md` — the subsystem diagnostic app.
- `notes/plan-ringkit-refactor.md` — the ringkit refactor plan and progress log (phases 1–4).
- `notes/hardware-bsp-reference.md` — hardware + BSP reference: product overview, BSP API, pin map, pitfalls.
- `examples/00_glyph_grid_reveal/README.md` — 00_glyph_grid_reveal design & build spec: layered reveal maths, layer timeline, risks.
- `notes/architecture-rationale.md` — architecture rationale: why the layout is `ringkit` + fragments; §6 has the two
  ESP-IDF traps (EXTRA_COMPONENT_DIRS ordering, double CMake evaluation).
- `notes/networking-https-webrtc.md` — networking, HTTPS & WebRTC research: ESP-IDF v6.1 stack, `esp_http_client`/`esp-tls`/
  cert bundle, HTTPS server, WebSocket/MQTT/mDNS, `esp_peer` WebRTC, and OpenAI API paths (REST, Realtime
  over WebSocket, Realtime over WebRTC).
- `notes/qianwen-ai-platform.md` — 千问 AI 平台 (qianwenai.com) reference: model catalog by category and the API surface
  (OpenAI-compatible, Anthropic-compatible, DashScope native, WebSocket/WebRTC/AOQ realtime, async/batch).
- `examples/06_qianwenai_push_to_talk/README.md` — hold-to-talk STT→LLM→TTS cascade (`ring_chat`):
  the three HTTPS stages, audio pipeline, task/memory layout, latency tradeoffs.
- `notes/design-ws-voice-chat.md` — WebSocket voice-chat design brainstorm for `examples/10_qianwen_ws_voice_chat`:
  PCM 16 kHz in / 24 kHz out resampler, `ring_ws` module, manual PTT / `smart_turn_v2` turn-taking, barge-in,
  function calling, stack + jitter-ring budget. The only realtime path that works on UDP-blocked networks.
- `notes/design-quic-voice-chat.md` — QUIC/AOQ research and the `ringgate` design for `examples/11_quic_voice_chat`;
  no code yet.
- `notes/research-meta-muse.md`, `notes/research-xiaozhi.md` — assistant feasibility research; no code yet.
- `examples/01_gbuddy_face/README.md` — the sprite-asset app: moods, blink, gaze, sleep, the
  on-device findings (flat background, feather, LVGL snapshot debugging) and how to regenerate
  the assets.
- `examples/README.md` — app contract and how to add one.
- BSP upstream: `https://github.com/VIEWESMART/smartring_plus` (component `viewesmart/smartring_plus` on the ESP Component Registry).
