# 千问 AI 平台 (qianwenai.com) — models & API reference

**Researched:** 2026-11-04, updated 2026-11-06 (Realtime API deep-dive + QUIC/AOQ
internals). Sources: `www.qianwenai.com`, `platform.qianwenai.com/docs`
(the official docs, incl. the full index at `platform.qianwenai.com/llms.txt`), and the
model-market pages.

> **What this site is.** `qianwenai.com` is the **千问 AI 平台** — Alibaba's
> consumer-facing AI platform (the cloud/console is `platform.qianwenai.com`,
> the API gateway is `maas.qianwenaiapi.com`). It is the same family as Alibaba
> Cloud **百炼 / Model Studio (DashScope)**: the model lineup and the DashScope
> API shape are shared, but the Qianwen platform has its own console, API keys
> and gateway. If you have seen DashScope docs, most of the API below will look
> familiar.
>
> It is **not** the same as the open-source Qwen models on Hugging Face — this
> is the hosted API platform, plus a set of consumer apps (千问 chat, 千问办公,
> Qoder, 秒悟, 万有无界, etc.).

---

## 1. API support (protocols & endpoints)

The platform is **multi-protocol**. The same models are reachable through
OpenAI-compatible, Anthropic-compatible and native DashScope HTTP APIs, plus
three realtime transports.

| Protocol | Base URL / endpoint | Auth | Notes |
|---|---|---|---|
| **OpenAI-compatible Chat** | `https://maas.qianwenaiapi.com/compatible-mode/v1` → `POST /chat/completions` | `Authorization: Bearer <key>` | Drop-in for any OpenAI SDK/client. Streaming, tool calling, structured output, multimodal input. |
| **OpenAI-compatible Responses** | `.../compatible-mode/v1/responses` (create/retrieve/delete, input-items list) | same | Built-in web search / code interpreter / web extraction; `previous_response_id` for multi-turn. |
| **Anthropic-compatible Messages** | `https://maas.qianwenaiapi.com/apps/anthropic` → `POST /v1/messages` | `x-api-key` **or** `Authorization: Bearer` | For Claude Code / Claude Desktop / Anthropic SDK. No `/v1/models` discovery endpoint. |
| **DashScope native (HTTP)** | `https://maas.qianwenaiapi.com/api/v1/services/aigc/text-generation/generation` (text) · `.../multimodal-generation/generation` (multimodal) | `Authorization: Bearer <key>` | Fullest parameter set. Add `X-DashScope-SSE: enable` for streaming. |
| **WebSocket (Realtime)** | `wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=<model>` | `Authorization: Bearer <key>` in the handshake | Session protocol (`session.update` → input → results). |
| **WebSocket (Inference)** | `wss://maas.qianwenaiapi.com/api-ws/v1/inference` | same | Task protocol (`run-task` → binary audio / text → `finish-task`). Used by TTS/ASR/CosyVoice. |
| **WebRTC (Realtime)** | `POST https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=<model>` | `Authorization: Bearer <key>` | Body = Offer SDP (`Content-Type: application/sdp`), response = Answer SDP. Media over RTP, events over DataChannel. |
| **AOQ (AI over QUIC)** | same URL with header `x-dashscope-rtc-transport: moq` | server-side API key → client token | Returns `aoqTokenForClient`, `sid`, relay endpoints; connect with the AOQ SDK (Android/iOS/HarmonyOS). Best weak-network/AEC behaviour. |

**Async generation APIs.** Image / video / 3D / audio generation are
**asynchronous**: `POST` to create a task → get a `task_id` → poll
`GET .../tasks/{task_id}` until done. There is also a **Batch API** for
large-scale text jobs (up to 50% cheaper).

**Platform APIs.** Conversations (server-managed multi-turn context), file
upload/management (10k files / 100 GB), Batch, and a model-list endpoint
(`GET /api/v1/models`).

**Authentication.**
- API keys are created in the console (`platform.qianwenai.com/home/api-keys`).
- `sk-ws-…` = pay-as-you-go key (older keys `sk-…`); `sk-sp-…` = Token Plan
  subscription key (different domain: `token-plan.maas.qianwenaiapi.com`).
- Send `Authorization: Bearer $DASHSCOPE_API_KEY`. For AOQ/WebRTC, keep the API
  key on your app server and issue short-lived client credentials.
- Optional `X-DashScope-DataInspection` header for data-compliance controls.

**SDKs.** DashScope SDKs (Python / Java / Node / Go), the OpenAI SDK (just
change `base_url`), the Anthropic SDK (change `base_url`), Qwen Code, and
Android/iOS/HarmonyOS SDKs for the realtime/AOQ paths. There is also an
OpenAPI spec set (`openapi-openai-chat.json`, `openapi-anthropic.json`,
`openapi-dashscope.json`, `openapi-openai-responses.json`, …).

