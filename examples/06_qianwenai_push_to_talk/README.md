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

### The provider decision

"OpenAI-compatible voice assistant" sounds obvious, but Qianwen's compatible
mode does **not** expose the OpenAI audio endpoints. It supports Chat
Completions, Responses, Embedding, File, Batch and Conversations — there is
**no `/v1/audio/speech` and no `/v1/audio/transcriptions`**. So the cascade
uses Qianwen for all three stages:

| Option | STT | LLM | TTS | Verdict |
|---|---|---|---|---|
| **A. Qianwen ✅ chosen** | Chat Completions, `qwen3-asr-flash`, `input_audio` base64 Data URI | Chat Completions, `qwen3.8-flash` | native `SpeechSynthesizer` (SSE) | one host, one key, one pinned CA, no new dependency |
| B. Literal OpenAI | `/v1/audio/transcriptions` | `/v1/chat/completions` | `/v1/audio/speech` | rejected: needs an OpenAI key (none in the repo) and a second host/CA |
| C. Hybrid | OpenAI Whisper | Qianwen LLM | OpenAI TTS | rejected: two keys, two hosts — worst of both |

Option A is the only one that fits the repo's single `maas.qianwenaiapi.com`
host, single pinned CA and single NVS API key.

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
| `components/ringkit/src/ring_http.c` | HTTPS client: pinned CA, bearer, body + buffered SSE readers |
| `components/ringkit/src/ring_resample.c` | 48k→16k decimator (shared with `ring_ws`) |
| `components/ringkit/assets/qianwen_ca.pem` | pinned trust chain (see §4) |

### The `ring_chat` module

`ring_chat` is a **blocking** REST cascade: no event task, no callback
marshalling. Everything runs on the app's worker task, and the TTS audio
callback runs on that same task — so it can write to the speaker and take the
LVGL lock briefly. The app drives the stages individually
(`stt` → `add_user` → `llm_complete` → `tts`) so it owns the stage UI and can
show the transcript and the reply before TTS starts. There is deliberately no
`ring_chat_ask()` convenience wrapper.

```c
typedef struct {
    const char *stt_model;         /* "qwen3-asr-flash"              */
    const char *llm_model;         /* "qwen3.8-flash"                */
    const char *tts_model;         /* "qwen-audio-3.1-tts-flash"     */
    const char *voice;             /* "longanhuan_v3.1"              */
    const char *system_prompt;     /* optional                       */
    const char *stt_language;      /* optional ASR hint ("en")       */
    int         max_history_turns; /* 0 = no limit (use with care)   */
} ring_chat_cfg_t;

/* TTS audio chunk: 48 kHz / 16-bit / mono PCM. */
typedef void (*ring_chat_audio_cb_t)(const int16_t *pcm, size_t samples,
                                     void *user);

/* Text delta (LLM streaming) or final result (STT). */
typedef void (*ring_chat_text_cb_t)(const char *text, size_t len, void *user);

esp_err_t ring_chat_init(const ring_chat_cfg_t *cfg, const char *api_key);
void      ring_chat_reset_history(void);
esp_err_t ring_chat_stt(const int16_t *pcm48k, size_t samples,
                        char *out, size_t out_cap);
esp_err_t ring_chat_add_user(const char *text);
esp_err_t ring_chat_add_assistant(const char *text);
esp_err_t ring_chat_llm_complete(char *reply, size_t cap);
esp_err_t ring_chat_llm_stream(ring_chat_text_cb_t cb, void *user);
esp_err_t ring_chat_tts(const char *text, ring_chat_audio_cb_t cb, void *user);
```

Each stage opens its own `esp_http_client` and talks to the host with the
pinned CA and a bearer header; the response body is read with the streaming API
(`open` → `write` → `fetch_headers` → `read`) — **`esp_http_client_perform()`
must not be used**, because it swallows the body. Failures are returned as
`ESP_ERR_*` and the app maps them to a stage for the UI.

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

The pinned CA matters: the server sends **only its leaf certificate**, so
chain building fails against a root-only bundle. `components/ringkit/assets/qianwen_ca.pem`
pins the intermediate + root and is embedded by the build.

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
- The request may carry a top-level `asr_options.language` hint (e.g. `en`,
  `zh`); the app leaves `stt_language = NULL` and lets the model auto-detect.
- Response: `choices[0].message.content` — a **string** (OpenAI shape). Do not
  confuse it with the DashScope-native shape
  (`output.choices[0].message.content[0].text`, an array).
- The endpoint accepts wav/mp3/opus, up to 5 min / 2 GB. `"stream": true`
  would return SSE deltas but is not used.
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
- `extra_body.enable_thinking = false` keeps the reply fast and short.
- History is a plain `messages` array, capped at **10 turns**
  (`max_history_turns`); the compile-time cap is 20 pairs.
- `ring_chat_llm_stream()` exists for progressive display but is currently a
  placeholder (see §11).
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

- `format` ∈ {mp3, pcm, wav, opus}; `sample_rate` ∈
  {8000, 12000, 16000, 22050, 24000, 44100, 48000}. Requesting
  `format=pcm, sample_rate=48000` means **no resampling on playback** — the
  ring's native rate, and no WAV header to parse.
- **SSE, not the non-streaming form.** The non-streaming response returns
  `output.audio.url`, an OSS host
  (`dashscope-result-bj.oss-cn-beijing.aliyuncs.com`) — a second HTTPS
  connection to a different CA. SSE keeps everything on the pinned host and
  starts audio sooner.
