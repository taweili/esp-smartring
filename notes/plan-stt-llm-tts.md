# STT → LLM → TTS rewrite of `examples/06_qianwenai_push_to_talk` — implementation plan

**Status:** implemented (`ring_chat` + the rewritten `06_qianwenai_push_to_talk`).
**Decisions locked:** Option A (Qianwen for all three stages); keep the
`06_qianwenai_push_to_talk` name/directory.
**Companion to:** `notes/qianwen-ai-platform.md` (§1 protocols, §2.7 TTS, §2.9 ASR, §3 transports),
`notes/networking-https-webrtc.md` §5.1 (OpenAI REST on ESP-IDF).
**Goal:** replace 06's WebRTC speech-to-speech session with the classic cascade
**mic → STT → LLM → TTS → speaker**, all over HTTPS/TCP 443.

---

## 0. Verdict and the one decision that shapes everything

The cascade is a **different transport, not a tweak**. 06 today is a realtime
WebRTC session (`ring_net` + `esp_peer` + Opus + DTLS/SRTP/SCTP). The rewrite
deletes all of that and replaces it with three sequential HTTPS POSTs:

```
record → POST chat/completions (ASR)  → POST chat/completions (LLM)
       → POST SpeechSynthesizer (TTS, SSE) → stream PCM to speaker
```

Consequences:

- **No `esp_peer`, no libsrtp, no DTLS, no Opus, no `ring_net`.** The image
  drops from 06's 2.78 MB toward 05's 1.56 MB (05 is the HTTPS-only app).
- **Works on this bench network.** All three calls are TCP/443; the UDP block
  that kills 06/07 (README §8) is irrelevant.
- **Latency is the price.** STT + LLM + first TTS chunk ≈ 1.5–3 s, versus a
  realtime session. The UI must show the stage (`heard…`, `thinking…`,
  `speaking…`) so the wait is legible.
- **Turn-taking is manual push-to-talk** — the same button-gated shape as 06,
  but now the "end of turn" is simply "button released".

### Provider decision — locked: Option A (Qianwen)

The phrase "OpenAI API" is ambiguous, and Qianwen's compatible mode does **not**
expose the OpenAI audio endpoints. Verified against
`platform.qianwenai.com/docs/api-reference/toolkitframework/openai-compatible/overview`:
compatible mode supports **Chat Completions, Responses, Embedding, File, Batch,
Conversations** — there is **no `/v1/audio/speech` and no
`/v1/audio/transcriptions`**.

| Option | STT | LLM | TTS | Verdict |
|---|---|---|---|---|
| **A. Qianwen ✅ chosen** | Chat Completions, model `qwen3-asr-flash`, `input_audio` base64 Data URI | Chat Completions, model `qwen3.8-flash` | Native `POST /api/v1/services/audio/tts/SpeechSynthesizer` (SSE) | One host, one key, one pinned CA, no new dependency |
| **B. Literal OpenAI** | `api.openai.com/v1/audio/transcriptions` | `api.openai.com/v1/chat/completions` | `api.openai.com/v1/audio/speech` | Rejected: needs an OpenAI key (none in repo), a second CA/host, and is a different provider |
| **C. Hybrid** | OpenAI Whisper | Qianwen LLM | OpenAI TTS | Rejected: two keys, two hosts — worst of both |

**Chosen: A.** It is the only option that fits the repo's single
`maas.qianwenaiapi.com` host, single pinned CA (`ring_net_ca_pem()`) and single
NVS API key. The rest of this plan assumes A throughout. (If B were ever
wanted, only §2 and §4 change — endpoint paths, multipart upload for STT,
WAV/MP3 response for TTS — the module and app shape are identical.)

---

## 1. Target architecture

```
   touch ──► LVGL task ──► 06 app (rewritten)
                            │  PRESS  → start capture (PSRAM buffer)
                            │  RELEASE → run cascade on a worker task
                            ▼
                       ring_chat  (new ringkit module)
                            │  1. resample 48k mono → 16k, wrap WAV, base64
                            │  2. POST /compatible-mode/v1/chat/completions  (STT)
                            │  3. POST /compatible-mode/v1/chat/completions  (LLM)
                            │  4. POST /api/v1/services/audio/tts/SpeechSynthesizer
                            │       + X-DashScope-SSE: enable                (TTS)
                            ▼
                maas.qianwenaiapi.com   (TCP/443, pinned CA)
```

