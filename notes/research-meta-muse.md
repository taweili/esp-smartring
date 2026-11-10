# Meta Muse on the Smart Ring Plus — Feasibility Research

**Date:** 2026-10-30 · **Updated:** 2026-11-10
**Goal:** What is **Meta Muse**, and how can it be used with the **VIEWE Smart Ring Plus** (ESP32-S3-N16R8, 1.8" ST77916 360×360 round)?

> **Headline change (2026-11-10):** the Muse Gadget SDK now ships an **official
> SmartRing Plus board** (`board_viewe_smartring_plus.c` +
> `sdkconfig.muse-viewe-smartring-plus`). The ring is no longer a "would-be"
> port — it is a supported device, with a setup doc, a direct-driver board
> file, and a verified-compiles status. See §4–§5.

---

## 1. What is Meta Muse?

**Muse** is **Meta's personal AI agent**, launched **8 September 2026** in the US
(iOS, Android, muse.ai). It doesn't just answer questions — it *acts*: sends
email, books travel, fills forms, negotiates, and checks out purchases (Link by
Stripe, with Shop Pay and 1Password support coming). It runs on **Muse Secure
VM**, a dedicated cloud computer per user, with a separate **Sentinel** agent
that approves every internet action; Meta says Muse never sees passwords or
payment details, asks before sensitive actions, and shows a full audit trail.
A **Confidential VM** (user-held encryption key) is promised later. Muse
remembers context across conversations and can proactively suggest actions.

**Pricing:** free tier with a weekly usage limit (~100M "Muse tokens"/week),
**Power $20/mo** (500M/week) and **Max $100/mo** (3B/week). Availability varies
by region/account.

**New since the last revision:**
- **Voice + expressive avatars** (5 Nov 2026, US; mobile/web/Mac): tap the phone
  icon to talk; the avatar reacts while listening/speaking; interruptible;
  audio-only, avatar-beside-chat, or full-screen; hundreds of voices. Meta
  reports ~870 ms from end-of-speech to first video frame + audio, 25 fps.
  Coming to AI Glasses and "Muse Charm".
- **Spaces** (6 Nov 2026): describe it and Muse builds a custom app-like
  experience (shopping hub, school dashboard, music tracker, trivia game) that
  pulls live data from connected apps (Gmail, Google Calendar, OpenTable,
  Notion, …).

## 2. The model stack

| Model | Role | Notes |
|---|---|---|
| **Muse Spark 1.3** (current) | Powers the Muse app; agentic/coding | Multimodal reasoning; tool + computer use; 1M context. Successor to 1.1 (Jul 2026) and 1.2. Standard **$1.25 / $4.25 per Mtok** (in/out), cached in $0.15; **Contributor mode** $0.10 / $0.20 (data may train Meta models) |
| **Muse Wonder 1.0** (12 Oct 2026) | Higher-capability tier | **$4 / $20 per Mtok** |
| **Muse Voice Transcribe** | Streaming STT | **$0.18/hour**; 25 languages; punctuation, speech-turn detection, speaker labels (20+ speakers), vocabulary biasing. Realtime endpoint `wss://api.meta.ai/v1/asr/realtime` (handshake frame carries the credential; `PCM_16KHZ`/`PCM_24KHZ` signed 16-bit LE mono) plus a file endpoint |
| **Muse Realtime Voice / Realtime Avatar** | In-app voice + embodiment | **No public API yet** — US Muse app only (as of Nov 2026) |

All are on the **Meta Model API** (`https://api.meta.ai/v1`, OpenAI-SDK
compatible, `MODEL_API_KEY`). That matters for Option C below: the ring's
existing `ring_ws` / `ring_chat` transports could target Muse Spark and Muse
Voice Transcribe directly.

## 3. Muse Gadgets — the open-source hardware SDK

**Muse Gadgets** (`gadgets.muse.ai`, repo **`facebookincubator/muse-gadget-sdk`**,
**Apache-2.0**, released **2 Oct 2026**) turns an ESP32 board or a Raspberry Pi
into a body for the Muse agent: screen (images, status, avatar), mic in,
speaker out, buttons, sensors, actuators, and a **home-network tunnel** so Muse
can reach local devices/HTTP APIs.

- **ESP32 Device SDK** (`esp32/`) — built with **ESP-IDF v6.0.3 exactly** (other
  versions unsupported) and **LVGL 9.5**. Each board is one `.c` file in
  `esp32/components/muse/boards/` plus one `sdkconfig.muse-<board>` overlay in
  `esp32/devices/`; `tools/muse/board.sh build <alias>` builds it.
- **Linux Device SDK** (`linux/`) — Raspberry Pi / Linux box; sysadmin & Home
  Assistant commands.
- **Muse Gadgets Test** app (Android + iOS) — streams the phone camera or a
  clip to a gadget's screen over Wi-Fi: **H.264 or VP8 decoded on-board, 30 fps
  on the round 466 px boards** (`protocols/VIDEO.md`).
- **Web flasher** (`facebookincubator.github.io/muse-gadget-sdk`) — flash from
  Chrome/Edge; the SDK token is pushed over USB and kept through updates.
- **Muse Code** — Meta's coding agent (`curl -fsSL https://dev.meta.ai/install.sh
  | sh`, then `muse --disable-sandbox`) reads the SDK's `AGENTS.md` and can
  build/flash/talk to the board.
- **Pairing:** SDK token from gadgets.muse.ai → flash → Muse app **Settings >
  Devices** (enable Developer mode; devices appear as "MuseGadget…").
- **Community:** Discord (`discord.gg/3bhjCkZdd6`); repo at ~2.7k stars /
  ~600 forks, actively maintained (pushes and PRs through 9 Nov 2026).
- **Power profiling:** `esp32/docs/power/` porting recipe + `power_sweep.py`
  (PPK2-based, USB-free battery-input sweeps); board-neutral diagnostic core.
- **Simulator:** `esp32/simulator/` runs the UI on the host (LVGL + SDL).

## 4. Supported boards — the ring is officially one of them

The SDK has grown well past the launch set (~37 boards). The relevant ones:

| Muse SDK board | Hardware | Relation to Smart Ring Plus |
|---|---|---|
| **`board_viewe_smartring_plus.c`** (`sdkconfig.muse-viewe-smartring-plus`, alias `smartring-plus`) | ESP32-S3-N16R8, 360×360 round ST77916 + CST816, ES8311, 16 MB / 8 MB octal | **The ring itself — official support.** Setup doc `esp32/devices/viewe-smartring-plus.md` |
| `board_waveshare_s3_185c.c` | ST77916 QSPI 360×360 round, CST816, ES8311 DAC + ES7210 mic, 16 MB/8 MB | Same panel/touch/codec stack; the original near-identical template |
| `board_ostb_echoear_2st.c` (experimental) | VIEWE-family EchoEar round 360 px, ES8311(0x18) + ES7210(0x40), touch 0x15 | VIEWE (OSTB) device already ported — closest vendor precedent |
| `board_waveshare_s3_175c.c` | Round 1.75" AMOLED 466×466, PTT, speaker+mic, battery | Same form factor; featured on gadgets.muse.ai |
| `board_sensecap_watcher.c` | ESP32-S3 round 412×412, ES8311 + ES7243E, PTT knob | Base board for the esp32-thinks multi-model fork (§6, Option B) |
| `board_ai_passport.c`, `board_m5stack_*`, `board_seeed_xiao_s3_147.c`, `board_espressif_box_3.c`, `board_esp_mosaico.c`, `board_deotaland_rorolee.c`, `board_waveshare_s3_*`, … | various | Confirms ES8311-only audio + round screens are well-trodden |

> `MUSE_AUDIO_RATE = 16000` (mono 16 kHz) — the SDK's voice pipeline matches the
> Muse Model API's `PCM_16KHZ` realtime handshake.

## 5. The official SmartRing Plus port — what's there and what isn't

The board file (`esp32/components/muse/boards/board_viewe_smartring_plus.c`,
424 lines) **drives the hardware directly** — it does not link the BSP. It
follows VIEWE's `viewesmart/smartring_plus` **BSP 1.1.2** for pins and behaviour:

- **Panel:** ST77916 B-panel init table from the BSP's `lcd_init_seq.h`
  (`viewe_smartring_display_init.h`); CST816S touch (reset pulsed on GPIO40).
- **Display:** **two 10-line buffers in internal RAM** (as EchoEar-2ST does with
  the same panel), *not* the BSP's 40-line PSRAM buffers — those need a fresh
  28.8 KB internal DMA buffer per flush, which may not exist once Wi-Fi, BLE
  and the Muse session are up.
- **Audio:** builds a **16 kHz duplex I2S** itself and returns separate speaker
  and mic handles (`mic_slot` 0). It deliberately does **not** use
  `bsp_audio_init()`, which opens the codec and leaves the speaker muted — Muse
  never unmutes a handle it did not open.
- **Battery:** V2 **AXP2101** (ALDO3 rail on, 300 mA charge to 4.2 V, gauge
  percent/voltage/charging/USB) or V1 **GPIO1 ADC** (voltage curve + 4.25 V
  charging threshold), with the same register values as `bsp_battery.c` /
  `bsp_axp2101.c` (`viewe_smartring_power.h`).
- **Talk:** the **on-screen mic icon** is the talk button (`touch_talk` on
  `muse_board_t`) — hold to talk, release to send; a tap confirms pairing.
  **BOOT (GPIO0)** still produces talk events but is inside the case.
- **Deliberately absent:** `power_off` (the GPIO47 soft power-off is unverified
  with this port — use the switch) and `display_pause` (pausing LVGL would stop
  touch, the only way to wake the screen).
- **Reset pairing:** Settings → MUSE → **Reset pairing** (tap again within 5 s).

**Build** (from `esp32/`): `tools/muse/board.sh build smartring-plus`, with the
SDK token set via menuconfig (`CONFIG_GADGET_SDK_TOKEN`). Uses
`partitions_muse.csv` (16 MB flash). ESP-IDF **v6.0.3**.

**Verification status (be precise about this):** the checks were made with an
**earlier BSP-based port** built locally with ESP-IDF v6.0.3 and no warnings:
screen powers on after Wi-Fi, touch orientation is correct, slot 0 carries the
live microphone. The **direct driver** keeps the BSP's pins, panel table, touch
driver and battery settings but **hasn't been rechecked on the board**, and
**Muse app pairing / app connection were not tested** (region/app
availability). So: *compiles and partly verified* — not a confirmed
end-to-end working gadget.

## 6. Three ways to put Muse on the ring

### Option A — Official Meta path (Muse app + SDK token)

Build the **stock Muse Gadget firmware** with the official ring board, flash it,
pair with the **Muse app** using an **SDK token**. Muse then runs on the ring:
hold the on-screen mic to talk, stream mic up / TTS down, show Muse's status +
avatar + images on the round display, and (via Muse Gadgets Test) receive video.
Requires a Meta account, the Muse app (US availability), and an SDK token; the
"brain" is Meta's. **This is now the lowest-effort path** — no porting needed.

### Option B — esp32-thinks / openmuse (bring your own model, no Meta account)

**`raullenchai/esp32-thinks`** is a reviewable patch on the Muse Gadget SDK
(**v0.2.12**, 19 Oct 2026; pinned SDK commit `812c46f`, 9 Oct 2026; **SenseCAP
Watcher** board; ESP-IDF v6.0.x, CI pins v6.0.3; MIT) that makes a
**multi-model Muse**:

- **Three independent OpenAI-compatible endpoints** (STT / chat / TTS) — pick
  any provider per task; presets `openai`, `quicksilver`, `openmuse`, `custom`.
- **Wake word "Hi, ESP"** (esp-sr WakeNet10, on-chip) **or button**; barge-in;
  streaming sentence-by-sentence replies (ASCII + CJK boundaries); silence
  guard; automatic stall recovery (20 s I2S/LCD deadline, wedged-I2C reboot).
- **Language following** (EN/CN/KO tested) and **language-aware voice**
  (qwen3-tts for CN/KO, chatterbox-turbo for EN with the QuickSilver preset).
- **Headless provisioning** — Wi-Fi in config, self-provisions on boot; no
  app/BLE/vendor account.
- Optional **openmuse** agent (in-repo, Docker/Node) adds location, clock,
  weather, persistent memory and web search, and proxies STT so STT + chat
  share one TLS session.
- **Measured on device:** typed warm turns 2.2–2.5 s (EN/KO), ~6.8 s (CN);
  voice turns ~3–5 s from end of speech to first sound; ~3.9/4.7 s with
  openmuse.

For the ring: no knob → use the wake word (or map BOOT). **esp32-thinks does
not yet support the ring** — it's Watcher-only today, though "more boards the
SDK supports are candidates". Porting it is a patch-scale job now that the SDK
has a ring board.

### Option C — Meta Model API in the ring's own firmware (no SDK)

Muse Spark (chat) and Muse Voice Transcribe (STT) are ordinary **Meta Model
API** endpoints (`https://api.meta.ai/v1`, OpenAI-SDK compatible), and Muse
Spark is the model that powers Muse itself. The ring's existing `ring_ws`
(WebSocket) / `ring_chat` (HTTPS cascade) transports in `components/ringkit/`
could target them directly — a "Muse brain, our body" path with no SDK token,
no Muse app, and no US-only dependency. TTS would need a separate model (the
Model API's speech endpoints are STT-only; Muse's realtime voice/avatar has no
public API yet). This is the most flexible option for this repo, but it means
reimplementing the agent loop that the SDK/Muse app otherwise provides.

## 7. Gotchas

- **ESP-IDF must be v6.0.3** for the Muse SDK firmware (the ring's own project
  uses 6.1 — keep them separate).
- **SDK token + Muse app** required for the official path (or use esp32-thinks'
  headless path).
- **Muse app availability is US-only** so far; pairing/connection is untested
  outside it.
- **No user button / no RGB LED** on the ring → the on-screen mic is the talk
  button; BOOT (GPIO0) works but is inside the case; show status on the display.
- **Battery (AXP2101)** is now handled by the official board file (V2 gauge +
  V1 ADC fallback) — the earlier "not modeled" caveat is resolved.
- **Audio:** ES8311 alone (mic+spk, no ES7210) is supported; the marketing
  "dual MIC" likely refers to a second mic — verify before wiring dual-mic.
- **Round screen:** use small internal-RAM buffers / banded drawing
  (`muse_lcd_bands`); full-screen LVGL flush exhausts internal RAM.
- **Flash/partition:** 16 MB flash + 8 MB octal PSRAM, `partitions_muse.csv`.
- License: SDK is Apache-2.0; `viewe_smartring_display_init.h` is Apache-2.0
  under VIEWE's copyright; the EchoEar display-init header is from
  `78/xiaozhi-esp32` (MIT).

## 8. Verdict

- **Meta Muse is a first-class fit for the Smart Ring Plus — and now an
  official one.** The ring is a supported Muse gadget board with a direct
  driver, a setup doc, and a build path.
- **Effort:** Option A is a build-and-pair away (with the verification caveats
  in §5); Option B is a patch-scale port now; Option C is a ringkit-scale
  integration for full independence.
- **Recommendation:** try **Option A** first (it exercises Meta's agent and the
  tunnel end-to-end); if the US-only/Meta-account dependency is a problem,
  **Option B** gives the same hardware experience with your own models, and
  **Option C** is the long-term fit for this repo.

## 9. References

- Muse Gadgets: https://gadgets.muse.ai/ · SDK repo: https://github.com/facebookincubator/muse-gadget-sdk
- **Ring board + setup:** `esp32/components/muse/boards/board_viewe_smartring_plus.c`,
  `esp32/devices/sdkconfig.muse-viewe-smartring-plus`,
  `esp32/devices/viewe-smartring-plus.md`
- Same-silicon template: `boards/board_waveshare_s3_185c.c` + `devices/sdkconfig.muse-waveshare-s3-185c`
- VIEWE-family precedent: `boards/board_ostb_echoear_2st.c` + `devices/ostb-echoear-2st.md`
- ESP32 SDK overview: `esp32/README.md`, `esp32/AGENTS.md`, `esp32/devices/README.md`
- Multi-model fork: https://github.com/raullenchai/esp32-thinks (v0.2.12) · openmuse: `openmuse/README.md`
- Meta Muse launch (8 Sep 2026): https://about.fb.com/news/2026/09/introducing-muse-personal-ai-agent/
- Muse voice + avatars (5 Nov 2026): https://about.fb.com/news/2026/11/introducing-muse-ai-voice-expressive-avatars/
- Muse spaces (6 Nov 2026): https://www.theverge.com/tech/1028871/meta-muse-ai-spaces
- Muse subscriptions: https://www.meta.com/help/subscriptions/1021145227643680/
- Muse Spark 1.3: https://dev.meta.ai/models/muse-spark · docs: https://dev.meta.ai/docs/muse-spark
- Muse Voice Transcribe: https://dev.meta.ai/models/muse-voice-transcribe · realtime API: https://dev.meta.ai/docs/api-reference/voice/realtime
- Meta Model API pricing: https://dev.meta.ai/docs/pricing-rate-limits
- Web flasher: https://facebookincubator.github.io/muse-gadget-sdk/
- Muse Code: https://developer.meta.com/ai/lp/muse-code/
- Espressif note (ESP32 powers Muse Gadgets): https://www.espressif.com/en/news/Meta_Muse_ESP32
- Smart Ring Plus BSP (pins): `VIEWESMART/smartring_plus` `include/bsp/board_config.h`

*Source basis: direct inspection of the `facebookincubator/muse-gadget-sdk` ESP32
SDK (README, devices list, the ring board file and setup doc, sdkconfig), the
`raullenchai/esp32-thinks` repo and releases, Meta's Muse launch/voice posts,
dev.meta.ai model + pricing docs, and gadgets.muse.ai.*
