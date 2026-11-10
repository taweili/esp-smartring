# 14_geodis_voice_chat — conversation with the GEODIS Voice Agent

> **Status: plan / design only.** No buildable app yet. This document is the
> implementation plan; it follows the same shape as
> `examples/11_quic_voice_chat/README.md`.

A ring application that holds a **speech-to-speech conversation with the
GEODIS Voice Agent** — the UAT deployment described in
`notes/geodis-ai-api.md` (Azure OpenAI Realtime `gpt-realtime-2.1` over
WebRTC, RAG via an MCP/MongoDB knowledge base).

```
board init → Wi-Fi → SNTP → HTTPS probe → POST /token → SDP exchange
            → WebRTC session → [ mic → mono → Opus → GEODIS → Opus → speaker ]
            → function_call → POST /tool-call → function_call_output → response.create
```

The transferable part is the **voice backend contract**, not the GEODIS
content: a Realtime model, server VAD, client-executed tool calls, and a
24 kHz mono PCM contract (Opus/48000/2 on the wire).

---

## 1. Goal and non-goals

**Goal.** A new ringkit module `ring_geodis` plus an example app that:

1. Gets an ephemeral token from the GEODIS UAT backend.
2. Brings up a WebRTC session to Azure OpenAI Realtime (`gpt-realtime-2.1`).
3. Streams mic audio up and plays the assistant's audio back, with turn-taking.
4. Turns on STT so the user's utterance is transcribed.
5. Executes the `rechercher_base_geodis` RAG tool call client-side and writes
   the result back into the session (otherwise the agent hangs forever).
6. Shows state, transcripts and tool activity on the round screen.

**Non-goals.** Production authentication, real consignment data, acoustic
echo cancellation, a GEODIS-approved product. The UAT endpoint is
unauthenticated and metered — this is a demo/probe, not a shippable feature.

---

## 2. Backend facts (from `notes/geodis-ai-api.md` + live probes)

### 2.1 Endpoints

| Method | URL | Purpose |
|---|---|---|
| `POST` | `https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1/token` | Ephemeral WebRTC token. Body optional: `system_prompt`, `chat_session_id`, `existing_session_id`, `user_token`. **No auth.** |
| `POST` | `https://geodisopenai.openai.azure.com/openai/v1/realtime/calls` | SDP exchange. `Authorization: Bearer <ek_…>`, `Content-Type: application/sdp` → **201** + answer SDP. |
| `POST` | `…/v1/tool-call` | Execute the RAG tool. `{name, arguments (JSON **string**), session_id}`. |
| `POST` | `…/v1/api/v1/save-message` | Persist one transcript message (optional). |
| `GET` | `…/v1/api/v1/transcripts/{chat_session_id}` | Fetch a transcript (optional). |
| `GET` | `…/v1/api/v1/info` | Agent metadata / health. |

### 2.2 Session behaviour

- Model `gpt-realtime-2.1`, `output_modalities: ["audio"]` (audio only).
- Audio is **PCM 24 kHz mono S16** logically; on the wire it is **Opus** in
  RTP (`a=rtpmap:opus/48000/2`). The SDP must contain an audio m-line even for
  text input (Azure rejects an offer with no audio with HTTP 400).
- **STT is OFF by default.** Opt in with
  `session.update → session.audio.input.transcription = {"model":"whisper-1"}`.
  The wrong shape (`session.input_audio_transcription`) is rejected with
  `unknown_parameter`.
- Server VAD: threshold 0.5, prefix 300 ms, silence 500 ms,
  `create_response: true`, `interrupt_response: true`.
- **Tool calls are not executed by the backend.** On
  `response.function_call_arguments.done` the client must `POST /tool-call`,
  then send `conversation.item.create` (`function_call_output`) and
  `response.create`. Skipping this hangs the turn (measured 90 s+).
- `/tool-call` quirks: `arguments` must be a JSON **string** (object → HTTP
  500); `name` is ignored; the response is a JSON-RPC wrapper whose `text` is
  itself a JSON string (double-escaped → parse twice).
- The server does **not** persist the transcript; the client must call
  `/api/v1/save-message` per bubble.
- The RAG has no consignment data — tracking scenarios need injected data.

