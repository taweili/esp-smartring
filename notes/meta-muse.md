# Meta Muse on the Smart Ring Plus — Feasibility Research

**Date:** 2026-10-30
**Goal:** What is **Meta Muse**, and how can it be used with the **VIEWE Smart Ring Plus** (ESP32-S3-N16R8, 1.8" ST77916 360×360 round)?

---

## 1. What is Meta Muse?

**Muse** is **Meta's personal AI agent**, launched **8 September 2026**. It's not just a chatbot — it *acts*: it can send email, book tables, fill forms, make purchases, and talk to you, running on a Meta cloud computer on your behalf. It connects to Facebook/Instagram and third-party apps (Spotify, OpenTable), and is reachable via an app, WhatsApp, and (rolling out) voice calling + shareable avatars.

Underlying Meta stack:
- **Muse Spark 1.1** — Meta Superintelligence Labs' multimodal reasoning model (agentic, tool/computer use, coding).
- **Muse Voice Transcribe** — streaming STT on **Meta Model API** (~$0.18/hour, `PCM_16KHZ`/`PCM_24KHZ` mono, realtime WebSocket handshake).
- **Muse Realtime Voice / Realtime Avatar** — real-time streaming conversation + expressive avatars (embodiment for live voice).

## 2. Meta Muse Gadgets — the open-source hardware SDK

**Muse Gadgets** (`gadgets.muse.ai`, repo **`facebookincubator/muse-gadget-sdk`**, **Apache-2.0**) lets you turn an ESP32 board (or Raspberry Pi) into a body for the Muse agent. Two SDKs:

- **ESP32 Device SDK** (`esp32/`) — flash an ESP32 board so it connects to Muse over home Wi-Fi: screen (images, status, avatar), mic in, speaker out, buttons, sensors, and a **home-network tunnel** so Muse can reach your local devices/HTTP APIs.
- **Linux Device SDK** (`linux/`) — Raspberry Pi / Linux box; sysadmin & Home Assistant commands.

**Flow:** get an **SDK token** (`gadgets.muse.ai/settings/sdk-tokens`) → flash the board → **pair with the Muse app** (Settings > Devices, enable Developer mode, devices appear as "MuseGadget…"). Built with **ESP-IDF 6.0.1** and **LVGL 9.5**. Each board is one `.c` file in `esp32/components/muse/boards/` + one `sdkconfig.muse-<board>` in `esp32/devices/`, selected by `CONFIG_MUSE_BOARD_<NAME>=y`.

### Already-supported boards (the important ones)

| Muse SDK board | Hardware | Relation to Smart Ring Plus |
|---|---|---|
| **`board_waveshare_s3_185c.c`** (sdkconfig.muse-waveshare-s3-185c) | ST77916 QSPI **360×360 round**, CST816, ES8311 DAC + ES7210 mic, TCA9554 expander, ADC battery, 16 MB/8 MB PSRAM | **Same silicon stack as the ring** (ST77916 + CST816 + ES8311 + round 360px). Near-identical port template. |
| **`board_waveshare_s3_175c.c`** (featured on gadgets.muse.ai) | round 1.75" AMOLED 466×466, PTT, speaker+mic, **battery** | Same *form factor* as the ring (round, battery). |
| **`board_ostb_echoear_2st.c`** | VIEWE-family **EchoEar** round 360px, ES8311(0x18)+ES7210(0x40), touch 0x15 | **VIEWE (OSTB) device already ported** — closest vendor precedent. |
| `board_sensecap_watcher.c` | ESP32-S3 round display, ES8311-only audio, knob | Used by the esp32-thinks example (below); ES8311-only mic+spk. |
| `board_aipi.c`, `board_m5stack_sticks3.c`, `board_m5stack_stopwatch.c`, `board_esp_mosaico.c`, … | various | Confirms ES8311-only audio + round screens are well-trodden. |

> `MUSE_AUDIO_RATE = 16000` (mono 16 kHz) — the SDK's voice pipeline matches the Muse Model API's `PCM_16KHZ` realtime handshake.

## 3. Can it be used with the Smart Ring Plus? — **Yes**

The ring is hardware-wise an ideal Muse gadget. **Every needed block is already proven in the SDK** — you only add a new board file with the ring's pin map (same effort as the Xiaozhi port, but within Meta's SDK).

