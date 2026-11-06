# Xiaozhi (小智) on the Smart Ring Plus — Feasibility Research

**Date:** 2026-10-30
**Goal:** Can the open-source **Xiaozhi** ESP32 voice-AI assistant run on the **VIEWE Smart Ring Plus** (ESP32-S3-N16R8, 1.8" ST77916 360×360 round)?

---

## 1. What is Xiaozhi?

**Xiaozhi** (小智, "Little Wise") is an open-source ESP32 voice-assistant framework. Upstream repo: **`78/xiaozhi-esp32`** (MIT), official docs: **https://xiaozhi.dev**.

- Runs on **ESP32, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-P4, ESP32-S31**.
- Each supported board lives in its own directory under `main/boards/`.
- Upstream release: **154 board directories / 188 firmware variants**, built with **ESP-IDF 6.1.0**.
- Architecture: the device is a **thin client** — it connects over Wi-Fi (WebSocket) to a Xiaozhi server (self-hostable or public `xiaozhi.me`), streams **16 kHz mic audio up / 24 kHz TTS audio down**, and the server runs an **LLM** behind an **MCP** (Model Context Protocol) tool layer. On-device wake word (ESP-SR), AEC, audio pipeline, LVGL UI, LED, and optional sensors are handled on the chip.
- It supports LLMs/backends via the server: OpenAI-compatible (ChatGPT, DeepSeek, Qwen), plus Xiaozhi's own ecosystem; the UI shows chat bubbles, status bar, emotion/expression.

## 2. Does it run on the Smart Ring Plus? — Short answer

**Yes, it is feasible, and no board port exists yet — you'd create one.** Every hardware block on the Smart Ring Plus is **already used by one or more existing Xiaozhi boards**, so no new drivers are needed — only a new board directory with the ring's pin map.

| Smart Ring Plus hardware | Xiaozhi support? | Boards already using it |
|---|---|---|
| **ST77916 QSPI 360×360** | ✅ Yes | `waveshare/esp32-s3-touch-lcd-1.85c`, `-1.85b`, `-1.85`, `taiji-pi-s3` (VIEWE!), `cyberai/cyber-vocat-*`, `espressif/esp-vocat`, `movecall/moji2-esp32c5` |
| **CST816 / CST816S touch (0x15)** | ✅ Yes | `taiji-pi-s3` (VIEWE), `lilygo/t-circle-s3*`, `spotpear/*`, ~20 more |
| **ES8311 codec + I2S** | ✅ Yes (most common codec in Xiaozhi) | ~80 boards incl. `m5stack/*`, `espressif/*`, `waveshare/esp32-s3-touch-lcd-1.85c` |
| **QMI8658A 6-axis IMU** | ✅ Optional (motion/shake feedback) | `cyberai/cyber-vocat-v2.0` (`main::Motion` namespace) |
| **SD card (SDMMC)** | ✅ (file/asset support) | several boards |
| **Wi-Fi / BLE 5** | ✅ Core requirement | all boards |

> **Critical:** the **Waveshare ESP32-S3-Touch-LCD-1.85C** uses the *exact same silicon stack* — ST77916 QSPI 360×360, CST816 touch, ES8311 codec, TF card. The **Smart Ring Plus is effectively the same board with different pin assignments** (and a battery/PMIC instead of USB-only). So porting is a **pin remap of a proven board**, not a driver bring-up.

## 3. What already ships on the ring vs. open-source Xiaozhi

- The ring **ships with VIEWE's own pre-flashed firmware** that supports **Meta Muse, DeepSeek, ChatGPT, Qwen, Xiaozhi** — i.e. it already does the "voice AI" job with a closed firmware.
- The open-source `xiaozhi-esp32` is a **separate, replaceable firmware** you can flash. It gives you an updatable, self-hostable pipeline (own server/LLM, MCP tools, custom UI).
- VIEWE already contributes one Xiaozhi board: **`taiji-pi-s3`** (TaiJi Viewe Pi, 1.75"/1.8" ST77916 round). There is **no `viewe-smartring-plus` / `smartring` board** in upstream yet (verified by scanning `main/boards/`).

## 4. How to port (the plan)

Porting = creating a new board dir under `main/boards/` and remapping pins. Follow upstream `docs/custom-board.md`.

### 4.1 Create the board directory

```
main/boards/viewe-smartring-plus/
├── config.h              # pin map (ring board_config.h -> Xiaozhi macros)
├── config.json           # { "type": "viewe-smartring-plus", "target": "esp32s3", builds:[...] }
├── viewe-smartring-plus.cc   # board class (WifiBoard subclass) + DECLARE_BOARD(...)
└── README.md
```

### 4.2 `config.h` — pin mapping (Smart Ring Plus → Xiaozhi)

From the ring's `board_config.h`:

| Ring function | Ring GPIO | Xiaozhi macro |
|---|---|---|
| I2C SDA (touch + codec + IMU shared) | **8** | `AUDIO_CODEC_I2C_SDA_PIN`, `TP_PIN_NUM_TP_SDA` |
| I2C SCL | **9** | `AUDIO_CODEC_I2C_SCL_PIN`, `TP_PIN_NUM_TP_SCL` |
| I2S MCLK | **48** | `AUDIO_I2S_GPIO_MCLK` |
| I2S BCLK | **21** | `AUDIO_I2S_GPIO_BCLK` |
| I2S WS | **17** | `AUDIO_I2S_GPIO_WS` |
| I2S DOUT (SPK) | **16** | `AUDIO_I2S_GPIO_DOUT` |
| I2S DIN (MIC) | **18** | `AUDIO_I2S_GPIO_DIN` |
| PA enable | **45** | `AUDIO_CODEC_PA_PIN` |
| ES8311 addr | 0x18 | `AUDIO_CODEC_ES8311_ADDR` |
| Touch RST / INT | **40 / 41** | `TP_PIN_NUM_TP_RST`, `TP_PIN_NUM_TP_INT` |
| LCD QSPI SCLK | **10** | `QSPI_PIN_NUM_LCD_PCLK` |
| LCD QSPI CS | **11** | `QSPI_PIN_NUM_LCD_CS` |
| LCD QSPI D0–D3 | **12/13/15/14** | `QSPI_PIN_NUM_LCD_DATA0..3` |
| LCD RST | **39** | `QSPI_PIN_NUM_LCD_RST` |
| Backlight | **46** | `QSPI_PIN_NUM_LCD_BL` / `DISPLAY_BACKLIGHT_PIN` |
| BOOT (download) | **0** | `BOOT_BUTTON_GPIO` |
| SD CLK/CMD/D0-3 | 4/5/3/2/7/6 | (optional `CONFIG_USE_SDMMC`) |
| IMU INT1 | 42 | optional motion pin |

Display geometry: `DISPLAY_WIDTH=360`, `DISPLAY_HEIGHT=360`, `QSPI_LCD_*_RES=360`, `QSPI_LCD_BIT_PER_PIXEL=16`, `QSPI_LCD_HOST=SPI2_HOST`. Round-screen UI (from `taiji-pi-s3`): `DISPLAY_STATUS_BAR_TOP_OFFSET=15`, `DISPLAY_CHAT_BAR_WIDTH=250`, `DISPLAY_CHAT_BAR_BOTTOM_OFFSET=40`.

### 4.3 Audio codec choice

Two proven classes both work with ES8311:

- **`Es8311AudioCodec`** (`main/audio/codecs/es8311_audio_codec.h`) — simplest, one codec, one I2S bus:
  ```cpp
  new Es8311AudioCodec(i2c_bus_, I2C_NUM_0,
      AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
      mclk, bclk, ws, dout, din, pa_pin, AUDIO_CODEC_ES8311_ADDR, use_mclk, pa_inverted)
  ```
- **`BoxAudioCodec`** (`main/audio/codecs/box_audio_codec.h`) — ES8311 out + optional ES7210/dual-mic in + reference gain; use if you want the advertised **dual-MIC** path (verify the second mic exists on your unit). `waveshare/esp32-s3-touch-lcd-1.85c` uses this.

Sample rates: mic **16 kHz** up (default wake-word/AEC path) or 24 kHz; speaker **24 kHz** out. `AUDIO_DEFAULT_OUTPUT_VOLUME≈80`.

### 4.4 Display + touch glue

- Derive a `SpiLcdDisplay` subclass and override `SetupUI()` for the **round screen** (copy the `taiji-pi-s3` `TaijiPiS3Display` pattern: status bar at `LV_ALIGN_TOP_MID` offset 15, bottom chat bar width 250 at `-40`).
- Init ST77916 with the **vendor init table** (the 1.85C/taiji tables are proven for this panel; the ring's BSP uses its own `lcd_init_seq.h` "B" table — either works, keep the ring's B table for color fidelity).
- Touch: use the `Cst816s` I2C device class at **0x15** (copy from `taiji-pi-s3` / `lilygo-t-circle-s3`) and register it as the LVGL pointer.
- One shared I2C bus (SDA=8/SCL=9, 400 kHz) drives touch + codec (+ IMU).

### 4.5 `config.json`

```json
{
  "manufacturer": "viewesmart",
  "type": "viewe-smartring-plus",
  "target": "esp32s3",
  "builds": [
    { "name": "viewe-smartring-plus", "sdkconfig_append": [
        "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y"
    ] }
  ]
}
```

`type`/`name` must be lowercase `[a-z0-9.-]` (no underscores). Don't set wake word/language here — those are build-time user options.

### 4.6 Build & flash

```bash
cd xiaozhi-esp32
. $IDF_PATH/export.sh                # or your idf setup
python scripts/build.py viewe-smartring-plus    # or: idf.py set-target esp32s3 && idf.py build
python scripts/build.py --list-boards
# flash to the ring's native USB port
python -m esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 build/bootloader/bootloader.bin \
   0x8000 build/partition_table/partition-table.bin 0x10000 build/viewe-smartring-plus.bin
```

The ring flashes via **native USB** (`/dev/ttyACM0`, Espressif USB-JTAG).

## 5. What to watch out for

- **Flash/partition:** ring is **16 MB flash + 8 MB octal PSRAM** → use `partitions/v2/16m.csv` (project default on ESP32-S3) and `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`. 16 MB is plenty.
- **Octal PSRAM:** `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y` (needed for LVGL buffers + audio).
- **FreeRTOS tick:** the ring's own BSP needed `CONFIG_FREERTOS_HZ=1000`; set it too (audio/motion tasks).
- **AEC:** enable `CONFIG_USE_DEVICE_AEC=y` if you use the mic; ES8311 supports it. With a single mic, device-side AEC is valuable for the "hands-free" chatbot.
- **Wake word:** ESP-SR wake word models need RAM/PSRAM; 8 MB PSRAM is fine. `config.json` must not select a model — set via build args/menuconfig.
- **Battery/PMIC (AXP2101) is NOT modeled by Xiaozhi.** You'd add a small battery task (read AXP2101/ADC) and optionally wire it to the UI or a low-battery shutdown. Not required for basic operation.
- **The IMU (QMI8658A)** is optional in Xiaozhi — only `cyber-vocat` uses it (shake-to-awake feedback). You can ignore it or port the `Motion` helper.
- **Audio gotchas:** ES8311 `esp_codec_dev_write()` returns 0 on success; mute after playback to avoid pops. ES8311 default address `0x18` matches the ring.
- **Don't overwrite an existing board** (OTA channel collision) — always a new `type`.
- **Round display**: set the round `SetupUI()` offsets, otherwise the chat/status bars can run off the circular corners.

## 6. Verdict

- **Hardware:** fully compatible — all chips already supported by Xiaozhi.
- **Effort:** low-to-moderate. It's a pin remap + board class based on the **Waveshare 1.85C** (same silicon) and **VIEWE `taiji-pi-s3`** (same vendor) templates; ~1 day to first boot, more for battery/IMU polish.
- **Alternative:** keep the ring's stock VIEWE firmware (already runs Xiaozhi-family/Meta Muse/DeepSeek/ChatGPT/Qwen). Use open-source Xiaozhi if you want a self-hosted server, custom LLM, MCP tools, or a fully custom UI.

---

## 7. References

- Upstream repo: https://github.com/78/xiaozhi-esp32
- Custom board guide: https://github.com/78/xiaozhi-esp32/blob/main/docs/custom-board.md (and `_zh.md`)
- Dev docs: https://xiaozhi.dev/en/docs/esp32/ · Supported boards: https://xiaozhi.dev/en/docs/usage/supported-boards/
- VIEWE board in Xiaozhi: `main/boards/taiji-pi-s3/` (ST77916 + CST816S 0x15 + round 360×360)
- Same-silicon reference: `main/boards/waveshare/esp32-s3-touch-lcd-1.85c/` (ST77916 + CST816 + ES8311)
- IMU reference: `main/boards/cyberai/cyber-vocat-v2.0/` (QMI8658A `Motion` helper)
- Audio codecs: `main/audio/codecs/es8311_audio_codec.h`, `box_audio_codec.h`
- Smart Ring Plus BSP (for pins): `VIEWESMART/smartring_plus` (`include/bsp/board_config.h`)

*Source basis: direct inspection of the `78/xiaozhi-esp32` board tree (`main/boards/`, `docs/custom-board.md`, `main/audio/codecs/*`), the `VIEWESMART/smartring_plus` BSP pin map, plus the product pages and xiaozhi.dev docs.*