---

## 2. Models by category

The model catalog is large (400+ models). Below is the organized list; model
IDs are the `model=` strings. Full specs/pricing live at
`www.qianwenai.com/models/<id>` (the docs list IDs; the market pages carry the
price tables).

### 2.1 Text generation (LLM)

Qwen flagships and tiers (all Qwen3+ support "thinking" via `enable_thinking`;
Responses API uses `reasoning.effort`):

| Model | Context | Max output | Notes |
|---|---|---|---|
| `qwen3.8-max` | 1M | 128k | Flagship; 2.4T-param MoE; text+image+video; built-in tools |
| `qwen3.8-flash` | 1M | 128k | Fast/cheap multimodal; OpenAI & Anthropic protocol compatible |
| `qwen3.8-2.4t-a95b` | 1M | 128k | Qwen3.8 MoE variant |
| `qwen3.7-max` / `-plus` / `-flash` | 1M | 64k | Previous flagship/balance/fast tiers (snapshot versions too) |
| `qwen3.6-plus` / `-flash` | 1M | 64k | Plus = balance, Flash = fast |
| `qwen3.5-plus` / `-flash` | 1M | 64k | Also 256k MoE variants (27b/35b-a3b/122b-a10b/397b-a17b) |
| `qwen-plus` / `qwen-flash` / `qwen-max` | 1M / 32k | 32k / 8k | Older general tiers (snapshot versions) |
| `qwen-long` | **10M** | — | Long-context, file upload + reference |
| `qwen3-coder-flash` | 1M | 64k | Coding/agent |
| `qwen2.5-*-instruct` (0.5b…72b) | 32k–1M | 8k | Open-source Qwen2.5 lineup |
| `qwen2.5-coder-*-instruct` | 32k–128k | 8k | Coder lineup |
| `qwq-32b`, `qwq-32b-preview` | 32k–128k | 8k–16k | Reasoning models |
| `qwen-mt-uni` / `-plus` / `-flash` / `-lite` | 16k | — | Machine translation (92 languages, terminology) |
| `qwen-doc-turbo` | — | — | Document understanding / structured extraction |
| `qwen-deep-research` | — | — | Multi-step research + web search |
| `tongyi-intent-detect-v3` | — | — | Intent detection |
| `tongyi-xiaomi-analysis-flash` / `-pro` | — | — | Dialogue analysis / QA |
| `decision-model-preview` | — | — | Structured classification / scoring with probabilities |
| `qwen-plus-character` / `qwen-flash-character` | 32k | — | Role-play |

**Third-party models** (same API format as Qwen; prefixes indicate supplier):

| Vendor | Models |
|---|---|
| DeepSeek | `deepseek-v4-pro`, `deepseek-v4.1-flash`, `deepseek-v4-flash-0731`, `vanchin/deepseek-v3.2-think`, `siliconflow/deepseek-v3.1-terminus`, `deepseek-r1-distill-qwen-1.5b`, `vanchin/deepseek-ocr`, plus `*-by-vanchin` / `*-by-siliconflow` variants |
| Kimi (Moonshot) | `kimi-k3`, `kimi-k3.1-pro`, `kimi-k3.1-pro-highspeed`, `kimi-k2.7-code`, `kimi-k2.6`, `kimi-k2.5`, `kimi/kimi-k3`, `kimi/kimi-k2.8-preview`, … |
| Zhipu GLM | `glm-5.3`, `glm-5.3-prime`, `glm-5.2`, `glm-5.1`, `glm-5`, `glm-5.2-fast-preview`, `ZHIPU/GLM-5.3-Flash`, `ZHIPU/GLM-5.3-FlashX`, … |
| MiniMax | `MiniMax-M3.1-Flash`, `MiniMax-M3`, `MiniMax-M2.7`, `MiniMax-M2.5`, `MiniMax/MiniMax-M2.1`, `MiniMax-H3` (video) |
| Xiaomi | `xiaomi/mimo-v2.6-pro-ultraspeed`, `xiaomi/mimo-v2.5-pro` |
| StepFun | `stepfun/step-5-preview`, `stepfun/step-3.7-flash` |
| Unisound | `unisound/unisound-u2` |

### 2.2 Vision understanding (image / video → text)

| Model | Context | Input | Notes |
|---|---|---|---|
| `qwen3.8-max` / `qwen3.8-flash` | 1M | text, image, video | 16M px/image, up to 2 h / 2 GB video, 2048 URL images |
| `qwen3.7-plus` / `-flash` | 1M | text, image, video | Same media limits |
| `qwen3.6-plus` / `-flash` | 1M | text, image, video | |
| `qwen3.5-plus` / `-flash` | 1M | text, image, video | |
| `qwen3-vl-plus` | 256k | text, image, video | 1 h video |
| `qwen3.5-omni-plus` / `-flash` | 256k | text, image, **audio**, video | Outputs text **and** audio |
| `qwen3-omni-flash` | 64k | text, image, audio, video | |
| `qwen3.5-ocr` / `qwen-vl-ocr` | 38k | text, image | OCR / document & table extraction |
| `qwen2.5-vl-72b/32b-instruct` | 128k | text, image, video | |

