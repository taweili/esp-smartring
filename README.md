# VIEWE Smart Ring Plus — ESP-IDF Firmware

ESP-IDF project for the **VIEWE "Smart Ring Plus"** — a round, 1.8-inch
(360×360) capacitive-touch terminal built around the **ESP32-S3-N16R8**
(8 MB octal PSRAM / 16 MB flash). The firmware boots the board through the
official `viewesmart/smartring_plus` Board Support Package and renders with
**LVGL 9** via `esp_lvgl_adapter`.

It is structured as a **generic skeleton plus interchangeable applications**:
`components/ringkit/` wraps the whole BSP, and each app under `examples/<name>/`
is selected at build time with the `RING_APP` cache variable.

## Hardware

| | |
|---|---|
| SoC | ESP32-S3-N16R8 (240 MHz dual-core, 8 MB octal PSRAM, 16 MB flash) |
| Display | 1.8" round IPS, 360×360, ST77916 QSPI + CST816 capacitive touch |
| Audio | ES8311 codec + I2S (48 kHz / 16-bit / stereo), dual mic, speaker (NS4150B PA) |
| IMU | QMI8658A 6-axis (accel / gyro / temperature) |
| Storage | MicroSD (SDMMC 4-bit, FAT, `/sdcard`) |
| Battery | 3.7 V / 600 mAh LiPo; AXP2101 PMIC (V2) or GPIO1 ADC (V1), auto-probed |
| Power | GPIO47 soft power-off |
| Console | USB-Serial/JTAG (native USB, `/dev/ttyACM0`) @ 115200 baud |

## Quick start

Requires ESP-IDF **v6.1** (`IDF_PATH` already exported) with the `esp32s3` target.
The device console is the USB-Serial/JTAG port — **`/dev/ttyACM0`**.

```bash
idf.py set-target esp32s3      # first time / after deleting a build dir
idf.py build                   # default app (glyph-anime)
idf.py -p /dev/ttyACM0 flash monitor
```

Any other app — always its **own build dir**, and pass the same `-B` to build,
flash and monitor (otherwise `flash` picks up a different image):

```bash
idf.py -DRING_APP=voice-note -B build/voice-note build
idf.py -DRING_APP=voice-note -B build/voice-note -p /dev/ttyACM0 flash monitor
```

An invalid `RING_APP` fails fast at configure time and lists the valid choices.

> **Console back-pressure looks like a hang.** With USB-Serial/JTAG as the
> console, `ESP_LOG*` writes block whenever the host stops reading
> `/dev/ttyACM0`. Always drain the port continuously (e.g. `idf.py monitor`) —
> a capture script that sleeps before reading will stall the firmware mid-boot.

## Applications

| App | Description |
|-----|-------------|
| `glyph-anime` | **Default.** 13×13 glyph grid, layered center-out reveal, hue-cycling accents. Touch to open/close. |
| `imu-dash` | Live IMU dashboard: bubble level (tilt) + accel + gyro + temperature + level calibration. |
| `gesture-test` | Reports every single-finger gesture (tap / hold / swipe / press / release) the moment it happens. |
| `voice-note` | Mic + speaker loop-back: hold to record, release to play the capture back. |
| `hello` | Bring-up smoke test: white screen + one large label. |

## Architecture

```
main/                     app-agnostic entry: ring_board_init() -> ring_example_start()
components/ringkit/       generic skeleton wrapping the whole BSP
components/<vendored>/    local overrides (esp_codec_dev, smartring_plus) — see AGENTS.md
examples/<app>/           CMake fragments compiled into main
```

Applications **never** include `bsp/smartring_plus.h` and never call `bsp_*`
directly — everything goes through `ringkit/ringkit.h`. Board bring-up order
lives in `ring_board_init()`: **display first** (it creates the shared I2C bus
and starts the LVGL task), then battery → power → audio (mandatory), then
SD → IMU (fail soft). All LVGL work happens under `RING_UI_LOCKED(-1) { ... }`.

Three things are **required** and must not be reverted (vendored `esp_codec_dev`
and `smartring_plus` overrides for ESP-IDF v6, and `CONFIG_FREERTOS_HZ=1000`) —
see `AGENTS.md` § "Critical fixes already applied".

## Documentation

| Doc | What it is |
|-----|------------|
| `AGENTS.md` | Developer guidance: build/flash commands, verification bar, critical fixes, gotchas. **Start here before changing code.** |
| `examples/README.md` | App contract, how apps are selected and added. |
| `notes/esp-smartring.md` | Hardware + BSP research reference: product overview, full `bsp_*` API, pin map, gotchas. |
| `examples/glyph-anime/README.md` | glyph-anime design & build spec (layered reveal maths, risks). |
| `notes/architecture.md` | Architecture rationale: why the layout is `ringkit` + fragments; the two ESP-IDF traps. |
| `notes/meta-muse.md`, `notes/xiaozhi.md` | Assistant feasibility research (no code yet). |
| BSP upstream | `https://github.com/VIEWESMART/smartring_plus` (component `viewesmart/smartring_plus`). |
