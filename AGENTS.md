# AGENTS.md

Guidance for AI coding agents and contributors working in this repository.

## Project overview

This is an **ESP-IDF** project for the **VIEWE "Smart Ring Plus"** — a round, 1.8-inch (360×360) capacitive-touch terminal based on the **ESP32-S3-N16R8** (8 MB octal PSRAM / 16 MB flash). The app boots all board peripherals through the official **`viewesmart/smartring_plus`** Board Support Package and renders a UI with **LVGL 9** via `esp_lvgl_adapter`.

Today it is structured as a **generic skeleton plus interchangeable applications**: `components/ringkit/` wraps the whole BSP, and each app under `examples/<name>/` is selected at build time with `RING_APP`.

Default firmware (`glyph-anime`): a full-screen 13×13 glyph grid with a layered center-out reveal and hue-cycling accents.

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

# default app (glyph-anime), default build dir
idf.py build
idf.py -p /dev/ttyACM0 flash       # flash + reset the ring
idf.py -p /dev/ttyACM0 monitor     # follow serial log (Ctrl-] to exit)

# any other app — ALWAYS its own build dir (see examples/README.md)
idf.py -DRING_APP=hello -B build/hello build
idf.py -DRING_APP=hello -B build/hello -p /dev/ttyACM0 flash
idf.py -B build/hello -p /dev/ttyACM0 monitor
```

> **Use a per-app build directory (`-B build/<app>`).** Switching `RING_APP` inside one
> build tree leaves stale objects and produces confusing link errors. Pass the **same**
> `-B` to `build`, `flash` and `monitor` — `flash` reads the image from that build dir,
> so a mismatched `-B` silently flashes something other than what you just built.

> **Which app is on the device?** Read the boot log — it prints `app: <name>`:
> ```
> I (963) smartring: Smart Ring Plus starting (ringkit 1.0.0, app: hello)
> I (1929) smartring: app 'hello' is on screen
> ```

> **An invalid `RING_APP` fails fast** at configure time and lists the choices:
> `RING_APP='nope' is not an example: no examples/nope/CMakeLists.txt.` → `Available: glyph-anime, hello`

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
├── sdkconfig.defaults           # known-good config (see below)
├── main/
│   ├── CMakeLists.txt           # includes examples/${RING_APP}/CMakeLists.txt
│   ├── idf_component.yml        # the ONLY dependency manifest
│   └── main.c                   # app-agnostic: board init + ring_example_start()
├── components/
│   ├── ringkit/                 # GENERIC SKELETON — wraps the whole BSP
│   └── espressif__esp_codec_dev/ # VENDORED OVERRIDE (see "Critical fixes")
├── examples/
│   ├── README.md
│   ├── glyph-anime/             # default app: layered glyph-grid reveal
│   └── hello/                   # bring-up smoke test (white screen + label)
└── notes/
    ├── esp-smartring.md         # product research + full BSP API reference
    ├── launcher-spec.md         # glyph-anime design document
    ├── restructure-proposal.md  # how this layout came about (§10 = the two IDF traps)
    ├── meta-muse.md             # research: Meta Muse gadget path (no code yet)
    └── xiaozhi.md               # research: open-source Xiaozhi assistant (no code yet)
```

### Skeleton (`components/ringkit/`)

Applications **never** include `bsp/smartring_plus.h` and never call `bsp_*` directly.
Everything goes through `ringkit/ringkit.h`:

- `ring_board_*` — bring-up in the required order + a `ring_board_t` summary
- `ring_display_*`, `ring_touch_*`, `ring_battery_*`, `ring_power_*`,
  `ring_audio_*`, `ring_imu_*`, `ring_sd_*` — one-to-one BSP wrappers
- `ring_ui_*` — LVGL lock + `RING_UI_LOCKED(-1) { ... }` scoped lock, label
  creation, exact centring
- `ring_theme.h` — colours, panel geometry, motion timings
- `ring_rand_*` — deterministic xorshift32

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

### Key ringkit APIs