### 2.3 Image generation & editing

| Model | Capabilities | Max output | Max resolution |
|---|---|---|---|
| `qwen-image-3.0-pro` | text→image + edit, negative prompts, 6 variants | 6 | 2048×2048 |
| `qwen-image-3.0` | faster version of 3.0-pro | 6 | 2048×2048 |
| `qwen-image-2.1-pro` / `-turbo` | 2.1 series; up to 10 reference images, transparent output | 6 | 2048×2048 |
| `qwen-image-2.0-pro` / `qwen-image-2.0` | | 6 | 2048×2048 |
| `wan2.7-image-pro` | 4K, brand colours, up to 9 inputs, sequential mode | 4 (12 seq.) | 4096×4096 |
| `wan2.7-image` | up to 2K | 4 (12 seq.) | 2048×2048 |
| `z-image-turbo` | fast/cheap text→image, no edit | 1 | 2048×2048 |
| `vidu/vidu-image_*` | Vidu ref/image generation | 1 | 4K |
| `wanx-*` (v1, v2.0, v2.1), `wan2.6-image`, `wan2.5-i2i/t2i-preview`, `wan2.2-t2i-*` | legacy Wan family | 4 | ~1440×1440 |
| `qwen-mt-image` / `qwen-mt-image-2.0` | image translation | — | — |
| `wanx-sketch-to-image-lite`, `wanx-style-repaint-v1`, `wanx-x-painting`, `image-instance-segmentation`, `image-erase-completion`, `image-out-painting`, `virtualmodel-v2`, `shoemodel-v1`, `facechain-facedetect`, `ai-tryon*` | image tools / verticals | — | — |
| `kling/kling-v3-image-generation`, `kling-v3-omni-image-generation` | Kling image | — | — |

### 2.4 Video generation & editing

| Model | Capability | Max resolution / duration |
|---|---|---|
| `wan3.0-video` / `wan3.0-video-prime` | all-in-one: text→video, first/first-last frame, reference, audio, 2–30 s | 480P/720P/1080P, 30 fps |
| `happyhorse-1.1-t2v` / `-i2v` / `-r2v` | text→video / first-frame / reference, with audio, 3–15 s | up to 1080P |
| `happyhorse-1.0-t2v` / `-i2v` / `-r2v` / `-video-edit` | previous generation | 720P/1080P |
| `wan2.7-t2v` / `-i2v` / `-r2v` / `-videoedit` | text/first-last/reference/editing | 720P/1080P, 2–15 s |
| `wan2.6-t2v` / `-i2v` / `-i2v-flash` / `-r2v` / `-r2v-flash` | | 720P/1080P |
| `wan2.5-t2v-preview` / `-i2v-preview` | 5/10 s clips | up to 1080P |
| `wan2.2-t2v-plus` / `-i2v-plus` / `-i2v-flash` / `-kf2v-flash` / `-animate-move` / `-animate-mix` | incl. character animation / swap | 720P/1080P |
| `wanx2.1-t2v-plus` / `-turbo` / `-i2v-*` / `-kf2v-plus` / `-vace-plus` | legacy Wan 2.1 | 720P |
| `wan2.2-s2v` / `wan2.2-s2v-detect` | digital human (image + audio → video) | — |
| `video-style-transform`, `videoretalk` | style transfer / lip-sync | — |
| `kling/kling-v3-*` | Kling video | — | — |
| `pixverse/pixverse-*` (v6, v5.6, c1; t2v/i2v/kf2v/r2v/lipsync/motioncontrol/upscale) | PixVerse family | — | — |
| `vidu/viduq3-*`, `viduq2-*` | Vidu family (text/img/ref/start-end → video) | — | — |
| `MiniMax/MiniMax-H3` | MiniMax video | — | — |

### 2.5 3D generation

`Tripo/Tripo-H3.1`, `Tripo-P1.0`, `Tripo-ImageToSplat`, `Tripo-Texture`,
`Tripo-Segmentation`, `Tripo-SmartSegmentation`, `Tripo-MeshCompletion`,
`Tripo-Retopology`, `Tripo-ConvertFormat`, `Tripo-RigCheck`,
`Tripo-AutoRig`, `Tripo-AnimationRetarget` (async task API).

### 2.6 World model

`happyoyster-1.0-adventure` — interactive "world exploration" model.

### 2.7 Speech synthesis (TTS)