### 2.3 Live probe (2026-11-10, this machine)

- `POST /token` with `{}` still returns a usable `ek_…` token — **no auth**.
- `GET /openai/v1/realtime?model=…` with WebSocket upgrade headers returns
  **404 from the Azure APIM** (`apim-request-id` present). The WebSocket
  transport is **not exposed** by the GEODIS gateway; **WebRTC is the only
  path**.
- `POST /openai/v1/realtime/calls` with a bogus SDP returns **400**
  (`unsupported_content_type` / invalid SDP), confirming the route exists.
- Cert chains are public and Mozilla-bundle-compatible:
  - `connect-weu-uat.api.geodis.com` → Google Trust Services `WE1` →
    GlobalSign Root CA.
  - `geodisopenai.openai.azure.com` → Microsoft TLS RSA Root G2 →
    DigiCert Global Root G2.
  - Both roots are in the ESP-IDF Mozilla bundle, so
    `esp_crt_bundle_attach` should validate them (verify on device).

---

## 3. Architecture decision: WebRTC via `esp_peer`

The GEODIS gateway only exposes the WebRTC SDP route, so the transport must be
WebRTC. This is the same transport `ring_net` already uses for Qianwen, so the
plan reuses the proven pattern rather than inventing one:

| Concern | Reuse |
|---|---|
| PeerConnection / ICE / DTLS / SCTP / RTP | `esp_peer` (already a `PRIV_REQUIRE`) |
| Opus encode/decode | `esp_audio_codec` + the `ring_net_codec.c` pattern |
| HTTPS (`/token`, `/tool-call`, `save-message`) | `ring_http`, `ring_json` |
| Downlink jitter/ring buffer | `ring_byte_ring` (PSRAM) |
| Wi-Fi / SNTP | `ring_wifi`, `ring_time` |
| Audio capture/playback | `ring_audio` (48 kHz stereo ↔ mono) |
| UI / lock / theme | `ring_ui`, `ring_theme` |

**Why not `ring_ws`?** The WebSocket transport is not reachable through the
GEODIS APIM (404), and the ephemeral `ek_` token is issued for the WebRTC
`/realtime/calls` route. If the gateway later exposes WebSocket, `ring_ws` is
the natural fallback (it already implements the OpenAI-Realtime event shape);
that would be a small adapter, not a new transport.

**Why not extend `ring_net`?** `ring_net` is Qianwen-specific: host, model,
event schema (`response.audio_transcript.delta`, `voice: longanqian_v3.1`,
`modalities: ["text","audio"]`). The Azure schema differs
(`response.output_audio_transcript.*`, `session.audio.input.transcription`,
audio-only output). A separate module keeps `07` stable and the two backends
independent. The shared Opus/peer scaffolding is factored out (see §4.2).

---

## 4. ringkit module: `ring_geodis`

### 4.1 Files

```
components/ringkit/include/ringkit/ring_geodis.h
components/ringkit/src/ring_geodis.c            # public API, state, tasks
components/ringkit/src/ring_geodis_signaling.c  # /token + SDP exchange
components/ringkit/src/ring_geodis_protocol.c   # DataChannel JSON (Azure events)
components/ringkit/src/ring_geodis_priv.h
```

Gated by `CONFIG_RINGKIT_GEODIS` (default `y`) in `components/ringkit/Kconfig`,
added to `RINGKIT_SRCS` in `components/ringkit/CMakeLists.txt`. No new managed
dependency: `esp_peer` and `esp_audio_codec` are already linked.

### 4.2 Shared Opus core (recommended refactor)

`ring_net_codec.c` is ~150 lines of Opus bring-up + encode/decode + downlink
push, coupled to `ring_net_ctx_t`. Extract a neutral helper:

```
components/ringkit/include/ringkit/ring_opus.h
components/ringkit/src/ring_opus.c
```

with a small struct (`ring_opus_t`) holding the encoder, decoder, scratch
buffers and the `dec_mutex`, plus:

