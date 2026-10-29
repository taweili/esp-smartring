# AGENTS.md

Guidance for AI coding agents and contributors working in this repository.

## Project overview

This is an **ESP-IDF** project for the **VIEWE "Smart Ring Plus"** — a round, 1.8-inch (360×360) capacitive-touch terminal based on the **ESP32-S3-N16R8** (8 MB octal PSRAM / 16 MB flash). The app boots all board peripherals through the official **`viewesmart/smartring_plus`** Board Support Package and renders a UI with **LVGL 9** via `esp_lvgl_adapter`.

Current firmware: a "Hello World" demo — a large black `Montserrat 48` label centered on a white background.

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
idf.py build
idf.py -p /dev/ttyACM0 flash       # flash + reset the ring
idf.py -p /dev/ttyACM0 monitor     # follow serial log (Ctrl-] to exit)
```

> **sdkconfig.defaults is only applied when `sdkconfig` is (re)created.** If you change
> `sdkconfig.defaults`, run `rm -f sdkconfig && idf.py reconfigure` so the new defaults take effect.

To capture the boot log non-interactively (e.g. from a script), reset the board and read the port:

```bash
python3 - <<'EOF'
import serial, time
p = serial.Serial("/dev/ttyACM0", 115200, timeout=0.5)
p.setDTR(False); p.setRTS(True); time.sleep(0.1); p.setRTS(False)
time.sleep(0.2); p.setDTR(True); time.sleep(0.1); p.setDTR(False)
time.sleep(3); print(p.read(8192).decode("utf-8", "replace"))
EOF
```

## Architecture

```
.
├── CMakeLists.txt               # project(smartring_hello)
├── sdkconfig.defaults           # known-good config (see below)
├── main/
│   ├── CMakeLists.txt           # REQUIRES smartring_plus esp_lvgl_adapter lvgl
│   ├── idf_component.yml        # idf >=5.5 + viewesmart/smartring_plus ^1.1.2
│   └── main.c                   # app: BSP init sequence + LVGL UI
├── components/
│   └── espressif__esp_codec_dev/ # VENDORED OVERRIDE (see "Critical fixes")
└── notes/esp-smartring.md       # product research + full BSP API reference
```

The app talks to hardware **only** through the BSP's `bsp_*` API (`bsp/smartring_plus.h`); never hard-code pins or init sequences in app code. All pins/I2C addresses are in `include/bsp/board_config.h` of the BSP.

### Required init order (in `app_main`)

`bsp_display_init()` creates the shared I2C bus and starts the LVGL task, so it **must be first**:

1. `bsp_display_init()`  — I2C + CST816 touch + ST77916 QSPI panel + `esp_lvgl_adapter` + backlight 100%
2. `bsp_battery_init()`  — probes AXP2101 (V2) or GPIO1 ADC (V1)
3. `bsp_power_init()`    — soft power-off GPIO
4. `bsp_sd_init()`       — SDMMC + FAT (`/sdcard`); treat failure as non-fatal (no card-detect pin)
5. `bsp_imu_init()`      — QMI8658A; treat failure as non-fatal
6. `bsp_audio_init()`    — ES8311 + I2S (48 kHz / 16-bit / stereo)

All LVGL object creation/updates **must** happen inside `esp_lv_adapter_lock(-1)` … `esp_lv_adapter_unlock()`.

### Key BSP APIs

- Display: `bsp_display_init()`, `bsp_display_backlight_set(pct)`, `bsp_display_get_handle()`
- Touch: `bsp_touch_get_handle()`
- Shared I2C bus: `bsp_i2c_get_bus()`
- Battery: `bsp_battery_get_percent()`, `bsp_battery_get_voltage()`, `bsp_battery_is_charging()`, `bsp_battery_get_pmic_type()`
- Power: `bsp_power_shutdown()` (holds GPIO47 ~3.5 s; normally doesn't return)
- Audio: `bsp_audio_init()`, `bsp_audio_get_codec()`, `bsp_audio_read()`, `bsp_audio_write()`, `bsp_audio_set_mute()`
- IMU: `bsp_imu_init()`, `bsp_imu_get_accel()`, `bsp_imu_get_tilt()`, `bsp_imu_calib_*()`
- SD: `bsp_sd_init()`, `bsp_sd_list()`, `bsp_sd_free_list()`

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

Verify the override is the one compiled:

```bash
grep -oE "(components|managed_components)/espressif__esp_codec_dev" build/compile_commands.json | sort | uniq -c
# expect:  NNN components/espressif__esp_codec_dev
```

### 2. `CONFIG_FREERTOS_HZ=1000` (boot crash fix)

The BSP's IMU sample task runs at 250 Hz via `vTaskDelayUntil(&last, pdMS_TO_TICKS(4))` (`bsp_imu.c:89`). At the default **100 Hz** FreeRTOS tick that rounds to **0 ticks**, asserting `xTaskDelayUntil tasks.c:1499 ((xTimeIncrement > 0U))` and causing a boot loop right after IMU init. This must stay `1000`:

```ini
# sdkconfig.defaults
CONFIG_FREERTOS_HZ=1000
```

## sdkconfig essentials

`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n`, `CONFIG_LV_USE_CLIB_MALLOC=y`, `CONFIG_FREERTOS_HZ=1000`, and Montserrat fonts (`14`, `16`, `48`).

## Conventions

- **C code** in `main/`; keep it in `app_main` or small helpers. No C++.
- Use the BSP API; **don't** touch `board_config.h` pins in app code.
- Wrap all LVGL calls in `esp_lv_adapter_lock(-1)` / `esp_lv_adapter_unlock()`.
- Log with `ESP_LOGI/LOGW/LOGE` using a `TAG`.
- Prefer `ESP_ERROR_CHECK` for mandatory init; soft-fail optional peripherals (SD, IMU).
- `printf`-style strings: `LV_LOG` is off by default; use `esp_log`.
- Keep `notes/esp-smartring.md` in sync when you change pin/API assumptions.

## Gotchas

- **Full-screen LVGL flush** (`buffer_height` = full panel) exhausts internal RAM → `ESP_ERR_NO_MEM`/garbage. The BSP uses a 40-line buffer; don't increase it in app code.
- **Audio pop/click:** always mute after playback (`set_mute(true)`); `esp_codec_dev_write()` returns `0` on success (inverted `esp_err_t`).
- **Panel revision:** the BSP uses the vendor "B" init table (`BOARD_LCD_VERSION_A`); a mismatch gives vertical stripes.
- **SD "no card" vs "mount failure"** can't be distinguished in software.
- **Reserved GPIOs:** 0 (BOOT), 19/20 (USB), 43/44 (UART0), 33–37 (PSRAM). Never reconfigure.

## Flashing / reset notes

- Hold **BOOT (GPIO0)** at power-up to enter download mode if the console doesn't auto-enumerate.
- The device uses **native USB** (GPIO19/20), so the port is the USB-Serial/JTAG (`/dev/ttyACM0`).
- If the port is not writable, add the user to `uucp` (`sudo usermod -aG uucp $USER`) or `sudo chmod 666 /dev/ttyACM0` for a one-off.

## Deep-dive docs

- `notes/esp-smartring.md` — full product research, BSP API reference, pin map, and both fix write-ups.
- BSP upstream: `https://github.com/VIEWESMART/smartring_plus` (component `viewesmart/smartring_plus` on the ESP Component Registry).