| Series | Models | Notes |
|---|---|---|
| Qwen-Audio-TTS | `qwen-audio-3.1-tts-plus`, `qwen-audio-3.1-tts-flash`, `qwen-audio-3.0-tts-plus`, `qwen-audio-3.0-tts-flash` | System/cloned/designed voices, instruction control, streaming |
| Qwen3-TTS | `qwen3-tts-flash`, `qwen3-tts-flash-realtime` | Low-latency streaming, system voices only |
| CosyVoice | `cosyvoice-v3.5-plus`, `cosyvoice-v3.5-flash`, `cosyvoice-v3-plus`, `cosyvoice-v3-flash`, `cosyvoice-v2` | Rich voice library, cloning/design; WebSocket / AOQ |
| Sambert | `Sambert语音合成` | WebSocket realtime, word/phoneme timestamps |
| MiniMax | `MiniMax/speech-2.8-hd`, `-turbo`, `speech-02-hd`, `-turbo` | Mixed voices, emotion styles, cloning |
| Voice clone/design | `voice-enrollment`, `qwen-voice-enrollment` | Create custom timbres |

### 2.8 Audio generation

`qwen-audio-3.1-tts-next` — full-scene sound generation (voices, effects,
ambient). (Music generation is separate: `Fun-Music`.)

### 2.9 Speech recognition (ASR)

| Model | Mode | API | Notes |
|---|---|---|---|
| `qwen-audio-3.1-asr-flash-message` | realtime | WebSocket | Short utterances / IME, hotwords + context |
| `qwen-audio-3.1-asr-flash-streaming` | realtime | WebSocket | Continuous transcription, hotwords, Prompt context, unlimited length |
| `qwen-audio-3.1-asr-flash-filetrans` | offline | HTTP | Up to 12 h / 2 GB, speaker diarization |
| `qwen-audio-3.1-asr-flash` | offline | HTTP | Up to 5 min / 2 GB, diarization |
| `qwen-audio-3.0-asr-flash-*` | realtime / offline | WebSocket / HTTP | Previous generation |
| `qwen3-asr-flash` / `-filetrans` / `-realtime` | offline / realtime | HTTP / WebSocket | Emotion recognition |
| `fun-asr`, `fun-asr-flash-2026-06-15`, `fun-asr-realtime`, `fun-asr-flash-8k-realtime` | realtime / offline | WebSocket / HTTP | Fun-ASR family |
| `paraformer-8k-v2`, `paraformer-realtime-8k-v2`, `paraformer-realtime-v2`, `paraformer-v2` | realtime / offline | WebSocket / HTTP | Paraformer family |
| `speech-biasing` | — | — | Custom hotwords |

### 2.10 Speech-to-speech / realtime dialogue (S2S)

End-to-end "speech in → speech out" (no separate ASR+LLM+TTS):

| Model | Use case | API |
|---|---|---|
| `qwen3.8-omni-flash-realtime` | realtime audio/video dialogue, function calling, web search | Realtime API (AOQ/WebRTC/WebSocket) |
| `qwen3.5-omni-plus-realtime` / `qwen3.5-omni-flash-realtime` | realtime dialogue | Realtime API |
| `qwen-audio-3.1-realtime-plus` / `-flash` | voice assistant / customer service | Realtime API |
| `qwen-audio-3.0-realtime-plus` / `-flash` | previous generation | Realtime API |
| `qwen3.8-livetranslate-flash-realtime` / `qwen3.5-livetranslate-flash-realtime` | simultaneous interpretation (2.3 s latency) | Realtime API |
| `qwen3-livetranslate-flash` | file dubbing / podcast translation | Chat Completions (HTTP) |

**Qwen-Audio Realtime voices (flash models):** `longanqian_v3.1` (default),
`longanhuan_v3.1`, `longanlingxin_v3.1`, `longanfengyue_v3.1`,
`xunanchuan_v3.1` — Chinese-first; set `session.output_audio.language = "en"`
for English output. There is **no `Jennifer`** on the Realtime models (that
name belongs to the HTTP `qwen3-tts-flash` voice list).

### 2.11 Omni / full-modal

`qwen3.8-omni-flash`, `qwen3.8-omni-flash-realtime`,
`qwen3.5-omni-plus`, `qwen3.5-omni-flash`,
`qwen3.5-omni-plus-realtime`, `qwen3.5-omni-flash-realtime`,
`qwen3-omni-flash`, `qwen3-omni-flash-realtime-2025-09-15`.

### 2.12 Embeddings & reranking