| Ring feature | Muse SDK support |
|---|---|
| ESP32-S3 (16 MB flash / 8 MB octal PSRAM) | ✅ Core; `CONFIG_SPIRAM_MODE_OCT`, `FLASHSIZE_16MB` (as in the 185c config) |
| ST77916 QSPI **360×360 round** | ✅ `board_waveshare_s3_185c.c` + `muse_lcd_bands.c` (banded round-screen drawing) |
| CST816 touch (0x15) | ✅ `esp_lcd_touch_cst816s` + direct GPIO or expander reset |
| **ES8311** speaker+mic (0x18) | ✅ ES8311-only boards (SenseCAP Watcher, AiPi, M5Stack); `esp_codec_dev` drives spk+mic from one codec |
| QMI8658A IMU | ⚠️ Not used by Muse firmware (optional; can add gesture/tap-to-talk) |
| SD card | ✅ (file/asset support in several boards) |
| Wi-Fi / BLE | ✅ Core requirement |
| **Battery (AXP2101 / ADC)** | ⚠️ Not modeled in the SDK — boards use an ADC reading only (e.g. 185c `BATT_ADC`). Add your own AXP2101 task. |
| Button for PTT/pairing | ⚠️ SDK uses a button (hold 5 s to pair, status LED). Ring has **only BOOT (GPIO0)** and no RGB LED — map BOOT as the button and show Muse status on the display instead. |

## 4. Two ways to use it

### Option A — Official Meta path (Muse app + SDK token)

Flash the **stock Muse Gadget firmware** (with a new `viewe-smartring` board) and pair it with the **Muse app** using an **SDK token**. Muse then runs on the ring: press-to-talk (or wake word), stream mic up / TTS down, show Muse's **status + avatar + images** on the round display, and (on round 466 px boards) **video streaming** (H.264/VP8). Requires a Meta account, the Muse app, and an SDK token; the "brain" is Meta's.

### Option B — esp32-thinks / openmuse (bring your own model, no Meta account)

**`raullenchai/esp32-thinks`** is a reviewable patch on the Muse Gadget SDK (built on it, ESP-IDF v6.0.x, **SenseCAP Watcher** board) that makes a **multi-model Muse**:
- **Three independent OpenAI-compatible endpoints** (STT / chat / TTS) — pick any provider per task (OpenAI, QuickSilver, local, custom); presets `openai`, `quicksilver`, `openmuse`, `custom`.
- **Wake word "Hi, ESP"** (esp-sr WakeNet10, on-chip) **or button**; **barge-in**, streaming sentence-by-sentence replies, language following (EN/CN/KO), silence guard, stall recovery.
- **Headless provisioning** — Wi-Fi in config, self-provisions on boot; no app/BLE.
- Optional **openmuse** agent adds location, clock, weather, persistent memory, web search, and proxies STT+chat over one TLS session.

This is a great fit for the ring, which has **no knob** — you'd use the wake word (or map BOOT as the button), and the round display shows the conversation.

## 5. Porting the Smart Ring Plus to the Muse Gadget SDK

Create two things (copy from `board_waveshare_s3_185c.c`, which is the same panel stack):

1. **`esp32/components/muse/boards/board_viewesmart_smartring.c`** — implement the board API (`muse_board.h`: `init`, `display_start`, `audio_init`, `button`, `battery`…) with the ring's pins.

2. **`esp32/devices/sdkconfig.muse-smartring`** — device config:
```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_MUSE_BOARD_VIEWESMART_SMARTRING=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
...  (octal-PSRAM settings, same as sdkconfig.muse-waveshare-s3-185c)
```
Then add the board to `esp32/components/muse/CMakeLists.txt` (the `CONFIG_MUSE_BOARD_*` → `.c` mapping) and build with the SDK's `AGENTS.md` instructions / `idf.py` (ESP-IDF **6.0.1**).

### Pin map: Smart Ring Plus → Muse board file