App-facing audio stays **48 kHz / 16-bit / mono PCM**, exactly like `ring_net`
and `ring_ws`, so the capture/playback code is portable across 06/07/10.

---

## 2. Verified API facts

All three calls go to `https://maas.qianwenaiapi.com` with
`Authorization: Bearer <qianwen key>`. TLS uses the embedded
`assets/qianwen_ca.pem` via `ring_net_ca_pem()` (the server sends only its leaf
cert; the intermediate must be supplied — same reason as `ring_net`).

### 2.1 STT — OpenAI-compatible Chat Completions

```
POST https://maas.qianwenaiapi.com/compatible-mode/v1/chat/completions
Content-Type: application/json
Authorization: Bearer <key>

{
  "model": "qwen3-asr-flash",
  "messages": [
    {"role": "user", "content": [
      {"type": "input_audio",
       "input_audio": {"data": "data:audio/wav;base64,<...>"}}
    ]}
  ],
  "asr_options": {"enable_itn": true}      // non-standard; top-level
}
```

- Response: `choices[0].message.content` (**string**, OpenAI shape).
  Note the DashScope-native shape is different
  (`output.choices[0].message.content[0].text`, an array) — do not mix them.
- Streaming (`"stream": true`) returns SSE chunks with
  `choices[0].delta.content`; useful for long utterances, optional in v1.
- `asr_options.language` (e.g. `zh`, `en`) improves accuracy when known.
- Max 5 min / 2 GB; formats wav/mp3/opus. **Sample rate is not documented —
  verify 16 kHz WAV in P0 before building the resampler.**
- Optional `system` message for hotwords/context (first in `messages`).

### 2.2 LLM — OpenAI-compatible Chat Completions

```
POST https://maas.qianwenaiapi.com/compatible-mode/v1/chat/completions
{
  "model": "qwen3.8-flash",                // or qwen3.7-plus; make configurable
  "messages": [
    {"role": "system", "content": "<system prompt>"},
    {"role": "user", "content": "<transcript>"},
    {"role": "assistant", "content": "<previous reply>"},
    ...
  ],
  "stream": false
}
```

- Response: `choices[0].message.content` (string).
- Streaming SSE (`stream: true`) gives `choices[0].delta.content`; enables
  showing the reply as it is generated and starting TTS earlier (P4).
- History is a plain `messages` array; cap it (e.g. last 10 turns via an
  explicit constant) to bound request size and latency. `max_history_turns` is
  a realtime-API concept, not applicable here.

### 2.3 TTS — native DashScope HTTP (SSE)

```
POST https://maas.qianwenaiapi.com/api/v1/services/audio/tts/SpeechSynthesizer
Content-Type: application/json
Authorization: Bearer <key>
X-DashScope-SSE: enable

{
  "model": "qwen-audio-3.1-tts-flash",
  "input": {
    "text": "<reply>",
    "voice": "longanhuan_v3.1",
    "format": "pcm",
    "sample_rate": 48000
  }
}
```

- `format` ∈ {mp3, pcm, wav, opus}; `sample_rate` ∈
  {8000, 12000, 16000, 22050, 24000, 44100, 48000}. **Request
  `format=pcm, sample_rate=48000`** → raw 16-bit mono PCM at the ring's native
  rate: no WAV header to parse, no resampling on playback.
- SSE response lines: `data: {"output":{"type":"sentence-synthesis",
  "audio":{"data":"<base64 PCM>"}}, ...}`. Sentence lifecycle events are
  `sentence-begin` / `sentence-synthesis` / `sentence-end`.
- **Why SSE and not non-streaming:** the non-streaming response returns
  `output.audio.url`, an OSS host
  (`dashscope-result-bj.oss-cn-beijing.aliyuncs.com`) — a second HTTPS
  connection to a different CA. SSE keeps everything on the pinned host and
  starts audio sooner. Keep non-streaming as a documented fallback only.
- `input.rate` (0.5–2.0), `input.volume` (0–100), `input.instruction`
  (dialect/emotion), `input.language_hints` are available.
- **SSE sentence lifecycle:** when the text spans multiple sentences, the stream
  delivers `sentence-begin` → (multiple `sentence-synthesis` events with
  `audio.data` base64) → `sentence-end` per sentence. The callback fires once
  per `sentence-synthesis` chunk; write each to the speaker as it arrives.
  Log the sentence index for diagnostics. The final `sentence-end` carries
  `finish_reason: "stop"` but no special action is needed.