| Type | Models |
|---|---|
| Text embedding | `qwen3.7-text-embedding`, `qwen3.7-text-embedding-flash`, `text-embedding-v4` (64–2048 dims), `text-embedding-v3`, `text-embedding-v2`, `text-embedding-v1`, `text-embedding-async-v1/v2` |
| Multimodal embedding | `qwen3-vl-embedding`, `qwen2.5-vl-embedding`, `tongyi-embedding-vision-plus`, `tongyi-embedding-vision-flash` |
| Reranking | `qwen3.7-text-rerank`, `qwen3-rerank`, `qwen3-vl-rerank`, `gte-rerank-v2` |

---

## 3. Realtime API transports compared

From `platform.qianwenai.com/docs/api-reference/realtime-api/overview`:

| Dimension | AOQ (AI over QUIC) | WebRTC | WebSocket |
|---|---|---|---|
| Best for | AI multimodal realtime, weak network, mixed data | Browser interaction, traditional AV calls | Server integration, quick prototyping |
| Browser support | No | Native | Native |
| Integration effort | Low (SDK) | Medium | Very low |
| Weak-network resilience | Best | Good | Poor |
| Data types | AV + text | AV + text | text / audio / image |
| Connect speed | Fast | Slow | Slow |
| AEC / noise suppression | Built-in | Built-in | None (client must handle) |
| Client platforms | Android / iOS / HarmonyOS | Browser, mobile | Any WebSocket environment |
| Token Plan | Yes | Yes | Yes |

**Model support matrix (summary):**

| Type | Models | AOQ | WebRTC | WebSocket |
|---|---|---|---|---|
| Realtime omni | `qwen3.8-omni-flash-realtime`, `qwen3.5-omni-plus-realtime`, `qwen3.5-omni-flash-realtime` | ✓ | ✓ | ✓ |
| Realtime translation | `qwen3.8-livetranslate-flash-realtime`, `qwen3.5-livetranslate-flash-realtime` | ✓ | ✓ | ✓ |
| Realtime ASR | `qwen-audio-3.1-asr-flash-message`, `-streaming`, `qwen-audio-3.0-asr-flash-streaming`, Fun-ASR-Realtime | ✓ | — | ✓ |
| Realtime TTS | `qwen-audio-3.1-tts-plus/flash`, `qwen-audio-3.0-tts-*`, CosyVoice | ✓ | — | ✓ |
| Realtime S2S | `qwen-audio-3.1-realtime-plus/flash`, `qwen-audio-3.0-realtime-*` | ✓ | ✓ | ✓ |
| Multimodal dialog kit | `multimodal-dialog` | ✓ | ✓ | ✓ |

**WebRTC-supported models (explicit list).** WebRTC works with 10 realtime
models:

- **Realtime omni:** `qwen3.8-omni-flash-realtime`,
  `qwen3.5-omni-plus-realtime`, `qwen3.5-omni-flash-realtime`
- **Realtime speech dialogue (S2S):** `qwen-audio-3.1-realtime-plus`,
  `qwen-audio-3.1-realtime-flash`, `qwen-audio-3.0-realtime-plus`,
  `qwen-audio-3.0-realtime-flash`
- **Realtime speech translation:** `qwen3.8-livetranslate-flash-realtime`,
  `qwen3.5-livetranslate-flash-realtime`
- **Multimodal dialog kit:** `multimodal-dialog` (dev kit, not a single model)

**Not available over WebRTC** (AOQ or WebSocket only):

- **Realtime ASR:** `qwen-audio-3.1-asr-flash-message`,
  `qwen-audio-3.1-asr-flash-streaming`, `qwen-audio-3.0-asr-flash-streaming`,
  Fun-ASR-Realtime series
- **Realtime TTS:** `qwen-audio-3.1-tts-plus`, `qwen-audio-3.1-tts-flash`,
  `qwen-audio-3.0-tts-flash`, `qwen-audio-3.0-tts-plus`, CosyVoice series

WebRTC access is a server-side SDP exchange:
`POST https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=<model>`
with `Content-Type: application/sdp` and the Offer SDP as the body (returns the
Answer SDP); auth via `Authorization: Bearer <key>`. Media travels over RTP,
model events over a DataChannel, with built-in AEC/noise suppression and
`server_vad`/`semantic_vad` turn detection. See
`developer-guides/tutorials/realtime/webrtc-omni-realtime`.

### 3.3 WebSocket endpoints & protocol styles

From `platform.qianwenai.com/docs/api-reference/realtime-api/websocket-overview`.

**Endpoints** (`wss://maas.qianwenaiapi.com`):

| Model family | API path | Model name passed in |
|---|---|---|
| Qwen-Omni-Realtime | `/api-ws/v1/realtime` | URL query param `model` |
| Qwen3-TTS-Realtime | `/api-ws/v1/realtime` | URL query param `model` |
| Qwen-ASR-Realtime | `/api-ws/v1/realtime` | URL query param `model` |
| Qwen-Audio-Realtime | `/api-ws/v1/realtime` | URL query param `model` |
| Qwen-LiveTranslate-Realtime | `/api-ws/v1/realtime` | URL query param `model` |
| Qwen-Audio-TTS / CosyVoice | `/api-ws/v1/inference` | `run-task` → `payload.model` |
| Qwen-Audio-ASR / Fun-ASR / Paraformer | `/api-ws/v1/inference` | `run-task` → `payload.model` |