- Board: `ring_board_init()`, `ring_board_log_summary()`, `ring_board_i2c_bus()`
- Display: `ring_display_backlight()`, `ring_display_handle()`, `ring_display_set_swap_bytes()`
- Touch: `ring_touch_handle()`
- Battery: `ring_battery_get()`, `ring_battery_percent()`, `ring_battery_voltage()`, `ring_battery_is_charging()`, `ring_battery_pmic_type()`
- Power: `ring_power_shutdown()` (holds GPIO47 ~3.5 s; normally doesn't return)
- Audio: `ring_audio_codec()`, `ring_audio_read()`, `ring_audio_write()`, `ring_audio_set_mute()`
- IMU: `ring_imu_accel()`, `ring_imu_tilt()`, `ring_imu_calib_start/poll/info()`
- SD: `ring_sd_list()`, `ring_sd_free_list()`, `ring_sd_is_mounted()`, `ring_sd_card()`, `ring_sd_deinit()`
- UI: `ring_ui_screen()`, `ring_ui_add_label()`, `ring_ui_place_centered()`, `ring_ui_lock()`, `ring_ui_unlock()`
- RNG: `ring_rand_seed()`, `ring_rand_u32()`, `ring_rand_below()`, `ring_rand_bool()` — do **not**
  replace with `esp_random()`; determinism is the point (a plain LCG breaks `% n` / `& 1`).

## Critical fixes already applied — DO NOT revert

Two things are **required** for this project to build and run. Keep them intact.

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
- **Do not** add an `idf_component.yml` under `examples/`; the single manifest is
  `main/idf_component.yml`.
- Keep `notes/esp-smartring.md` in sync when you change pin/API assumptions.
- Adding an app: copy an existing `examples/<name>/` (three files) and build with
  `idf.py -DRING_APP=<name> -B build/<name> build`. See `examples/README.md`.

## Verifying a change

Minimum bar before you claim something works — do all of it, from clean dirs when
you touched CMake, `sdkconfig.defaults`, or `ringkit` headers:

1. **Both apps build** (a change that only builds the app you care about can still
   break the other one, since both share `ringkit`):
   ```bash
   rm -rf build/glyph-anime build/hello
   idf.py -B build/glyph-anime -DRING_APP=glyph-anime build
   idf.py -B build/hello     -DRING_APP=hello       build
   ```
2. **The vendored codec override still wins** (see "Critical fixes").
3. **Flash and read the boot log** — expect the `ringkit:` summary line with all
   peripherals `ok`, then the app's own `ready` line, and **no** `panic` /
   `assert` / `Guru Meditation`.
4. **Touch paths need a human.** There is no way to synthesise a tap from the
   host: animations and gestures must be checked on the device. Do not report a
   UI interaction as verified from the log alone — say it needs a visual check.

## Repo hygiene

Tracked: source, `CMakeLists.txt`, `sdkconfig.defaults`, `AGENTS.md`, `notes/`,
`components/` (incl. the vendored codec override).

Git-ignored on purpose: `build/`, `sdkconfig`, `sdkconfig.old`, `dependencies.lock`,
`managed_components/`. Never commit a resolved `sdkconfig` — `sdkconfig.defaults`
is the source of truth.

`managed_components/` is regenerated by the component manager; the one exception
that must stay committed is the override *copy* in `components/`.

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
- **`hello` is bigger than `glyph-anime`** (~800 KB vs ~729 KB). Not a regression: it is
  the only app using `lv_font_montserrat_48`, which pulls in the 48 px font tables. App
  partition is 1 MB, so both fit with room to spare — but think before adding another
  large font.
- **Stale `build/` after the rename.** The project used to be `smartring_hello`; an old
  `build/` may still hold that artifact. If CMake behaves strangely, `rm -rf build build/*`
  and reconfigure.

## Flashing / reset notes

- Hold **BOOT (GPIO0)** at power-up to enter download mode if the console doesn't auto-enumerate.
- The device uses **native USB** (GPIO19/20), so the port is the USB-Serial/JTAG (`/dev/ttyACM0`).
- If the port is not writable, add the user to `uucp` (`sudo usermod -aG uucp $USER`) or `sudo chmod 666 /dev/ttyACM0` for a one-off.

## Deep-dive docs

- `notes/esp-smartring.md` — full product research, BSP API reference, pin map, and both fix write-ups.
- `notes/launcher-spec.md` — glyph-anime design: layered reveal maths, layer timeline, risks.
- `notes/restructure-proposal.md` — why the layout is `ringkit` + fragments; §10 has the two
  ESP-IDF traps (EXTRA_COMPONENT_DIRS ordering, double CMake evaluation).
- `notes/meta-muse.md`, `notes/xiaozhi.md` — assistant feasibility research; no code yet.
- `examples/README.md` — app contract and how to add one.
- BSP upstream: `https://github.com/VIEWESMART/smartring_plus` (component `viewesmart/smartring_plus` on the ESP Component Registry).