---

## 3. Audio pipeline

```
mic 48k stereo ──ring_audio_read──► ring_audio_stereo_to_mono ──► PSRAM buffer
                                                                     │
                          48k mono ──decimate 3:1──► 16k mono ──WAV──► base64 ──► STT
                                                                     │
TTS SSE ──base64──► 48k mono PCM ──ring_audio_mono_to_stereo──► ring_audio_write
```

- **Capture:** `ring_audio_read()` 3840-byte stereo frames → fold to mono →
  append to a PSRAM buffer. Cap one utterance at ~10 s (960 KB mono); the
  button is the gate, so the cap is a safety net, not a feature.
- **STT resample:** 48 kHz → 16 kHz is an exact 3:1 decimation. The repo
  already has the generated polyphase tables for 48→16k (`ring_ws_resample.c`)
  and the decimator code (static in `ring_ws.c`). **Extract only the decimator
  into a shared `ring_resample.[ch]`** as a standalone refactor (see §6); leave
  the interpolator (24→48k) private in `ring_ws` — it is the only consumer.
  Do not copy the tables.
- **WAV:** 44-byte canonical header, PCM 16-bit mono 16 kHz. `mbedtls_base64_encode`
  for the Data URI (already linked).
- **Playback:** TTS gives 48 kHz mono PCM directly → `ring_audio_mono_to_stereo`
  → `ring_audio_set_mute(false)` / `write` / `set_mute(true)` per chunk to avoid
  pops (the BSP's fixed mute sequence).
- **No AEC:** half-duplex PTT, same as 06. The mic is closed while TTS plays.

---

## 4. New ringkit module: `ring_chat`

A blocking REST cascade module. It is deliberately **not** `ring_net` (WebRTC)
and **not** `ring_ws` (WebSocket); those stay untouched so 07 and 10 remain
stable.

```c
/* components/ringkit/include/ringkit/ring_chat.h */
typedef struct {
    const char *stt_model;     /* "qwen3-asr-flash"                  */
    const char *llm_model;     /* "qwen3.8-flash"                    */
    const char *tts_model;     /* "qwen-audio-3.1-tts-flash"         */
    const char *voice;         /* "longanhuan_v3.1"                  */
    const char *system_prompt; /* optional                          */
    int         max_history_turns; /* 0 = default (e.g. 10)          */
} ring_chat_cfg_t;

/* TTS audio chunk: 48 kHz / 16-bit / mono PCM. */
typedef void (*ring_chat_audio_cb_t)(const int16_t *pcm, size_t samples,
                                     void *user);

/* Text delta (LLM streaming) or final transcript (STT). */
typedef void (*ring_chat_text_cb_t)(const char *text, size_t len, void *user);

esp_err_t ring_chat_init(const ring_chat_cfg_t *cfg, const char *api_key);
void      ring_chat_reset_history(void);

/* STT: 48k mono PCM in, transcript out. */
esp_err_t ring_chat_stt(const int16_t *pcm48k, size_t samples,
                        char *out, size_t out_cap);

/* LLM: append a user turn, then complete. */
esp_err_t ring_chat_add_user(const char *text);
esp_err_t ring_chat_add_assistant(const char *text);
esp_err_t ring_chat_llm_complete(char *reply, size_t cap);      /* non-streaming */
esp_err_t ring_chat_llm_stream(ring_chat_text_cb_t text_cb, void *user); /* P4 */

/* TTS: stream PCM to the callback as it arrives. */
esp_err_t ring_chat_tts(const char *text, ring_chat_audio_cb_t cb, void *user);

/* No ring_chat_ask() convenience wrapper: the app calls the stages individually
 * so it owns the stage UI (transcript → reply → speaking) and can surface each
 * intermediate result before TTS starts. */
```

Design notes:

- **Threading:** all functions block; call them from the app's worker task
  (`qw-talk`). No event task, no callback marshalling. The TTS callback runs on
  that same task, so it can write to the speaker and take the LVGL lock for
  brief UI updates (the 04/06 pattern). The app calls the individual functions
  (`stt` → `add_user` → `llm_complete` → `tts`) so it owns the stage UI and can
  show transcript/reply before TTS starts.
- **Stack:** no codec → no 64 KB PSRAM stack. Start at 12–16 KB internal
  (mbedTLS handshake + large HTTPS bodies) and measure the high-water mark. Do
  **not** copy 06's `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` workaround.
- **Buffers:** STT WAV + base64 (5 s @16k = 160 KB + 214 KB base64) and the LLM
  history in **PSRAM**. `esp_http_client` with `buffer_size` raised and
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.
- **JSON:** request building is simple `snprintf`; response parsing reuses the
  `json_get_str` / `json_type_is` helpers currently static in `ring_net.c`.
  Extract them into `ring_json.[ch]` as a **standalone change** (see §6) — do
  not couple that refactor to this feature.
- **SSE parsing:** line-buffered `data: {…}`; ignore `:` keepalives and blank
  lines; parse each complete JSON object. `esp_http_client_read` in a loop.
- **Connection reuse:** keep a single `esp_http_client_handle_t` and call
  `esp_http_client_set_url()` between stages instead of cleanup+reinit. Each
  HTTPS handshake is ~1–3 round-trips; reusing saves 2× that per turn. Only
  `cleanup` on error.
- **Errors:** surface HTTP status + the API `error` object; the app maps them
  to a stage (`stt` / `llm` / `tts`) for the UI.

---

## 5. App rewrite: `examples/06_qianwenai_push_to_talk`

Keep the directory, name and RING_APP value (minimal churn); replace the
implementation and README. The "push to talk" name still fits.

### UI (evolve 06's, keep the round button)

| State | Button | Status | What happens |
|---|---|---|---|
| Idle | green, `HOLD TO TALK` | "ready — press & hold" | mic closed |
| Recording | red, `TALK` | "listening…" + level | capture to PSRAM |
| Transcribing | orange, `…` | "heard: …" | STT POST |
| Thinking | orange, `…` | "thinking…" + reply draft | LLM POST |
| Speaking | blue, `REPLY` | "replying…" | TTS SSE → speaker |
| Error | red, `!` | "STT/LLM/TTS failed — tap to retry" | retry |

Show the recognised utterance and the model reply in two labels (the 07
pattern), plus the level bar. A stage line makes the 1.5–3 s cascade legible.

### State machine

1. **PRESS** (LVGL task): allocate/clear the capture buffer, start the
   recording loop on `qw-talk`.
2. **Recording** (`qw-talk`): read mic frames, fold to mono, append; update the
   level bar. Stop on `s_stop` or the 10 s cap.
3. **RELEASE**: stop capture → `ring_chat_stt()` → show transcript →
   `ring_chat_add_user()` + `ring_chat_llm_complete()` → show reply →
   `ring_chat_tts()` with a callback that folds to stereo and writes to the
   speaker → idle.
4. **Errors**: per-stage timeout (STT 15 s, LLM 20 s, TTS **30–40 s** — the TTS
   SSE `esp_http_client_read` loop runs on the same task as
   `ring_audio_write()`, and blocking writes extend the wall-clock time) and
   retry.
5. **Connect task**: Wi-Fi + SNTP before the first turn (reuse
   `ring_wifi_*` and `ring_net_sync_clock()`; see the coupling note in §6).

### Task layout

| Task | Stack | RAM | Role |
|---|---|---|---|
| `qw-connect` | 8 KB | internal | Wi-Fi + SNTP, retry |
| `qw-talk` | 12–16 KB | internal | capture + STT + LLM + TTS |
| `ring-net-*` / `ring-ws-*` | — | — | not used by 06 any more |

---

## 6. Dependencies, config, standalone refactors

- **No new managed dependency.** `esp_http_client`, `esp-tls`, `mbedtls`
  (base64) and the resampler tables are already present.
- **`main/idf_component.yml`:** no change for 06. `espressif/esp_peer` and
  `espressif/esp_audio_codec` stay for 07 (and the manifest comment should be
  updated to say 07, not 06).
- **`sdkconfig.defaults`:** existing config suffices —
  `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y`, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`,
  cert bundle, SNTP, `CONFIG_FREERTOS_HZ=1000`. No new options.
- **Standalone refactor 1 — `ring_resample.[ch]`:** move the generated
  **decimator** tables (`ring_ws_decim_coeffs` in `ring_ws_resample.c`) and
  the `decim_*` code out of `ring_ws.c` into a shared module. Leave the
  interpolator (`ring_ws_interp_coeffs`, `interp_*`) private in `ring_ws` — it
  is the only consumer. Rebuild 10 to prove no regression. Land this *before*
  `ring_chat`, separately.
- **Standalone refactor 2 — `ring_json.[ch]`:** extract `json_get_str` /
  `json_type_is` from `ring_net.c`; rebuild 06 (old) and 07 to prove no
  regression. Also land separately.
- **Clock coupling:** `ring_net_sync_clock()` lives in `ring_net.c`. Either
  call it (ring_net is linked anyway) or add a tiny generic `ring_clock_sync()`
  and have both use it. Prefer the latter if the refactor is cheap; otherwise
  call `ring_net_sync_clock()` and note the coupling.
- **Partition:** 06 shrinks well below the 3000K app partition (05 is 1.56 MB).
  Measure and record; no change expected.

---

## 7. Phased plan

| Phase | Deliverable | Effort |
|---|---|---|
| **P0** | **Host-side probe (do first).** `curl` all three endpoints with a real recording. **STT:** send the same clip as 16 kHz WAV **and** 48 kHz WAV (Data URI); verify acceptance and the response shape. **TTS SSE:** verify `format=pcm, sample_rate=48000` returns raw PCM and the SSE event shape. **LLM:** verify the Chat Completions response. Record exact request/response JSON for all three. | ½ day |
| **P1** | `ring_chat` STT + LLM only: init, `ring_chat_stt()`, `ring_chat_llm_complete()`, log transcript and reply. Feed from a short PCM clip embedded via `EMBED_FILES` (e.g. ~2 s 48 kHz mono from `scripts/assets/test_clip.pcm`). No UI, no mic capture, no TTS. | ½–1 day |
| **P2** | `ring_chat_tts()` SSE streaming + playback; mute sequencing; per-stage errors. | ½–1 day |
| **P3** | App rewrite: capture buffer, state machine, UI stages, connect task, secrets. One full turn on device. | 1 day |
| **P4** | Polish: LLM streaming text with a **buffered sentence bridge** that feeds completed sentences into TTS immediately (overlapping LLM generation with TTS playback), history cap, settings (model / voice / system prompt / language), function calling (optional). | ½ day |

Realistic: **~3 days to P3** for someone who knows `ringkit`.

---

## 8. Files touched

| File | Change |
|---|---|
| `components/ringkit/src/ring_chat.c` | new |
| `components/ringkit/include/ringkit/ring_chat.h` | new |
| `components/ringkit/src/ring_resample.c` + `include/ringkit/ring_resample.h` | new (extracted from `ring_ws_resample.c` + `ring_ws.c`) |
| `components/ringkit/src/ring_json.c` + `include/ringkit/ring_json.h` | new (extracted from `ring_net.c`) — optional standalone |
| `components/ringkit/CMakeLists.txt` | add `ring_chat.c`, `ring_resample.c`; includes already cover `esp_http_client`/`mbedtls` |
| `components/ringkit/include/ringkit/ringkit.h` | include `ring_chat.h` |
| `examples/06_qianwenai_push_to_talk/qianwenai_push_to_talk.c` | rewrite |
| `examples/06_qianwenai_push_to_talk/README.md` | rewrite (cascade architecture, latency, PTT) |
| `examples/README.md`, `AGENTS.md` | update 06 description; manifest comment (esp_peer is 07 now) |
| `notes/qianwen-ai-platform.md` | optional: note the STT/TTS REST shapes |

---

## 9. Risks (rank before coding)

| # | Risk | Mitigation |
|---|---|---|
| 1 | ASR rejects 16 kHz WAV or wants a different container | P0 sends both 16k and 48k WAV; fallback is 48 kHz WAV (3× body, bigger PSRAM budget — note the impact in the pipeline) |
| 2 | TTS SSE event shape differs from the docs | P0 dumps a real SSE stream; parse defensively (unknown fields ignored) |
| 3 | STT response shape confusion (OpenAI string vs DashScope array) | Parse `choices[0].message.content` as a string; log the raw body at DEBUG |
| 4 | Latency 1.5–3 s feels dead | Stage UI + level meter; P4 adds streaming LLM/TTS overlap |
| 5 | Large HTTPS bodies exhaust internal RAM | PSRAM buffers, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, raised `buffer_size`. If 48 kHz STT is required the WAV is 3× bigger (1.3 MB base64 for 10 s) — verify this changes the latency/PSRAM budget |
| 6 | TLS handshake stack | Measure `uxTaskGetStackHighWaterMark`; start 16 KB, do not copy 06's 64 KB |
| 7 | Console back-pressure (USB-Serial-JTAG) | Never log whole JSON/WAV; bounded `%.200s` snippets |
| 8 | History grows unbounded | Cap turns; store in PSRAM |
| 9 | TTS non-streaming fallback needs a second CA (OSS) | Use SSE; document non-streaming as not supported without the cert bundle |
| 10 | `ring_net_sync_clock()` coupling | Prefer a generic `ring_clock_sync()`; otherwise call `ring_net_sync_clock()` (ring_net is still linked for 07, but 06 will no longer need it at runtime — accept the link overhead or compile it out) |
| 11 | Two refactors (resampler, JSON) regress 07/10 | Land them as standalone commits with a rebuild of 07 and 10 before `ring_chat` |
| 12 | No AEC → the ring hears itself | PTT half-duplex; mic closed during TTS |

---

## 10. Verification checklist

1. **Host probe first** (P0): all three endpoints return the expected shapes with
   a real recording; save the request/response samples to `notes/`.
2. **Builds from clean `-B` dirs:** `00_glyph_grid_reveal`, `09_hello_smoke_test`,
   `06_qianwenai_push_to_talk`, `07_qianwen_realtime_harness` (07 shares
   `ringkit`; the refactors must not regress it), and `10_qianwen_ws_voice_chat`
   if `ring_ws` changed.
3. **Vendored overrides still win** (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
4. **Image size** measured against the 3000K partition (expect ~1.5–1.8 MB).
5. **Boot log:** `ringkit:` summary all `ok`, `app '06_qianwenai_push_to_talk'
   is on screen`, no `panic` / `assert` / `Guru Meditation`.
6. **On this bench network (UDP blocked):** a full turn works — the key
   advantage over the current 06/07.
7. **Stage timings** logged (STT ms, LLM ms, first TTS chunk ms) to quantify the
   cascade latency.
8. **Human check:** transcript correctness, reply relevance, TTS intelligibility,
   no speaker pops. The log proves the pipeline, not the conversation.

---

## 11. Open questions

1. **Provider:** ✅ locked — Option A (Qianwen for all three stages).
2. **Models:** `qwen3.8-flash` vs `qwen3.7-plus` for the LLM;
   `qwen-audio-3.1-tts-flash` vs `-plus` for TTS; `qwen3-asr-flash` for STT.
   Make them compile-time configurable and log the choices.
3. **Streaming:** non-streaming LLM in v1 (simpler) vs SSE from the start
   (lower perceived latency). TTS should be SSE regardless.
4. **History:** manual `messages` array (simple, full control) vs the
   Conversations API (server-managed, but another endpoint and semantics).
5. **STT language:** fixed (`zh`/`en`) or omitted for auto-detect.
6. **Keep the 06 name?** ✅ locked — keep `06_qianwenai_push_to_talk`
   (directory, RING_APP and build dirs unchanged).
7. **Resampler:** extract (clean) vs duplicate a minimal decimator (faster, but
   a second copy of the tables). Recommend extract.

---

## 12. Sources

- `platform.qianwenai.com/docs/api-reference/toolkitframework/openai-compatible/overview`
  — compatible API list (no `/audio/speech`, no `/audio/transcriptions`).
- `platform.qianwenai.com/docs/api-reference/speech-recognition/qwen-asr/api-reference`
  — OpenAI-compatible STT via `/compatible-mode/v1/chat/completions` with
  `input_audio`; response `choices[0].message.content`.
- `platform.qianwenai.com/docs/api-reference/speech-synthesis/qwen-audio-tts/http-api`
  — `SpeechSynthesizer`, `format`/`sample_rate`, SSE via `X-DashScope-SSE`.
- `platform.qianwenai.com/docs/developer-guides/speech/tts` — non-streaming vs
  SSE, voice list.
- In-repo: `notes/qianwen-ai-platform.md`, `notes/networking-https-webrtc.md` §5.1,
  `examples/05_wifi_https_demo` (HTTPS + probe pattern), `examples/06_*`
  (button/state-machine pattern), `examples/07_*` (transcript UI pattern).