| Ring function | Ring GPIO | Muse board macro/note |
|---|---|---|
| LCD QSPI SCLK / CS / D0–D3 | **10 / 11 / 12-13-15-14** | `LCD_SCLK/LCD_CS/LCD_D0..D3` |
| LCD RST / TE / BL | **39 / 38 / 46** | `LCD_RST/LCD_TE/LCD_BL` (direct GPIO — **no TCA9554 expander** on the ring) |
| I2C SDA / SCL (touch+codec) | **8 / 9** | `I2C_SDA/I2C_SCL` |
| Touch CST816 (0x15) RST / INT | **40 / 41** | `TOUCH_RST/TOUCH_INT` |
| I2S MCLK / BCLK / WS / DOUT / DIN | **48 / 21 / 17 / 16 / 18** | `I2S_MCLK/I2S_BCLK/I2S_WS/I2S_DOUT/I2S_DIN` |
| PA enable | **45** | `PA_EN` |
| ES8311 addr | **0x18** | single codec for **both** mic+spk (unlike the 185c's ES8311+ES7210) |
| BOOT (button) | **0** | `BOOT_BUTTON_GPIO` / `muse_gpio_button_init` |
| IMU INT1 | **42** | optional gesture |
| Battery | AXP2101 (V2) / ADC1_CH0 (V1) | add own task (SDK only does ADC) |
| Round UI | 360×360 | `muse_lcd_bands.c` (e.g. `DRAW_BUF_LINES=72`) |

## 6. Gotchas

- **SDK token + Muse app** required for the official path (or use esp32-thinks' headless path).
- **ESP-IDF must be 6.0.1** for the Muse SDK firmware (the ring's own project uses 6.1 — keep them separate).
- **No user button / no RGB LED** on the ring → use the **wake word** and/or map **BOOT (GPIO0)** as the push-to-talk/pairing button; show pairing/listening/speaking status on the round display.
- **Battery (AXP2101)** isn't modeled by the SDK — add a low-battery/charging task (the ring's own BSP already has `bsp_battery_*`).
- **Audio:** ES8311 alone (mic+spk, no ES7210) is supported (`muse_audio.c` uses `esp_codec_dev` with separate spk/mic handles). The marketing "dual MIC" likely refers to an ES7210 second mic — verify before wiring dual-mic.
- **Round screen:** use `muse_lcd_bands` banded drawing (full-screen LVGL flush exhausts internal RAM) and the ST77916 vendor init table from `waveshare_s3_185c/st77916_init_185c.h` (or the ring BSP's `lcd_init_seq.h`).
- **Flash/partition:** 16 MB flash + 8 MB octal PSRAM as in `sdkconfig.muse-waveshare-s3-185c`.
- License: SDK is Apache-2.0; the EchoEar display-init header is from `78/xiaozhi-esp32` (MIT).

## 7. Verdict

- **Meta Muse is a first-class fit for the Smart Ring Plus.** The ring is essentially a **Muse gadget**: round touch display, mic, speaker, ESP32-S3, Wi-Fi/BLE, battery. The SDK even ships **the same ST77916/CST816/ES8311 stack** (Waveshare 1.85C) and a **VIEWE-family EchoEar** board.
- **Effort:** low-to-moderate — one board `.c` + one `sdkconfig` + a CMake line, modeled on `board_waveshare_s3_185c.c`.
- **Choice:** Option A (Meta app, agentic Muse, needs Meta account + token) vs **Option B** (esp32-thinks: your own OpenAI-compatible models, wake word, self-hosted — best for a button-less ring).

---

## 8. References

- Muse Gadgets: https://gadgets.muse.ai/ · SDK repo: https://github.com/facebookincubator/muse-gadget-sdk
- ESP32 Device SDK: `esp32/README.md`, `esp32/AGENTS.md`, boards in `esp32/components/muse/boards/`
- Same-silicon template: `boards/board_waveshare_s3_185c.c` + `devices/sdkconfig.muse-waveshare-s3-185c`
- VIEWE-family precedent: `boards/board_ostb_echoear_2st.c`
- Multi-model example: https://github.com/raullenchai/esp32-thinks (patch on the Muse Gadget SDK)
- Meta Muse launch: NYT 2026-09-08 · Muse Spark 1.1: ai.meta.com · Muse Voice Transcribe: dev.meta.ai · Muse Realtime Voice/Avatar: research.meta.ai
- Espressif note (ESP32 powers Muse Gadgets): https://www.espressif.com/en/news/Meta_Muse_ESP32
- Smart Ring Plus BSP (pins): `VIEWESMART/smartring_plus` `include/bsp/board_config.h`

*Source basis: direct inspection of the `facebookincubator/muse-gadget-sdk` ESP32 SDK (boards, devices, `muse_audio.c`, CMake wiring), the `raullenchai/esp32-thinks` repo, the gadgets.muse.ai site, and Meta's Muse launch/model docs.*