```c
esp_err_t ring_opus_open(ring_opus_t *o, int sample_rate, int channels);
void      ring_opus_close(ring_opus_t *o);
/* encode one 20 ms mono frame -> Opus packet; 48 kHz clock */
esp_err_t ring_opus_encode(ring_opus_t *o, const int16_t *mono, size_t samples,
                           uint8_t *out, int out_cap, int *out_len);
/* decode one Opus packet -> mono PCM */
esp_err_t ring_opus_decode(ring_opus_t *o, const uint8_t *pkt, int pkt_len,
                           int16_t *mono, size_t max_samples, size_t *out_samples);
```

`ring_net` and `ring_geodis` both use it. This is a **safe, mechanical
refactor** of a working module: the `07` app is the regression test. If the
refactor is deferred, duplicate the ~150 lines with a comment pointing at
`ring_net_codec.c` — but do not leave two copies long-term.

### 4.3 Public API

```c
typedef enum {
    RING_GEODIS_OFF = 0,
    RING_GEODIS_CONNECTING,   /* Wi-Fi / SNTP / token / SDP / DTLS */
    RING_GEODIS_READY,        /* session configured, uplink closed */
    RING_GEODIS_LISTENING,    /* uplink open */
    RING_GEODIS_SPEAKING,     /* assistant reply arriving */
    RING_GEODIS_ERROR,
} ring_geodis_state_t;

typedef struct {
    const char *chat_session_id;   /* transcript binding; NULL = ephemeral   */
    const char *system_prompt;     /* persona override; NULL = GEODIS default */
    const char *voice;             /* "coral" (default), "martha", …         */
    bool        enable_stt;        /* default true: opt in to whisper-1     */
    const char *instructions;      /* session.instructions; NULL = none      */
} ring_geodis_cfg_t;

typedef void (*ring_geodis_state_cb_t)(ring_geodis_state_t state, void *user);

typedef void (*ring_geodis_transcript_cb_t)(const char *text, size_t len,
                                            bool is_user, bool final, void *user);

typedef void (*ring_geodis_tool_cb_t)(const char *name, const char *args_json,
                                      void *user);

esp_err_t ring_geodis_init(ring_geodis_state_cb_t state_cb,
                           ring_geodis_transcript_cb_t text_cb,
                           ring_geodis_tool_cb_t tool_cb, void *user);
esp_err_t ring_geodis_start(const ring_geodis_cfg_t *cfg);   /* blocks → READY */
esp_err_t ring_geodis_stop(void);
ring_geodis_state_t ring_geodis_state(void);
const char *ring_geodis_state_name(ring_geodis_state_t state);

esp_err_t ring_geodis_set_listening(bool on);
esp_err_t ring_geodis_send_audio(const int16_t *pcm48k_mono, size_t samples);
esp_err_t ring_geodis_read_audio(int16_t *pcm48k_mono, size_t samples,
                                 size_t *out_samples, uint32_t timeout_ms);
bool      ring_geodis_is_speaking(void);

esp_err_t ring_geodis_send_event(const char *json);
esp_err_t ring_geodis_response_create(void);
esp_err_t ring_geodis_cancel(void);

/* Optional: take over tool execution. If set, the module forwards the function
 * call to this callback instead of POSTing /tool-call itself. The callback
 * must send function_call_output + response.create (see §6). */
void ring_geodis_set_tool_handler(bool (*handler)(const char *name,
                                                  const char *args_json,
                                                  void *user),
                                  void *user);

/* Optional: persist a transcript bubble via /api/v1/save-message. */
esp_err_t ring_geodis_save_message(const char *role, const char *text);

const char *ring_geodis_ca_pem(void);   /* NULL: use the Mozilla bundle */
```

### 4.4 Tasks

| Task | Stack | Prio | Role |
|---|---|---|---|
| `geodis-signal` | 4 KB | 4 | `/token` + SDP POST, peer bring-up |
| `geodis-peer` | 16 KB | 5 | `esp_peer_main_loop` + DTLS |
| `geodis-event` | 6 KB | 4 | state / transcript / tool callbacks |
| app audio task | 256 KB | — | mic → Opus → uplink; downlink → speaker (PSRAM) |

Mirror `ring_net`'s task split and stack sizes (`ring_net_priv.h`). The Opus
encoder needs >96 KB of stack, so the app audio task must be created with
`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` (see `07`).

### 4.5 State machine