- The stream is one `data:` JSON object per line, with `type` and the audio
  nested under `output`:
  `{"output":{"type":"sentence-synthesis","audio":{"data":"<base64 PCM>"}}}`.
  A sentence is `sentence-begin` → one or more `sentence-synthesis` chunks →
  `sentence-end` (`finish_reason: "stop"`). The callback fires once per
  `sentence-synthesis` chunk; unknown or malformed events are skipped, not
  fatal.
- `input.rate` (0.5–2.0), `input.volume` (0–100), `input.instruction`
  (dialect/emotion) and `input.language_hints` are available but unused.
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
- **STT resample.** 48 kHz → 16 kHz is an exact 3:1 decimation, done by the
  shared `ring_resample` module (extracted from `ring_ws`). `ring_chat` wraps a
  canonical 44-byte WAV header (PCM 16-bit mono 16 kHz) and base64-encodes it
  into the Data URI with `mbedtls_base64_encode`. The app never sees the WAV or
  the base64.
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
- **No AEC.** Half-duplex PTT, like every other voice example: the mic is only
  read while the button is held, so the model cannot hear itself.

---

## 6. Task & memory layout

The state machine is driven by the button and runs entirely on `qw-talk`:

```
PRESS (LVGL task)   → allocate the PSRAM capture buffer, start qw-talk
recording (qw-talk) → ring_audio_read → stereo_to_mono → PSRAM; update level
RELEASE              → stop capture
                      → ring_chat_stt           → show transcript
                      → add_user + llm_complete  → show reply
                      → ring_chat_tts           → mono_to_stereo → speaker → idle
error (any stage)    → sticky "! — tap to retry"; the next press starts a turn
```

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

**Partition** — the app is ~1.49 MB, 50 % free of the 3000K `factory`
partition. No partition change is needed.

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

1. **Host probe first.** Exercise all three endpoints with a real recording
   before trusting the device: STT with a 16 kHz WAV Data URI, the LLM Chat
   Completions shape, and the TTS SSE stream (`format=pcm, sample_rate=48000`).
   Record the exact request/response JSON.
2. `00_glyph_grid_reveal`, `06_qianwenai_push_to_talk` and
   `07_qianwen_realtime_harness` all build from clean dirs.
3. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
4. Boot log: `ringkit:` summary all `ok`,
   `app '06_qianwenai_push_to_talk' is on screen`, no `panic` / `assert` /
   `Guru Meditation`.
5. Connect: `Wi-Fi connected, IP …`, `ring_chat: initialised (model …)`,
   `ready`, then the button turns green.
6. A full turn: hold → `listening…` + level bar → release → `heard:` +
   transcript → `thinking…` + reply → `replying…` → audio out of the speaker.
   The log carries `stt/llm/tts: … in N ms` for each stage.
7. **Touch and voice need a human** — no host-side way to synthesise a press or
   judge the reply audio.

---

## 10. Known limitations and risks

- **Latency.** Three sequential HTTPS calls, each with its own TLS handshake
  (no connection reuse). Measured ~8 s from release to the end of the reply
  audio (STT ≈ 2.4 s, LLM ≈ 3.8 s, TTS ≈ 2.3 s).
- **No barge-in, no AEC.** The ring has no acoustic echo cancellation; the mic
  is read only while the button is held, so the model cannot interrupt itself.
- **No streaming LLM.** `ring_chat_llm_stream()` is a placeholder that returns
  `ESP_ERR_NOT_SUPPORTED`; the reply appears all at once after the LLM call.
- **Capture cap.** 10 s per utterance (the STT WAV buffer alone allows ~16 s).
- **History cap.** 10 turns (`max_history_turns`); older turns are dropped
  from the request, not from the device.
- **TTS fallback.** The non-streaming TTS response is not supported — it
  returns an OSS URL on a different host and CA.
- **Large HTTPS bodies.** A 10 s utterance is a ~427 KB base64 body. All large
  buffers live in PSRAM and `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` puts the TLS
  buffers there too.
- **ASR sample rate is undocumented.** 16 kHz WAV was verified against the
  live endpoint; if it is ever rejected, the fallback is 48 kHz WAV (3× the
  body, 3× the PSRAM).
- **Console back-pressure.** Log snippets are bounded (`%.200s`); never log a
  whole JSON body or WAV.

---

## 11. Future work

- **Streaming LLM with a sentence bridge.** Stream the reply and feed
  completed sentences into TTS immediately, overlapping LLM generation with
  playback — the single biggest latency win available.
- **Runtime settings.** Model / voice / system prompt / ASR language are
  compile-time `#define`s; a settings UI would need NVS-backed storage.
- **Function calling.** The endpoint supports tool calls; not implemented.

---

## 12. References

- Qianwen platform: <https://www.qianwenai.com> · docs index
  <https://platform.qianwenai.com/docs/llms.txt>
- OpenAI-compatible API (STT/LLM):
  `platform.qianwenai.com/docs/api-reference/toolkitframework/openai-compatible/overview`
- TTS HTTP API: `…/api-reference/speech-synthesis/qwen-audio-tts/http-api`
- ASR: `…/api-reference/speech-recognition/qwen-asr/api-reference`
- Non-streaming vs SSE and the voice list:
  `platform.qianwenai.com/docs/developer-guides/speech/tts`
- In-repo: `notes/qianwen-ai-platform.md` (models & APIs),
  `notes/networking-https-webrtc.md` (HTTPS background),
  `examples/04_voice_loopback/` (worker-task audio pattern),
  `examples/07_qianwen_realtime_harness/` (the WebRTC alternative),
  `examples/10_qianwen_ws_voice_chat/` (the WebSocket alternative).