**Realtime (session) protocol** — `/api-ws/v1/realtime`:

1. Connect with model in URL + `Authorization` header; server sends `session.created`.
2. Send `session.update` (audio format, output modalities, voice, VAD); wait for `session.updated`.
3. Send input: Base64 audio via `input_audio_buffer.append`; text via `input_text_buffer.append`; images via `input_image_buffer.append`.
4. Trigger: VAD mode auto-triggers; manual mode = `input_audio_buffer.commit` then `response.create`.
5. End: models with finish events use `session.finish` → `session.finished`; otherwise close the socket. `response.done` ends one response, not the connection.

Example session config for `qwen3.8-omni-flash-realtime`:

```json
{
  "type": "session.update",
  "session": {
    "modalities": ["text", "audio"],
    "voice": "Tina",
    "turn_detection": { "type": "server_vad" }
  }
}
```

**Inference (task) protocol** — `/api-ws/v1/inference`:

1. Connect; send `run-task` with `payload.model`; wait for `task-started`.
2. ASR: stream binary audio frames → results via `result-generated`. TTS: send text via `continue-task` → receive binary audio frames.
3. Send `finish-task`; wait for `task-finished`. All three messages must share `header.task_id`.

**Model-specific differences:**

| Model | Trigger | End | Main output events |
|---|---|---|---|
| Qwen3.8/3.5-Omni | VAD auto; or commit + `response.create` | close after results | `response.text.delta`, `response.audio_transcript.delta`, `response.audio.delta`, `response.done` |
| Qwen3-TTS-Realtime | `server_commit` auto; or `input_text_buffer.commit` (no `response.create`) | `session.finish` | `response.audio.delta`, `response.done` |
| Qwen-ASR-Realtime | VAD auto; or commit | `session.finish` | `conversation.item.input_audio_transcription.text` / `.completed` |
| Qwen-Audio-Realtime | auto turn; or commit + `response.create` | close after results | `response.audio_transcript.delta`, `response.audio.delta`, `response.done` |
| Qwen3.8-LiveTranslate | `output_modalities` + `audio.input.turn_detection` | `session.finish` | `response.text.delta`, `response.audio_transcript.delta`, `response.audio.delta`, `response.done` |
| Qwen3.5-LiveTranslate | `modalities` + `turn_detection`; manual = commit (no `response.create`) | `session.finish` | `response.text.text`, `response.audio_transcript.text`, `response.audio.delta`, `response.done` |

**Errors:** handshake failure → check HTTP status (401/403 = auth), endpoint, region, workspace, model permissions. Realtime errors arrive as `error` events; inference errors as `task-failed` with `header.error_code` / `header.error_message`.

### 3.4 Realtime token authentication

From `platform.qianwenai.com/docs/api-reference/realtime-api/token-auth`.
All three protocols authenticate at **connect time only**, via
`Authorization: Bearer $DASHSCOPE_API_KEY`:

| Protocol | When | How |
|---|---|---|
| AOQ | AppServer → gateway | API key stays server-side; client gets a temporary token |
| WebRTC | SDP exchange HTTP request | header on the POST |
| WebSocket | connection handshake | header on the WS upgrade |

**AOQ flow (server-side proxy):**

```bash
curl -X POST \
  "https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=qwen3.8-omni-flash-realtime" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $DASHSCOPE_API_KEY" \
  -H "x-dashscope-rtc-transport: moq" \
  -d "{\"clientIp\": \"${CLIENT_REAL_IP}\"}"
```

Response: `sid`, `aoqTokenForClient`, `clientRelayEndpoints[]` (endpoint + port + route_index), `clientRelayCertFingerprint`, `sidExpiresInSecs`. The client SDK connects with token + sid + relay endpoints; publish/subscribe tracks are audio + data. `x-dashscope-rtc-transport: moq` selects AOQ.

### 3.5 QUIC & why AOQ is built on it

QUIC (RFC 9000, IETF 2021) is a transport protocol over UDP — the basis of HTTP/3. It exists to fix TCP's limits:

| TCP problem | QUIC fix |
|---|---|
| Head-of-line blocking (one lost packet stalls everything) | Independent **streams**; loss on one doesn't block others |
| TCP+TLS = 2–3 RTT handshake | Integrated TLS 1.3 → **1-RTT**, **0-RTT** resumption for returning clients |
| Connection bound to IP+port (4-tuple); network change kills it | **Connection IDs** → seamless **connection migration** (Wi-Fi ↔ cellular) |
| Ossified (kernel + middleboxes) | User-space over UDP, encrypted, evolvable |
| Coarse loss detection (RTO-based) | ACK-frame based detection (RFC 9002), pluggable congestion control (CUBIC/BBR) |