```
OFF ──start()──▶ CONNECTING ──session.updated──▶ READY ──set_listening(true)──▶ LISTENING
                        ▲                          │                              │
                        └──────────ERROR◀──────────┘                              │
                                     ▲                                    response.created
                                     │                                            │
                                     └──────response.done◀──────SPEAKING◀──────────┘
```

Half-duplex gate: while `SPEAKING`, `send_audio()` drops frames (the ring has
no AEC — see §7). `response.done` returns to `READY`.

---

## 5. Protocol mapping (Azure Realtime events)

### 5.1 Signalling

1. `POST /token` with `{"chat_session_id": "…"}` (and optionally
   `system_prompt`) → `{session_id, token}`. Store both.
2. `esp_peer_new_connection()` produces the local Offer (Opus/48000/2 +
   DataChannel). Normalise LF→CRLF (`ring_net_signaling.c` already does this).
3. `POST https://geodisopenai.openai.azure.com/openai/v1/realtime/calls`
   with `Authorization: Bearer <token>`, `Content-Type: application/sdp`,
   body = Offer → 201 + Answer SDP. Feed the Answer to `esp_peer_send_msg()`.
4. Wait for the DataChannel + `session.updated`.

### 5.2 DataChannel events

| Event | Action |
|---|---|
| `session.created` | Send `session.update` (see below). |
| `session.updated` | → `READY`. |
| `input_audio_buffer.speech_started` | If speaking: barge-in → `response.cancel()` + flush downlink. |
| `input_audio_buffer.speech_stopped` | Log only (server VAD handles the turn). |
| `conversation.item.input_audio_transcription.completed` | `text_cb(transcript, is_user=true, final=true)`. |
| `response.output_audio_transcript.delta` | `text_cb(delta, is_user=false, final=false)`. |
| `response.output_audio_transcript.done` | `text_cb(transcript, is_user=false, final=true)`. |
| `response.function_call_arguments.done` | Execute the tool (§6), write back, `response.create`. |
| `response.done` | → `READY`; clear `speaking`. |
| `error` | Log; `server_error` → `ERROR`. |

**Note the event names.** Azure uses `response.output_audio_transcript.*`;
Qianwen uses `response.audio_transcript.*`. This is the main place a
copy-paste from `ring_net_protocol.c` breaks.

### 5.3 `session.update`

```json
{
  "type": "session.update",
  "session": {
    "type": "realtime",
    "audio": {
      "input":  { "transcription": { "model": "whisper-1" } },
      "output": { "voice": "coral" }
    }
  }
}
```

- **Do not send `turn_detection`** — the GA endpoint rejects it with
  `unknown_parameter` (`Unknown parameter: 'session.turn_detection'`). The
  server configures server_vad itself, and the default config is visible in
  `session.created` under `audio.input.turn_detection` (threshold 0.5,
  prefix 300 ms, silence 500 ms, `idle_timeout_ms: null`,
  `create_response: true`, `interrupt_response: true`).
- `modalities` **cannot** be changed (audio-only output) — sending it is
  likewise rejected.
- `instructions` is accepted as a system-prompt override; `system_prompt` in
  the `/token` body is equivalent and set at session creation. Prefer
  `system_prompt` for a static persona, `instructions` for a runtime change.
- If `enable_stt` is false, omit `audio.input.transcription`.

### 5.4 Keepalive / session lifetime

The note does not document an Azure idle timeout (Qianwen WS closes after
180 s). Add a 60 s timer that re-sends `session.update` while `READY`, and log
if the session drops. Treat the exact timeout as an open question (§10).

---

## 6. Tool call execution

The module owns the Realtime `session_id` and the HTTPS plumbing, so it
executes the tool call by default — this is the trap that hangs the agent.

```
response.function_call_arguments.done
  → extract call_id, name, arguments (arguments is already a JSON string)
  → POST /tool-call  { name, arguments: <string>, session_id }
  → parse result.result.content[0].text   (double-escaped → json.loads twice)
  → conversation.item.create { item: { type: "function_call_output",
                                        call_id, output: <json string> } }
  → response.create
```

