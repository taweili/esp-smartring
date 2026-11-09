# 06_qianwenai_push_to_talk — one round button, hold-to-talk STT → LLM → TTS

`examples/06_qianwenai_push_to_talk/` is a **push-to-talk voice assistant** for
the Smart Ring Plus. One round button dominates the screen: **green when ready
to talk**, **red while recording or waiting for the model**, **blue while the
reply plays**. Hold it to talk to the Qianwen models; release it and the ring
transcribes the utterance (`qwen3-asr-flash`), asks `qwen3.8-flash` for an
answer, and speaks it (`qwen-audio-3.1-tts-flash`).

The whole turn is three sequential **HTTPS POSTs on TCP/443** to
[maas.qianwenaiapi.com](https://www.qianwenai.com) — no WebRTC, no UDP, no
Opus. That is the point: it works on the bench network where `07`'s DTLS
handshake cannot complete.

```
        ┌─────────────────────────────┐
        │          QIANWEN            │
        │      ┌───────────────┐      │
        │      │               │      │
        │      │  HOLD TO TALK │      │  ← the one button (r = 70 px)
        │      │               │      │
        │      └───────────────┘      │
        │      ready — press & hold   │  ← stage line
        │      you said: …            │  ← transcript
        │      reply: …               │  ← reply
        │      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁      │  ← level bar
        └─────────────────────────────┘
```

| State | Button | Stage line | What happens |
|---|---|---|---|
| Connecting | grey `···` | "connecting…", "connecting to \<SSID\>…", "syncing clock…" | Wi-Fi → SNTP → `ring_chat_init()`; retries every 5 s |
| Idle | **green** `HOLD TO TALK` | "ready — press & hold to talk" | session ready, mic closed |
| Listening | **red** `TALK` | "listening…" + level bar | 48 kHz mic → mono → PSRAM |
| Transcribing | red `TALK` | "heard:" | STT POST; the transcript label fills in |
| Thinking | red `TALK` | "thinking…" | LLM POST; the reply label fills in |
| Speaking | **blue** `REPLY` | "replying…" | TTS SSE → PCM → speaker |
| Error | **red** `!` | "STT failed — tap to retry" (or LLM/TTS) | waits for the next press |

The error state is **sticky**: it stays on screen until the button is pressed
again, so a failed stage is never silently overwritten by the idle screen.

---

## 1. Why this shape

**A cascade, not a realtime session.** `07_qianwen_realtime_harness` runs a
WebRTC session (`ring_net` + `esp_peer` + Opus + DTLS/SRTP/SCTP); `10` runs a
WebSocket session (`ring_ws`). This app does neither — it makes three ordinary
HTTPS calls in sequence:

```
record → POST chat/completions (STT) → POST chat/completions (LLM)
       → POST SpeechSynthesizer (TTS, SSE) → stream PCM to the speaker
```

| | `ring_chat` (this app) | `ring_ws` (10) | `ring_net` (07) |
|---|---|---|---|
| Transport | HTTPS REST (TCP/443) | WebSocket (TCP/443) | WebRTC (UDP/DTLS + RTP) |
| Turn-taking | none — 3 sequential calls | server_vad / smart_turn_v2 / PTT | server_vad only |
| Works on UDP-blocked nets | **yes** | yes | **no** (DTLS times out) |
| Latency | highest (3 round trips) | low | lowest |
| Extra deps | `esp_http_client` | `esp_websocket_client` | `esp_peer`, `esp_audio_codec` |

The price is latency: three HTTPS round trips, each with its own TLS
handshake. Measured on the bench with the test clip: **STT ≈ 2.4 s, LLM ≈ 3.8 s,
TTS ≈ 2.3 s** — about 8 s from release to the end of the reply audio (the first
audio arrives a few seconds earlier). The stage line exists to make that wait
legible (`heard:` → `thinking…` → `replying…`), and the log carries per-stage
timings (`stt: ok in N ms`, `llm: …`, `tts: …`).

Turn-taking is **manual push-to-talk**: the button gates the microphone, and
"the button was released" is the end of the utterance. There is no barge-in and
no acoustic echo cancellation — the ring is half-duplex, and the mic is only
read while the button is held.

---

## 2. Architecture

```
   touch ──► LVGL task ──► 06_qianwenai_push_to_talk (this example)
                             │  PRESSED / RELEASED  → flags + worker task
                             │  ui_*()  ── RING_UI_LOCKED
                             ▼
                        ring_chat  (components/ringkit)
                             │  ring_stt   48k mono → 16k WAV → base64 → HTTPS
                             │  ring_llm   history JSON → HTTPS
                             │  ring_tts   HTTPS SSE → base64 PCM → 48k mono
                             ▼
                        ring_http  (pinned CA + bearer)
                             ▼
              maas.qianwenaiapi.com   (TCP/443)
```

Applications never include `esp_http_client`, `esp_peer` or the codec — the
`ringkit` rule holds: hardware and network live behind `ring_*` APIs.

| File | Role |
|---|---|
| `examples/06_qianwenai_push_to_talk/qianwenai_push_to_talk.c` | UI, button state machine, stereo↔mono, talk turn |
| `components/ringkit/src/ring_chat.c` | `ring_chat_init()`, history, shared audio helpers |
| `components/ringkit/src/ring_stt.c` | STT stage (48k→16k, WAV, base64, POST) |
| `components/ringkit/src/ring_llm.c` | LLM stage (history JSON, POST) |
| `components/ringkit/src/ring_tts.c` | TTS stage (SSE, base64 PCM) |
| `components/ringkit/src/ring_chat_priv.h` | private shared contract (not installed) |
| `components/ringkit/include/ringkit/ring_chat.h` | `ring_chat_*` API |
| `components/ringkit/src/ring_http.c` | HTTPS client: pinned CA, bearer, body + SSE readers |
| `components/ringkit/src/ring_resample.c` | 48k→16k decimator (shared with `ring_ws`) |
| `components/ringkit/assets/qianwen_ca.pem` | pinned trust chain (see §7) |

---

## 3. Connection sequence

1. **Wi-Fi, first and explicitly.** The app calls `ring_wifi_init()`,
   `ring_wifi_start()` and `ring_wifi_wait_connected(20 s)` in its connect task
   *before* the first turn, and shows the link state on screen
   (`connecting to <SSID>…`). A failure retries every 5 s. Credentials come
   from NVS, seeded from `RING_WIFI_DEFAULT_*` on first boot.
2. **Clock — mandatory.** `ring_time_sync(15 s)` (SNTP). The HTTPS calls
   validate the server certificate against the wall clock; without a valid
   clock every connection fails. `ring_time` is the shared SNTP helper — 06
   does not use `ring_net`.
3. **`ring_chat_init()`** — stores the models, voice, system prompt and history
   cap, and allocates the history arrays.
4. **Ready** — the button turns green.

---

## 4. The three stages

All three calls go to `https://maas.qianwenaiapi.com` with
`Authorization: Bearer <key>` and the pinned CA. Each stage opens its own
`esp_http_client` (no connection reuse — see §10).

### 4.1 STT — `ring_chat_stt()`

```
POST https://maas.qianwenaiapi.com/compatible-mode/v1/chat/completions
Content-Type: application/json
Authorization: Bearer <key>

{"model":"qwen3-asr-flash",
 "messages":[{"role":"user","content":[
   {"type":"input_audio",
    "input_audio":{"data":"data:audio/wav;base64,<...>"}}]}],
 "stream":false}
```

- Input is **48 kHz / 16-bit / mono PCM**; `ring_chat` resamples to **16 kHz**
  with the shared 3:1 decimator (`ring_resample`), wraps a 44-byte WAV header
  and base64-encodes it into an OpenAI-compatible `input_audio` Data URI.
- Response: `choices[0].message.content` (a string) — extracted into the
  app's transcript buffer.
- Timeout **15 s**.

### 4.2 LLM — `ring_chat_llm_complete()`

```
POST https://maas.qianwenaiapi.com/compatible-mode/v1/chat/completions
{"model":"qwen3.8-flash",
 "messages":[{"role":"system","content":"…"},
             {"role":"user","content":"<transcript>"},
             {"role":"assistant","content":"<previous reply>"}],
 "stream":false,
 "extra_body":{"enable_thinking":false}}
```

- The app calls `ring_chat_add_user(transcript)` first; `llm_complete()` sends
  the history and appends the reply as an assistant message on success.
- History is capped at **10 turns** (`max_history_turns`); the compile-time
  cap is 20 pairs.
- Response: `choices[0].message.content` (a string).
- Timeout **20 s**.

### 4.3 TTS — `ring_chat_tts()`

```
POST https://maas.qianwenaiapi.com/api/v1/services/audio/tts/SpeechSynthesizer
Content-Type: application/json
Authorization: Bearer <key>
X-DashScope-SSE: enable

{"model":"qwen-audio-3.1-tts-flash",
 "input":{"text":"<reply>","voice":"longanhuan_v3.1",
          "format":"pcm","sample_rate":48000}}
```

- **SSE, not the non-streaming form.** The non-streaming response returns an
  OSS URL on a different host (a second CA); SSE keeps everything on the
  pinned host and starts audio sooner.
- The stream delivers `sentence-begin` → one or more `sentence-synthesis`
  (base64 PCM in `data`) → `sentence-end` per sentence. Each chunk is decoded
  to **48 kHz / 16-bit / mono PCM** and handed to the app's audio callback.
- Requesting `format=pcm, sample_rate=48000` means **no resampling on
  playback** — the ring's native rate.
- Timeout **40 s** (the SSE read loop runs on the same task as the blocking
  speaker writes).

---

## 5. Audio pipeline

```
mic 48k stereo ──ring_audio_read──► ring_audio_stereo_to_mono ──► PSRAM buffer
                                                                     │
                          48k mono ──decimate 3:1──► 16k mono ──WAV──► base64 ──► STT
                                                                     │
TTS SSE ──base64──► 48k mono PCM ──ring_audio_mono_to_stereo──► ring_audio_write
```

- **Capture.** `ring_audio_read()` fills 3840-byte stereo frames (20 ms);
  `ring_audio_stereo_to_mono()` folds them (average L/R) and the app appends
  the mono samples to a **PSRAM** buffer. One utterance is capped at **10 s**
  (960 KB mono) — a safety net, since the button is the real gate.
- **STT** resamples 48 kHz → 16 kHz inside `ring_chat`; the app never sees the
  WAV or the base64.
- **TTS chunks are large.** The server sends one base64 PCM blob per
  `sentence-synthesis` event, up to ~100 KB (1.6 s @ 48 kHz). `ring_chat`
  reads the SSE stream with a **buffered** reader (`ring_http_sse_t`) and
  decodes into a 128 KB base64 + 192 KB PCM buffer in PSRAM. The old
  byte-at-a-time reader took 10 s to drain a 0.7 s reply; the buffered one
  takes ~2.3 s for the whole turn.
- **Playback.** The TTS callback expands each chunk to stereo
  (`ring_audio_mono_to_stereo`) and writes it. Chunks arrive up to 1.6 s
  long, so the callback splits them into 20 ms frames — a single 20 ms buffer
  would truncate everything after the first frame.
- **Mute sequencing.** The codec is unmuted **once** before `ring_chat_tts()`
  and muted again after it returns. Toggling mute around every chunk is an
  I2C write per chunk and cuts the I2S DMA queue mid-buffer (clicks, dropped
  syllables) — the same lesson `10` documents.

---

## 6. Task & memory layout

| Task | Stack | RAM | Role |
|---|---|---|---|
| `qw-connect` | 8 KB | internal | Wi-Fi + SNTP + `ring_chat_init()`, retry loop |
| `qw-talk` | 16 KB | internal | capture + STT + LLM + TTS |
| `lvgl_adapter` | BSP | — | LVGL timerhandler + flush |
| BSP IMU sampler | BSP | — | 250 Hz accel/gyro/temp |

| Buffer | Size | Where | Owner |
|---|---|---|---|
| capture (48 kHz mono, 10 s) | 960 KB | PSRAM | app |
| STT WAV (16 kHz mono) | 512 KB | PSRAM | `ring_chat` |
| STT base64 | ~700 KB | PSRAM | `ring_chat` |
| TTS base64 + PCM decode | 128 KB + 192 KB | PSRAM | `ring_chat` |
| TTS SSE line (growable) | ≤ ~100 KB | PSRAM | `ring_http` |
| history | small | heap | `ring_chat` |

There is **no codec** in this path, so the worker task needs no PSRAM stack:
16 KB of internal RAM covers the mbedTLS handshake and the HTTPS bodies. The
TTS callback's 3840-byte stereo frame is the largest stack object.

---

## 7. Configuration

**API key** — NVS, never source. The app calls
`ring_secret_get("qianwen", "api_key", QIANWEN_API_KEY, …)`: NVS wins; if it is
absent the build-time value is used and **persisted**, so later builds do not
need it. `ring_example_start()` calls `ring_nvs_init()` first (idempotent) —
without it `ring_secret_get()` reads NVS before it is up and always falls
through to the build-time value. The build-time value comes from `ring_secrets.h`, generated by
`main/CMakeLists.txt` from the environment — `tools/dev.sh` sources the
git-ignored `./secrets.local`, so the key never appears on a command line or in
`CMakeCache.txt`:

```bash
# preferred: put QIANWEN_API_KEY=sk-... in ./secrets.local, then
tools/dev.sh build 06_qianwenai_push_to_talk

# legacy -D form (honoured only when secrets.local is absent)
idf.py -DRING_APP=06_qianwenai_push_to_talk -DQIANWEN_API_KEY="sk-ws-..." \
       -B build/06_qianwenai_push_to_talk build
```

**Models / voice / prompt** — compile-time `#define`s at the top of
`qianwenai_push_to_talk.c`:

```c
#define STT_MODEL     "qwen3-asr-flash"
#define LLM_MODEL     "qwen3.8-flash"
#define TTS_MODEL     "qwen-audio-3.1-tts-flash"
#define TTS_VOICE     "longanhuan_v3.1"
#define SYSTEM_PROMPT "You are a helpful assistant. Answer concisely in English."
```

**Dependencies** — none new. `esp_http_client`, mbedTLS base64 and the
resampler tables are already linked; `main/idf_component.yml` needs no change
for this app.

**`sdkconfig.defaults`** (shared by all apps):

```ini
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y      # TLS buffers in PSRAM
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_FREERTOS_HZ=1000
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

**Partition** — the app is ~1.49 MB (measured `0x1759c0`), 50 % free of the
3000K `factory` partition. No partition change is needed.

---

## 8. Build & flash

```bash
tools/dev.sh build 06_qianwenai_push_to_talk
tools/dev.sh flash 06_qianwenai_push_to_talk
tools/dev.sh monitor
```

or, directly:

```bash
idf.py -DRING_APP=06_qianwenai_push_to_talk \
       -B build/06_qianwenai_push_to_talk build
idf.py -B build/06_qianwenai_push_to_talk -p /dev/ttyACM0 flash
idf.py -B build/06_qianwenai_push_to_talk -p /dev/ttyACM0 monitor
```

Use the **same `-B`** for build/flash/monitor. The other apps share `ringkit`
and `sdkconfig.defaults`, so verify them too (`00_glyph_grid_reveal`,
`09_hello_smoke_test`, and `07` — which shares the CA/clock plumbing).

---

## 9. Verification checklist

1. `00_glyph_grid_reveal`, `06_qianwenai_push_to_talk` and `07_qianwen_realtime_harness`
   all build from clean dirs.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
3. Boot log: `ringkit:` summary all `ok`, `app '06_qianwenai_push_to_talk' is on screen`,
   no `panic` / `assert` / `Guru Meditation`.
4. Connect: `Wi-Fi connected, IP …`, `ring_chat: initialised (model …)`, `ready`,
   then the button turns green.
5. A full turn: hold → `listening…` + level bar → release → `heard:` +
   transcript → `thinking…` + reply → `replying…` → audio out of the speaker.
   The log carries `stt/llm/tts: … in N ms` for each stage.
6. **Touch and voice need a human** — no host-side way to synthesise a press or
   judge the reply audio.

---

## 10. Known limitations

- **Latency.** Three sequential HTTPS calls, each with its own TLS handshake
  (no connection reuse). Measured ~8 s from release to the end of the reply
  audio (STT ≈ 2.4 s, LLM ≈ 3.8 s, TTS ≈ 2.3 s).
- **No barge-in.** The ring has no AEC; the mic is read only while the button
  is held, so the model cannot interrupt itself.
- **No streaming LLM.** `ring_chat_llm_stream()` is a placeholder that returns
  `ESP_ERR_NOT_SUPPORTED`; the reply appears all at once after the LLM call.
- **Capture cap.** 10 s per utterance (the STT WAV buffer alone allows ~16 s).
- **History cap.** 10 turns (`max_history_turns`); older turns are dropped
  from the request, not from the device.
- **TTS fallback.** The non-streaming TTS response is not supported — it
  returns an OSS URL on a different host and CA.
- **Half-duplex.** Same as every other voice example on this hardware.

---

## 11. References

- Qianwen platform: <https://www.qianwenai.com> · docs index
  <https://platform.qianwenai.com/docs/llms.txt>
- OpenAI-compatible API (STT/LLM):
  `platform.qianwenai.com/docs/api-reference/toolkitframework/openai-compatible/overview`
- TTS HTTP API: `…/api-reference/speech-synthesis/qwen-audio-tts/http-api`
- ASR: `…/api-reference/speech-recognition/qwen-asr/api-reference`
- In-repo: `notes/qianwen-ai-platform.md` (models & APIs),
  `notes/networking-https-webrtc.md` (HTTPS background),
  `examples/04_voice_loopback/` (worker-task audio pattern),
  `examples/07_qianwen_realtime_harness/` (the WebRTC alternative),
  `examples/10_qianwen_ws_voice_chat/` (the WebSocket alternative).