Mechanics in brief: everything is encrypted (incl. ACKs); reliability via never-reused packet numbers, ACK ranges, retransmission, credit-based flow control; `DATAGRAM` frames (RFC 9221) add unreliable unordered delivery for realtime media; migration is validated with `PATH_CHALLENGE`/`PATH_RESPONSE`.

**Why AOQ uses QUIC** (per Alibaba's AOQ docs / `developer.aliyun.com/article/1750325`):

- 0-RTT/1-RTT → fast call setup
- Streams + DATAGRAM → audio/video/text/events on separate tracks with different reliability needs ("实时 + 非实时" dual-mode adaptive)
- Stream-level fault tolerance → one track's loss doesn't stall the others
- Connection migration → network switch mid-call is transparent
- Better congestion control → advertised 60% packet-loss resilience
- Built-in TLS

AOQ SDK is declarative: set desired state, SDK handles transitions; auto-recovers from recoverable errors, reports only physical limits (network/device) or invalid tokens.

### 3.6 AOQ (QUIC) supported models

From the official protocol-support matrix (`realtime-api/overview` → 模型与协议支持):

| Category | Models | AOQ |
|---|---|---|
| Realtime Omni | `qwen3.8-omni-flash-realtime`, `qwen3.5-omni-plus-realtime`, `qwen3.5-omni-flash-realtime` | ✓ |
| Realtime TTS | `qwen-audio-3.1-tts-plus`, `qwen-audio-3.1-tts-flash`, `qwen-audio-3.0-tts-flash`, `qwen-audio-3.0-tts-plus` | ✓ |
| CosyVoice | CosyVoice series (docs example: `cosyvoice-v3-plus`) | ✓ |
| Realtime ASR | `qwen-audio-3.1-asr-flash-message`, `qwen-audio-3.1-asr-flash-streaming`, `qwen-audio-3.0-asr-flash-streaming`, Fun-ASR-Realtime series | ✓ |
| Realtime S2S | `qwen-audio-3.1-realtime-plus`, `qwen-audio-3.1-realtime-flash`, `qwen-audio-3.0-realtime-plus`, `qwen-audio-3.0-realtime-flash` | ✓ |
| Realtime translation | `qwen3.8-livetranslate-flash-realtime`, `qwen3.5-livetranslate-flash-realtime`, `qwen3.5-livetranslate-flash-realtime-2026-05-19` | ✓ |
| Multimodal dev kit | `multimodal-dialog` | ✓ |

**WebSocket-only (no AOQ):** `qwen3-tts-flash-realtime` (Qwen3-TTS-Realtime),
`qwen-asr-realtime`, Paraformer, Sambert.

**Two AOQ connection paths:**

| Model family | Endpoint |
|---|---|
| Realtime (Omni, S2S, LiveTranslate) | `POST /api/v1/webrtc/realtime?model=<model>` |
| Inference (TTS, ASR) | `POST /api/v1/webrtc/inference?model=<model>` |

Both use `x-dashscope-rtc-transport: moq` and return `sid` + `aoqTokenForClient`
+ relay endpoints. TTS/ASR use the inference (task) protocol on the Data track;
Omni/S2S/LiveTranslate use the session protocol.

### 3.7 WebRTC vs WebSocket: technical differences

| Dimension | WebSocket | WebRTC |
|---|---|---|
| Transport | TCP (reliable, ordered) | UDP (SRTP media, SCTP data) |
| Head-of-line blocking | Yes — one lost packet stalls the stream | No — late media packets are dropped |
| Media path | None — audio sent as Base64 over the socket | Dedicated RTP pipeline: jitter buffer, PLC, AEC |
| Echo cancellation / noise reduction | **Not built in** — client must handle | **Built in** |
| Data types | Text / audio / image | Audio / video + text |
| NAT traversal | Not needed (client→server TCP 443) | Built in (ICE / STUN / TURN) |
| Connection setup | HTTP upgrade + TCP + TLS | SDP exchange + ICE + DTLS-SRTP |
| Encryption | TLS (`wss://`) | DTLS-SRTP (mandatory) |
| Browser support | Native | Native |
| Weak-network resilience | Poor | Good |
| Integration effort | Very low | Medium |

**Qianwen connection details:**

- **WebSocket:** `wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=<model>`, auth in the handshake header; plain JSON events, audio as Base64 in `input_audio_buffer.append`; no media stack — client manages encoding, timing, echo cancellation.
- **WebRTC:** `POST https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=<model>` with `Content-Type: application/sdp` (Offer SDP → Answer SDP); media over RTP, model events over a DataChannel; built-in AEC/noise suppression and `server_vad`/`semantic_vad`.

**When to choose:** WebSocket for server-side integration, quick prototypes, constrained devices (ESP32-S3) — lowest effort but no AEC. WebRTC for browser apps / low-latency voice where you want the built-in media pipeline. AOQ for mobile native apps wanting the best weak-network resilience (proprietary SDK).

---

## 4. Pricing (examples; check the model pages for current numbers)

Prices are on `www.qianwenai.com/models/<id>`; the platform also sells a
**Token Plan** subscription (one key, all modalities). Examples seen on the
homepage:

| Model | Input | Output | Context |
|---|---|---|---|
| `qwen3.8-flash` | ¥0.8 / M tokens | ¥2.7 / M tokens | 1M |
| DeepSeek (V4.1 Flash) | ¥1 / M | ¥4 / M | 1M |
| Kimi K3.1 Pro | ¥26 / M | ¥136 / M | 1M |
| `wan3.0-video` | ¥0.45–1.8 / second | — | — |

New users get free quota across 400+ models. Batch APIs are up to 50% cheaper
than realtime.

---

## 5. Relevance to the Smart Ring Plus (ESP32-S3)

- **OpenAI-compatible endpoint is the easy win.** `espressif/openai` (see
  `notes/networking.md` §5.1) or a hand-rolled `esp_http_client` call can use
  `https://maas.qianwenaiapi.com/compatible-mode/v1` as the base URL and a
  Qianwen API key. `OpenAIChangeBaseURL()` exists for exactly this.
- **Voice / realtime.** The platform's WebSocket realtime path
  (`wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=qwen3.8-omni-flash-realtime`)
  maps onto `esp_websocket_client`; WebRTC maps onto `esp_peer`. The session
  protocol (`session.update` → `input_audio_buffer.append` → `response.*` events)
  is plain JSON over the socket, so it fits `esp_websocket_client` cleanly; the
  inference protocol (`run-task`) is the ASR/TTS path. Note the WebSocket path
  has **no built-in AEC** — the ring's speaker/mic echo problem (§5.2 of
  `notes/networking.md`) still needs a client-side AEC. AOQ/WebRTC have AEC
  built in, but AOQ needs a proprietary SDK (Android/iOS/HarmonyOS), so on
  ESP32-S3 the practical choices are WebSocket + own AEC, or WebRTC.