- Use a PSRAM buffer for the `/tool-call` response (RAG chunks can be large).
- If `ring_geodis_set_tool_handler()` is set, forward `(name, args_json)` to
  the app and let it write the output — this is how a demo injects consignment
  data the GEODIS RAG does not have (e.g. "Shanghai → Paris, in transit, ETA
  2026-11-05"). The handler must send `function_call_output` + `response.create`
  itself.
- Always send `response.create` after the output, even on tool error (send
  `{"error": "…"}` as the output) so the turn cannot hang.

---

## 7. Audio pipeline and turn-taking

`ring_audio` is 48 kHz / 16-bit / **stereo**; `ring_geodis` is 48 kHz / 16-bit
/**mono**; the wire is Opus/48000/2.

- **Uplink:** `ring_audio_read()` 20 ms stereo → fold L/R to mono →
  `ring_geodis_send_audio()` → `ring_opus_encode()` → RTP.
- **Downlink:** `ring_opus_decode()` → fold to mono → `ring_geodis_read_audio()`
  → expand to stereo → `ring_audio_set_mute(false)` → `ring_audio_write()` →
  `ring_audio_set_mute(true)` (mute avoids pops).
- **No resampling needed:** Opus runs on a 48 kHz clock; the 24 kHz logical
  format is inside the codec. `ring_resample` is not required for this path.

**Turn-taking.** The ring has no AEC (ES8311 has none, speaker next to mic), so
full-duplex is not safe. The only mode the endpoint offers is **server VAD**:
`turn_detection` is rejected in `session.update` (see §5.3), so PTT over
WebRTC is not available. Use the **half-duplex gate** from `07`:

1. Open the uplink; the server VAD detects the end of speech and
   auto-creates the response (`create_response: true`).
2. When the first reply frame arrives, flip to `SPEAKING` and drop uplink
   frames until `response.done` — the model never hears itself.
3. `interrupt_response: true` gives barge-in on the server side, but without
   AEC the local mic will hear the speaker, so the uplink gate is what keeps
   it clean.

If the gateway ever exposes WebSocket, `ring_ws` supports PTT and
`turn_detection: null`; that would be the path for a PTT variant.

---

## 8. Example app: `14_geodis_voice_chat`

Three files, following the example contract:

```
examples/14_geodis_voice_chat/
├── CMakeLists.txt          # RING_EXAMPLE_SRCS / _INCLUDE_DIRS / _REQUIRES
├── include/ring_example.h  # identical to the other examples
└── geodis_voice_chat.c     # ring_example_start() + ring_example_name()
```

**UI (round screen, `RING_SAFE_RADIUS`):**

- **Connect screen** — status line (`wi-fi…`, `network check…`, `token…`,
  `session…`), IP, retry with 5 s backoff.
- **Chat screen** — orb with a radial level ring (like `10`), the user's
  utterance and the assistant's reply as wrapped labels, a tool-call indicator
  (`RAG…`), and state text.
- **Talk control** — touch-and-hold (or BOOT button) for PTT; in half-duplex
  mode, the orb animates while listening/speaking.
- All LVGL work under `RING_UI_LOCKED(-1)`.

**App tasks:** one connect task (Wi-Fi + SNTP + probe + `ring_geodis_start`,
retries) and one audio task (PSRAM stack) for the mic/speaker loop — the same
layout as `07`.

**Network gate:** before the session, probe
`https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1/api/v1/info` with
the Mozilla bundle. If the probe fails, do not attempt the session.

---

## 9. Verification plan

### M0 — host spike (before any firmware)

Use the existing `hermes-ws/geodis-buddy/geodis_buddy_demo.py` (aiortc) or a
minimal script to:

1. Confirm `/token` + SDP + STT + TTS + tool call still work.
2. Capture the exact JSON of `session.created`, `session.updated`,
   `response.function_call_arguments.done`, `response.output_audio_transcript.done`
   and the `/tool-call` response — these drive the parser.
3. Test `turn_detection: null` over WebRTC (PTT feasibility).
4. Confirm the `/tool-call` `session_id` is the Realtime session id.

### M1–M5 — firmware

- Build with a per-app build dir:
  ```bash
  rm -rf build/14_geodis_voice_chat
  idf.py -DRING_APP=14_geodis_voice_chat -B build/14_geodis_voice_chat build
  ```
- Check `idf.py -B build/14_geodis_voice_chat size` (the app partition is
  3000K; `07`/`12` have ~360 KB headroom — adding ring_geodis should be well
  under that).
- Flash and read the boot log: `app: 14_geodis_voice_chat`, `ringkit:` summary
  all `ok`, `state -> ready`, no `panic`/`assert`/`Guru Meditation`.
- **Voice quality, turn-taking and barge-in need a human** — the log proves
  the pipeline runs, not that the conversation is intelligible.
- Verify the tool path by asking a RAG question (e.g. "geodisxx2026") and
  checking the log for `/tool-call` and the reply.
- If `ring_json`/`ring_resample`/`ring_byte_ring` were touched, run
  `cd tests && make check`.

---

## 10. Risks and open questions

| Risk | Mitigation |
|---|---|
| **UAT `/token` is unauthenticated** | No credentials to leak, but anyone can burn GEODIS's Azure budget. Do not ship; flag to GEODIS. Do not accept arbitrary user input as `system_prompt`. |
| **UDP blocked → DTLS fails** | Same as `07`; WebRTC is the only exposed transport, so there is no TCP fallback today. If the gateway adds WebSocket, adapt `ring_ws`. |
| **No AEC** | Half-duplex gate (or PTT if supported). Never open the uplink while the assistant speaks. |
| **Tool call not executed → hang** | Module executes it by default; always send `response.create` after the output. |
| **Wrong event names** | `response.output_audio_transcript.*`, not Qianwen's `response.audio_transcript.*`. |
| **Double-escaped `/tool-call` response** | Parse `result.result.content[0].text` twice. |
| **RAG has no consignment data** | App tool handler can inject data; otherwise the agent answers from marketing docs. |
| **Cert bundle** | Verify GTS/DigiCert roots are in the on-device Mozilla bundle with the `/api/v1/info` probe before the session. |
| **Session lifetime unknown** | `idle_timeout_ms` is `null` in the default VAD config; send a `session.update` keepalive every 60 s while `READY` and log drops. |
| **Build size** | Check `size`; ring_geodis adds ~30–50 KB of code. |

**Open questions for GEODIS / further probing:**

1. What are the legal values of `mcp_database` / `mcp_collection`? Can a demo
   point the RAG at its own consignment data?
2. Is `response.output_audio_transcript.done` guaranteed with audio-only
   output? (The note says yes.)
3. Will the gateway expose WebSocket in the future? (Would allow `ring_ws`
   and PTT.)

> Resolved by the M0 spike: `turn_detection` is **not** settable over WebRTC
> (`unknown_parameter`), so PTT is not available; the WebSocket route returns
> 404; `idle_timeout_ms` is `null`.

---

## 11. Milestones

| # | Milestone | Deliverable |
|---|---|---|
| M0 | Host spike | **Done.** `scripts/test_geodis_api.py` verifies the HTTP surface and a real WebRTC session (text + voice); event JSON captured in `notes/geodis-ai-api.md` §5. PTT ruled out (`turn_detection` rejected). |
| M1 | `ring_geodis` skeleton + signalling | `/token` + SDP + peer + Opus; `state -> ready` on device |
| M2 | DataChannel protocol | `session.update`, STT opt-in, transcripts, `response.done` |
| M3 | Tool call | `/tool-call` → `function_call_output` → `response.create` |
| M4 | Example app | UI, audio loop, turn-taking, retry |
| M5 | Extras | `save-message`, stats, keepalive, app tool handler |
| M6 | Docs + verification | README, build size, boot log, human voice check |

---

## 12. Repo touch-list (when implementing)

- `components/ringkit/Kconfig` — add `CONFIG_RINGKIT_GEODIS`.
- `components/ringkit/CMakeLists.txt` — add `ring_geodis*.c`.
- `components/ringkit/README.md` — module map + transport decision guide row.
- `examples/README.md`, `README.md` — app table row.
- `notes/geodis-ai-api.md` — link the implemented app; update the live-probe
  date if the endpoint behaviour changes.
- No change to `main/idf_component.yml` (no new dependency).
- No change to `sdkconfig.defaults` (WebRTC config already present).
