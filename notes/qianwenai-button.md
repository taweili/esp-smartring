# qianwenai-button — one green button, hold-to-talk over WebRTC

**Status:** design note (no code yet). Companion to `notes/qianwenai.md` (platform
& model reference) and `notes/networking.md` (HTTPS/WebRTC on the ring).

**App:** `examples/qianwenai-button/` (proposed). One round **green** button on
the 360×360 screen. **Press and hold** to talk; **release** to let the model
answer. Voice path is **`qwen-audio-3.1-realtime-flash` over WebRTC** (Qianwen AI
platform, `qianwenai.com`).

---

## 1. What the app does

| State | Screen | What happens |
|---|---|---|
| **Idle** | green ring button, "HOLD TO TALK" | WebRTC session is **already connected** and the model is ready; mic muted |
| **Talking** (button held) | button turns red, "LISTENING…", audio level bar | Mic open, 16 kHz mono PCM streamed uplink over WebRTC |
| **Thinking / replying** | button orange, "…", then "REPLYING…", level bar | Uplink muted, model's 24 kHz PCM reply streamed to the speaker |
| **Error / no network** | status line red | Falls back to idle with a message; auto-reconnect in the background |

The whole interaction is one press: there is no separate "connect" step the user
sees. The app boots, brings up Wi-Fi, connects WebRTC, and shows the idle button.

> **Why hold-to-talk instead of always-on VAD.** The ring has a speaker next to
> the microphone. The model supports `server_vad`, but while the speaker is
> playing the model hears itself and can start a reply loop. Hold-to-talk gives
> a deterministic half-duplex window: **mic open only while the button is down,
> speaker only while it is up.** WebRTC's built-in AEC/noise suppression still
> runs on the uplink. `server_vad` remains an option for a later "hands-free"
> mode, with `response.cancel` as the off switch.

---

## 2. Why WebRTC (and what it costs)

The target model supports three transports (AOQ, WebRTC, WebSocket — see
`notes/qianwenai.md` §3). WebRTC is chosen because:

- **Built-in AEC + noise suppression** on both directions (WebSocket has none;
  the ring's speaker/mic coupling makes that painful).
- **Better weak-network behaviour** than WebSocket, and it is the transport the
  platform documents for non-browser clients ("any platform with a standard
  WebRTC library").
- Media travels on **RTP tracks** (no base64-in-JSON overhead) and model events
  travel on a **DataChannel** — a good fit for an embedded client.

The trade: WebRTC needs a WebRTC stack. The candidate is Espressif's
**`esp_peer`** (see `notes/networking.md` §4):

| Requirement | Detail |
|---|---|
| Components | `espressif/esp_peer` (registry, v1.5.8+) — pulls `espressif/esp_libsrtp` |
| Kconfig | `CONFIG_MBEDTLS_SSL_PROTO_DTLS=y`, `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y`, `CONFIG_MBEDTLS_X509_CREATE_C=y` |
| Memory | TLS buffers in PSRAM: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` (DTLS disables `CONFIG_MBEDTLS_DYNAMIC_BUFFER`) |
| Version | **`esp_peer` ≥ v1.4 on IDF v6.1** (older prebuilts crash — `notes/networking.md` §4.5) |
| Size | App partition is 1500K; WebRTC + DTLS + audio codec + UI will grow the image — measure after integrating |
| Signalling | **Not provided by `esp_peer`.** The app implements the Qianwen SDP exchange itself (§4) |

**Non-negotiable compatibility risk (call out before coding).** Qianwen's
documented WebRTC clients are **browsers** and the **AOQ SDK** (Android/iOS/
HarmonyOS). The platform does not publish its exact SDP offer requirements
(codec order, `m=audio` attributes, DataChannel labels beyond `oai-events` /
`txt`) or an `esp_peer` interop example. The first milestone is therefore a
**bring-up probe** (§10), not the full UI: connect `esp_peer`, exchange SDP,
listen for `session.created`, and only then build the button. Budget for
SDP/codec iteration against `qianwenai.com`.

---

## 3. System architecture

```
                         ┌──────────────────────────────┐
   touch ──► LVGL task ──►│  qianwenai-button (app)      │
                         │                              │
                         │  ui_*()  ── RING_UI_LOCKED   │
                         │  button state machine        │
                         └───────┬──────────────┬───────┘
                                 │              │
                    PRESSED/     │              │  audio frames
                    RELEASED     │              ▼
                                 │      ┌───────────────┐
                                 │      │ ring_audio_*  │  ES8311 48k/16-bit/stereo
                                 │      │  (ringkit)    │
                                 │      └───────────────┘
                                 ▼
                         ┌──────────────────────────────┐
                         │ ring_net (new ringkit module)│
                         │  - Wi-Fi / NVS credentials   │
                         │  - HTTPS SDP exchange        │
                         │  - esp_peer wrapper          │
                         └──────────────┬───────────────┘
                                        │ WebRTC (ICE/DTLS/SRTP)
                                        ▼
                              maas.qianwenaiapi.com
                         /api/v1/webrtc/realtime?model=qwen-audio-3.1-realtime-flash
```

**Rule:** the app talks to hardware and network **only** through `ringkit`
(`ringkit/ringkit.h`). A new `ring_net` module (or a `ring_webrtc` wrapper)
belongs in `components/ringkit/`, not in the example — the example holds only
the UI and the button state machine, exactly like `voice-note`.

---

## 4. Connection sequence (Qianwen WebRTC)

From `qianwenai.com` / `platform.qianwenai.com/docs/api-reference/realtime-api/webrtc-access`:

1. **Wi-Fi first.** `ring_wifi_start()` + `ring_wifi_wait_connected()` (NVS
   credentials). No TLS/WebRTC without an IP and a working clock.
2. **Build an Offer SDP.** `esp_peer` gathers local ICE candidates; the offer
   must contain an **`m=audio`** section. Audio directions: `sendrecv`
   (Qianwen-Audio-Realtime is send-audio + receive-audio; no video).
3. **Exchange SDP over HTTPS.**
   ```
   POST https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=qwen-audio-3.1-realtime-flash
   Content-Type: application/sdp
   Authorization: Bearer <QIANWEN_API_KEY>
   <body = Offer SDP>
   → 200, body = Answer SDP (plain text, no JSON wrapper)
   ```
   Use `esp_http_client` with `crt_bundle_attach` (see `notes/networking.md` §3).
   **Keep the API key in NVS**, never in source. The Qianwen docs assume an
   AppServer proxies SDP so the key stays server-side; on the ring the key lives
   in NVS on-device — acceptable for a prototype, note it as a security caveat.
4. **Set the remote description**, wait for `ESP_PEER_STATE_CONNECTED` and the
   DataChannel to open (label `oai-events` from the client, `txt` from the
   server — accept both).
5. **Configure the session** over the DataChannel (JSON), then wait for
   `session.updated`:
   ```json
   { "type": "session.update",
     "session": {
       "modalities": ["text", "audio"],
       "voice": "longanqian_v3.1",
       "input_audio_format": "pcm",
       "output_audio_format": "pcm",
       "turn_detection": { "type": "server_vad", "threshold": 0.5,
                           "silence_duration_ms": 800 }
     } }
   ```
   (`turn_detection` can be omitted for manual hold-to-talk.)
6. **Ready → show the green idle button.**

**Audio formats (from `qianwenai.com` model page):**

| Direction | Format | Spec |
|---|---|---|
| Input (ring → cloud) | PCM | **16 kHz, 16-bit, mono** |
| Output (cloud → ring) | PCM | **24 kHz, 16-bit, mono** |

---

## 5. Audio pipeline (the ring's codec vs the model)

`ring_audio_*` is fixed at **48 kHz / 16-bit / stereo** (~192 KB/s). The model
wants **16 kHz / 16-bit / mono** in and **24 kHz / 16-bit / mono** out. So the
app needs a small conversion stage — this is the heart of the example.

```
  ES8311 mic ──48k/16/stereo──► [decimate ×3, L+R mix, mono] ──16k/16/mono──► WebRTC RTP
  ES8311 spk ◄─48k/16/stereo── [interpolate ×2, duplicate to stereo] ◄──24k/16/mono── WebRTC RTP
```

**Uplink (16 kHz mono PCM).** Read `REC_CHUNK` bytes from `ring_audio_read()`
(48 kHz stereo), then:
- take one frame every 3 (48000 → 16000) — a plain decimator is enough for
  speech;
- mix L+R to mono (average the two 16-bit samples);
- push the resulting 16-bit PCM into the WebRTC audio track as it is produced
  (≈20–40 ms frames). No `input_audio_buffer.append` over WebRTC — audio is a
  media track.

**Downlink (24 kHz mono PCM).** Pull decoded PCM from `esp_peer`'s
`on_audio_data` callback into a **PSRAM ring buffer**, then:
- interpolate 24000 → 48000 (linear is fine; the codec is 16-bit and the speaker
  is small);
- duplicate mono → stereo;
- write to `ring_audio_write()` in chunks, with the required
  `ring_audio_set_mute(false) → write → ring_audio_set_mute(true)` sequence to
  avoid pops.

**Buffer budget.** Use PSRAM (`CONFIG_SPIRAM_USE_MALLOC` /
`MALLOC_CAP_SPIRAM`): a ~2 s downlink ring buffer at 24 kHz mono 16-bit is
~96 KB; a small uplink scratch is internal RAM. Do **not** allocate the capture
buffer in internal RAM (the LVGL 40-line panel already needs it — see
`notes/esp-smartring.md` §6).

**Optional improvement:** encode uplink as **Opus** (`esp_peer` supports it) to
cut bandwidth; for the first version, **PCM/G.711 is simpler and matches the
model's documented PCM input**. Decide after the bring-up probe.

---

## 6. UI design (360×360 round panel)

Layout follows `ring_theme.h` (`RING_CX`/`RING_CY` = 180) and the `voice-note`
example's geometry. One green button, everything else is text.

```
        ┌─────────────────────────────┐
        │                             │
        │        QIANWEN              │  ← title (montserrat_16, dim)
        │                             │
        │      ┌───────────────┐      │
        │      │               │      │
        │      │   green ring  │      │  ← the ONE button (r=78 px)
        │      │  HOLD TO TALK │      │     border = RING_COLOR_GREEN
        │      │               │      │
        │      └───────────────┘      │
        │                             │
        │      press & hold to talk   │  ← status line (montserrat_16)
        │      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁      │  ← audio level bar (green→red)
        │      hold = talk · release  │  ← hint (montserrat_14, dim)
        └─────────────────────────────┘
```

**Button states** (all under `RING_UI_LOCKED`):

| State | Border / fill | Label | Status |
|---|---|---|---|
| Idle | green border, dark fill | `HOLD\nTO\nTALK` | "press & hold to talk" |
| Connecting | dim/grey, pulsing | `···` | "connecting…" |
| Talking | **red** border | `LISTEN` | "listening…" + level bar |
| Thinking | amber | `···` | "…" |
| Replying | orange | `REPLY` | "replying…" + level bar |
| Error | red | `!` | error text (e.g. "no network") |

Use the existing `RING_COLOR_GREEN` (`0x00E676`) accent, `lv_font_montserrat_20`
for the button label, and the same `lv_bar` pattern as `voice-note` for the
level meter. The button is a `lv_obj` with `LV_OBJ_FLAG_CLICKABLE`; the
`PRESSED`/`RELEASED`/`PRESS_LOST` events drive the state machine (§7).

**Level meter.** Compute a crude RMS from the uplink chunk (or the downlink
buffer fill) and map it to the bar — 250 ms refresh is enough. This gives the
user feedback that the mic is live.

---

## 7. Button state machine & task structure

Follow the **`voice-note` pattern exactly**: LVGL events run on the LVGL task;
audio/WebRTC I/O is blocking and runs on a **worker task**. Never do I/O in an
event callback.

```
LVGL task                          worker task ("qw-button")
────────────                       ─────────────────────────
PRESSED  ──set s_talking──────────►  open mic, start uplink
RELEASED ──set s_stop─────────────►  close uplink, wait for reply
                                     drain downlink → speaker
                                     ──set s_idle────────────► LVGL task
```

- **`btn_cb`** (LVGL task): on `LV_EVENT_PRESSED` set `s_talking=true` and
  spawn the worker; on `RELEASED`/`PRESS_LOST` set `s_stop=true`. Never block.
- **`qw_worker`** (worker task): owns the talk turn. Opens the mic, streams
  uplink until `s_stop`, stops, then plays the model reply, then returns the UI
  to idle. Takes the LVGL lock only for brief UI updates, never around I/O.
- **Reentrancy:** a `s_worker_busy` flag ignores PRESSED while a turn is in
  flight (same guard as `voice-note`).
- **Background tasks:** Wi-Fi (`ring_wifi`), WebRTC (`esp_peer_main_loop` in
  its own task), and a UI `lv_timer` at 5–10 Hz to refresh the level bar and
  connection state.

**Why the worker matters.** `ring_audio_read()`/`write()` and the WebRTC send
path block. If they ran on the LVGL task, the `RELEASED` event that stops the
turn could never be processed — the exact bug `voice-note` documents.

---

## 8. State & error handling

| Condition | Behaviour |
|---|---|
| Wi-Fi not connected | Show "connecting…" / "no network"; retry `ring_wifi_wait_connected()` in the UI timer. Do not attempt SDP until IP is up. |
| WebRTC connect fails | Log the `esp_peer` error, drop to idle with "connection failed — tap to retry". Reconnect in the background; do not spin. |
| Model replies after release | Drain the downlink buffer to the speaker, then idle. If the user presses again, `response.cancel` over the DataChannel cancels the reply. |
| Very short press (<150 ms) | Ignore (treat as a tap) to avoid sending a click/pop. |
| Button held too long | No hard limit; the model's VAD/`silence_duration_ms` ends the turn. Optional 60 s safety cap. |
| Speaker pop | Always `ring_audio_set_mute(false)` before the first write and `true` after the last. |
| Console back-pressure | Log sparingly during a turn (see `AGENTS.md`); a flood of `ESP_LOGI` stalls the firmware. |

---

## 9. Config & dependencies

**`main/idf_component.yml`** (single manifest):
```yaml
dependencies:
  idf: ">=5.5"
  viewesmart/smartring_plus: "^1.1.2"
  espressif/esp_peer: "^1.5.8"     # pulls espressif/esp_libsrtp
```

**`sdkconfig.defaults`** (shared by all apps):
```ini
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
CONFIG_MBEDTLS_X509_CREATE_C=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y      # TLS buffers in PSRAM
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

**`components/ringkit/`**: add `ring_net` (or `ring_webrtc`) with
`ring_net_webrtc_start(model, key)`, `ring_net_webrtc_send_audio()`,
`ring_net_webrtc_recv_audio()`, `ring_net_webrtc_stop()`, and callbacks for
state/events. `REQUIRES esp_peer esp_http_client esp-tls esp_wifi esp_netif
mbedtls nvs_flash`.

**Example fragment** (`examples/qianwenai-button/`):
```cmake
set(RING_EXAMPLE_SRCS         "qianwenai_button.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "heap")
```
(`heap` for the PSRAM buffers, like `voice-note`.)

**API key storage.** Add the key to NVS (namespace e.g. `qwen`, key `api_key`)
via `nvs_set_str`, or pass it at build time with
`-DQIANWEN_API_KEY="sk-..."`. Never hardcode a real key in the source; the
existing `ring_wifi.h` default credentials are a bad example to copy.

---

## 10. Build order & verification

1. **Bring-up probe (highest risk first).** A minimal `examples/qianwenai-button`
   that only connects: Wi-Fi → SDP exchange → `session.created`/`session.updated`
   → log `ESP_PEER_STATE_CONNECTED`. No audio, no button. This de-risks the
   `esp_peer` ↔ Qianwen interop before any UI work.
2. **Audio loopback over WebRTC.** Send a fixed PCM tone, receive the reply,
   verify the 16 k/24 k conversion and the level meter.
3. **Button UI.** Add the green button, hold-to-talk, state machine.
4. **Both apps build** from clean dirs (`build/glyph-anime`, `build/qianwenai-button`)
   — they share `ringkit`.
5. **Vendored overrides still win** (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`) — see `AGENTS.md`.
6. **Flash and read the boot log** — expect the `ringkit:` summary, then
   `app 'qianwenai-button' is on screen`, then WebRTC/session logs; no panic.
7. **On-device voice check needs a human.** Mic/speaker echo, latency and the
   hold-to-talk feel cannot be verified from the log alone.

---

## 11. Risks & open questions

- **`esp_peer` ↔ Qianwen WebRTC interop is unproven.** No documented embedded
  example. The SDP answer may require specific codecs/attributes; expect
  iteration. Mitigation: the bring-up probe (§10) and, if it fails, fall back to
  the **WebSocket** realtime path (`notes/qianwenai.md` §3) — simpler, but no
  built-in AEC.
- **API key on-device.** Qianwen's model assumes an AppServer holds the key. On
  the ring the key is in NVS; acceptable for a prototype, not for production.
- **Audio conversion quality.** A naive decimator/interpolator is fine for
  speech but will hiss on music; if quality matters, use an Opus codec and the
  model's Opus path (check `input_audio_format` support at bring-up).
- **Full-duplex echo.** Even with WebRTC AEC, holding the ring near the speaker
  at high volume can still couple. Test at `RING_AUDIO_OUT_VOL` 75 first.
- **Image size.** App partition is 1500K; measure after adding `esp_peer` +
  DTLS + audio. If it overflows, drop to the WebSocket transport or trim
  `esp_peer_default_cfg_t` buffers.
- **Rate limits / cost.** Realtime sessions are billed by the platform; add a
  session cap and log usage. The `maas.qianwenaiapi.com` gateway is the China
  region — check latency from the deployment location.

---

## 12. References

- Qianwen platform: `https://www.qianwenai.com` (model market, model pages)
- Qianwen docs index: `https://platform.qianwenai.com/docs/llms.txt`
- WebRTC access: `platform.qianwenai.com/docs/api-reference/realtime-api/webrtc-access`
- Realtime API overview / model support: `.../realtime-api/overview`
- Qwen-Audio-Realtime client events: `.../api-reference/qwen-audio-realtime/client-events`
- Token/auth: `.../api-reference/realtime-api/token-auth`
- Model page (`qwen-audio-3.1-realtime-flash`, audio formats): `www.qianwenai.com/models/qwen-audio-3.1-realtime-flash`
- In-repo: `notes/qianwenai.md`, `notes/networking.md`, `examples/voice-note/`,
  `examples/wifi-demo/`, `AGENTS.md`