- **ASR / TTS.** `qwen-audio-3.1-asr-flash-streaming` (WebSocket) and
  `qwen-audio-3.1-tts-flash` (WebSocket / HTTP) are a strong Whisper/TTS
  alternative; the ring's ES8311 codec gives PCM in/out via `ring_audio_*`.
- **Region / latency.** `maas.qianwenaiapi.com` is the China gateway; for
  devices outside China, check the international DashScope endpoints
  (`dashscope-intl.aliyuncs.com`) and the Qianwen platform's own region
  support. TLS + cert bundle is required (see `notes/networking.md` §3).
- **No camera** on the ring, so the video/3D/image-generation models are not
  usable for capture, only for cloud-side generation from other inputs.

---

## 6. Key links

- Platform console: `https://platform.qianwenai.com`
- Docs index (full): `https://platform.qianwenai.com/docs/llms.txt`
- Model list (docs): `https://platform.qianwenai.com/docs/models/model-list`
- Model market (specs/pricing): `https://www.qianwenai.com/models`
- Model selection guide: `https://platform.qianwenai.com/docs/developer-guides/getting-started/model-selection`
- OpenAI-compatible Chat API: `https://platform.qianwenai.com/docs/api-reference/chat/openai-chat`
- Anthropic Messages API: `https://platform.qianwenai.com/docs/api-reference/chat/anthropic`
- DashScope API: `https://platform.qianwenai.com/docs/api-reference/chat/dashscope`
- Realtime API overview: `https://platform.qianwenai.com/docs/api-reference/realtime-api/overview`
- Realtime WebSocket: `https://platform.qianwenai.com/docs/api-reference/realtime-api/websocket-overview`
- Realtime WebRTC: `https://platform.qianwenai.com/docs/api-reference/realtime-api/webrtc-access`
- Realtime AOQ: `https://platform.qianwenai.com/docs/api-reference/realtime-api/aoq-access`
- AOQ SDK intro: `https://platform.qianwenai.com/docs/api-reference/realtime-api/aoq-sdk-intro`
- Qwen-Omni-Realtime WebSocket access: `https://platform.qianwenai.com/docs/api-reference/real-time-multimodal/websocket-access`
- Token/auth: `https://platform.qianwenai.com/docs/api-reference/realtime-api/token-auth`
- API key: `https://platform.qianwenai.com/docs/api-reference/preparation/api-key`
