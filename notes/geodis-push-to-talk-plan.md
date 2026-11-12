# Plan — `14_geodis_push_to_talk` (GEODIS voice agent, push-to-talk)

**Status:** proposal, not started.
**Inputs:** `notes/geodis-ai-api.md` (the API survey + the 2026-11-10 verification
update), `scripts/test_geodis_api.py` (the host spike), `examples/07`/`10` (the
existing WebRTC and PTT examples), `components/ringkit/` (the existing
`ring_net`/`ring_ws` transports).
**Target app:** `examples/14_geodis_push_to_talk` (next free number after `13`).
**New ringkit module:** `ring_geodis` (WebRTC + Azure OpenAI Realtime).

---

## 1. Goal

A push-to-talk example that holds a button, streams the ring's microphone to the
**GEODIS Voice Agent** (Azure OpenAI Realtime `gpt-realtime-2.1` over WebRTC),
plays the spoken reply, and shows the user/assistant transcripts. It must
execute the client-side RAG tool call (`rechercher_base_geodis`) so factual
questions get answered instead of hanging.

Non-goals: hands-free mode (a later phase), transcript persistence to
`/api/v1/save-message` (easy follow-up), the DEV/intranet endpoint, production
auth.

---

## 2. What the GEODIS transport requires (and how it differs from `ring_net`)

The note's §5 ("in the order the ring's `ring_geodis` module meets them") is the
authoritative checklist. The deltas from the existing Qianwen `ring_net`:

| Aspect | `ring_net` (Qianwen) | `ring_geodis` (GEODIS / Azure Realtime) |
|---|---|---|
| Signalling | one HTTPS POST to `maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=…` with `Bearer sk-…` | **two steps**: `POST {base}/token` → `ek_…` token, then `POST {azure_sdp}` with `Bearer ek_…` |
| Hosts | one | **two** (GEODIS gateway + `geodisopenai.openai.azure.com`) |
| Trust anchor | pinned `assets/qianwen_ca.pem` (server omits its intermediate) | public certs → Mozilla bundle (spike; fall back to an embedded chain) |
| Credentials | API key in NVS | **none** — the UAT gateway is unauthenticated |
| `session.update` | `modalities`, `voice`, top-level `turn_detection` | `audio.input.transcription`, `audio.output.voice`; **never** top-level `turn_detection` / `modalities` (rejected as `unknown_parameter`) |
| STT | on by default | **off by default** — opt in with `{"model":"whisper-1"}` |
| Assistant transcript event | `response.audio_transcript.done` / `.delta` | **`response.output_audio_transcript.done` / `.delta`** |
| User transcript event | `conversation.item.input_audio_transcription.completed` | same |
| Tool calls | none | **client-executed**: `response.function_call_arguments.done` → `POST /tool-call` → `conversation.item.create` (`function_call_output`) → `response.create` |
| Turn-taking | server VAD only | server VAD by default; **PTT** via `audio.input.turn_detection: null` + `input_audio_buffer.commit` + `response.create` (spike) |
| Media | Opus/48 kHz/stereo over RTP | same (the API-level 24 kHz PCM is inside the Opus RTP; the ring stays at 48 kHz) |

Everything else — `esp_peer`, the Opus codec, the 48 kHz mono PCM boundary, the
downlink byte ring, the state machine, the PSRAM audio task — is identical to
`07`. The new module is an adaptation, not a new stack.

---

## 3. Architecture decision

**Recommendation: a standalone `ring_geodis` module, modelled on `ring_net`, with
a shared WebRTC core extracted as a follow-up.**

Why not reuse `ring_net` directly:

- The signalling, CA, auth, session protocol and turn-taking are all different;
  `ring_net`'s API (`ring_net_start(model, api_key)`) cannot express a two-step
  token flow or a tool call.
- `ring_net` is the only WebRTC consumer and `07` works; rewriting it risks a
  working example for no immediate gain.

Why not a full shared core now:

- `ring_net.c` owns `vApplicationStackOverflowHook`; a second definition would
  be a duplicate symbol. A shared core means moving that hook and the Opus/peer
  plumbing out of `ring_net` — a refactor of a working module.
- The incremental image cost of a second module is small: `esp_peer` and
  `esp_audio_codec` are already in `ringkit`'s unconditional `PRIV_REQUIRES`, so
  only the new `.c` text is added (~40–60 KB). `07` is 2640 KB in a 3000K
  partition, so there is headroom.

Follow-up (optional, after `14` works): extract `ring_rtc_core` (peer open, Opus
codec, downlink ring, state machine) and make `ring_net` / `ring_geodis` thin
protocol adapters. Do this only if a third transport appears or the image
pressure demands it.

---

## 4. `ring_geodis` module design

### 4.1 Files

```
components/ringkit/
├── include/ringkit/ring_geodis.h          # public API
├── src/ring_geodis_priv.h                 # shared context (like ring_net_priv.h)
├── src/ring_geodis.c                      # public API, state, event task
├── src/ring_geodis_signaling.c            # /token + SDP exchange, peer bring-up
├── src/ring_geodis_codec.c                # Opus encode/decode (adapted from ring_net_codec.c)
└── src/ring_geodis_protocol.c             # DataChannel JSON, session, tool calls
```

Add a `CONFIG_RINGKIT_GEODIS` gate (default `y`, consistent with the other four)
in `components/ringkit/Kconfig`, and the sources under that gate in
`components/ringkit/CMakeLists.txt`. Add `ring_geodis.h` to `ringkit.h`.

**Move `vApplicationStackOverflowHook` out of `ring_net.c`** into a shared file
(`ring_board.c` or a new `ring_stack_hook.c`) before adding the second module —
otherwise the link fails with a duplicate symbol.

### 4.2 Public API

```c
typedef enum {
    RING_GEODIS_OFF = 0,
    RING_GEODIS_CONNECTING,   /* Wi-Fi / SNTP / token / SDP / DTLS / session */
    RING_GEODIS_READY,        /* session configured, uplink closed */
    RING_GEODIS_LISTENING,    /* uplink open (button held) */
    RING_GEODIS_SPEAKING,     /* reply arriving */
    RING_GEODIS_ERROR,
} ring_geodis_state_t;

typedef struct {
    const char *base_url;      /* GEODIS gateway, e.g. ".../corp/voice-agent/v1" */
    const char *sdp_url;       /* Azure OpenAI ".../openai/v1/realtime/calls" */
    const char *voice;         /* "coral" */
    const char *system_prompt; /* optional; sent in the /token body */
    bool        enable_stt;    /* default true: whisper-1 opt-in */
    bool        enable_ptt;    /* default true: turn_detection=null; false = server VAD */
} ring_geodis_cfg_t;

typedef void (*ring_geodis_state_cb_t)(ring_geodis_state_t state, void *user);
typedef void (*ring_geodis_transcript_cb_t)(const char *text, size_t len,
                                            bool is_user, void *user);

/* Tool-call hook. Return true if the app handled the call (it must send
 * function_call_output + response.create itself); return false to use the
 * built-in RAG execution. May be NULL. */
typedef bool (*ring_geodis_tool_cb_t)(const char *name, const char *args_json,
                                      void *user);

esp_err_t ring_geodis_init(ring_geodis_state_cb_t state_cb,
                           ring_geodis_transcript_cb_t text_cb,
                           ring_geodis_tool_cb_t tool_cb, void *user);
esp_err_t ring_geodis_start(const ring_geodis_cfg_t *cfg);
esp_err_t ring_geodis_stop(void);
ring_geodis_state_t ring_geodis_state(void);
const char *ring_geodis_state_name(ring_geodis_state_t state);

esp_err_t ring_geodis_set_listening(bool on);
esp_err_t ring_geodis_send_audio(const int16_t *pcm48k, size_t samples);
esp_err_t ring_geodis_read_audio(int16_t *pcm48k, size_t samples,
                                 size_t *out, uint32_t timeout_ms);
esp_err_t ring_geodis_commit(void);   /* PTT: input_audio_buffer.commit + response.create */
esp_err_t ring_geodis_cancel(void);   /* response.cancel (barge-in) */
esp_err_t ring_geodis_send_event(const char *json);
esp_err_t ring_geodis_keepalive(void); /* optional; session lifetime is undocumented */
```

`RING_GEODIS_FRAME_SAMPLES` is 960 (20 ms at 48 kHz), matching `ring_net`.

### 4.3 Signalling (`ring_geodis_signaling.c`)

1. **Wi-Fi + SNTP** — reuse `ring_wifi_start()` / `ring_wifi_wait_connected()`
   and `ring_time_sync()` (TLS needs the wall clock).
2. **Token** — `POST {base_url}/token` with
   `{"chat_session_id":"ring-XXXXXXXX"}` (+ `system_prompt` when set). Parse
   `token` (`ek_…`) and `session_id`. Store both in the context. The token is
   short-lived: fetch it immediately before the SDP POST, never cache it across
   sessions.
3. **Peer open** — `esp_peer_open()` with `ESP_PEER_AUDIO_CODEC_OPUS`, 48 kHz,
   stereo (same as `ring_net_peer_open()`).
4. **SDP exchange** — `POST {sdp_url}` with `Authorization: Bearer <ek_>`,
   `Content-Type: application/sdp`, body = the local offer (CRLF-normalised).
   Expect **HTTP 201** with the answer SDP as the body (not 200). Install it via
   `esp_peer_send_msg()`.
5. Wait for `session.created` → send `session.update` → wait for `session.updated`
   → `RING_GEODIS_READY`.

**TLS:** `ring_http_init()` currently requires a pinned PEM. Add a variant that
uses `esp_crt_bundle_attach` (e.g. `ring_http_init_bundle()`), or extend
`ring_http_init()` with a `use_bundle` flag. The spike decides whether the bundle
validates both hosts; if not, embed the Azure/GEODIS chain as
`assets/geodis_ca.pem` and add a `ring_geodis_ca_pem()` accessor.

### 4.4 Session protocol (`ring_geodis_protocol.c`)

On `session.created`:

```json
{"type":"session.update","session":{
  "audio":{
    "input":{"transcription":{"model":"whisper-1"},
             "turn_detection":null},
    "output":{"voice":"coral"}
  }}}
```

- `turn_detection: null` is the PTT opt-out; **spike first** (§6). If the GA
  endpoint rejects it, drop the field and use the server-VAD fallback (§4.7).
- Never send `modalities` or top-level `turn_detection`.

Event handling:

| Event | Action |
|---|---|
| `session.updated` | state → `READY` |
| `conversation.item.input_audio_transcription.completed` | `text_cb(..., is_user=true)` |
| `response.output_audio_transcript.delta` | `text_cb(..., is_user=false)` (streaming) |
| `response.output_audio_transcript.done` | `text_cb(..., is_user=false)` (final) |
| `response.function_call_arguments.done` | queue a tool-call job (§4.5) |
| `response.done` | `speaking=false`; state → `READY` |
| `error` | log; surface via state/error callback |

Replies go back on the SCTP stream the event arrived on (same `send_stream_id`
pattern as `ring_net`).

### 4.5 Client-executed tool call

On `response.function_call_arguments.done`:

1. Copy `call_id`, `name`, `arguments` (a JSON **string**).
2. `POST {base_url}/tool-call` with
   `{"name":<name>,"arguments":<arguments string>,"session_id":<session_id>}`.
   `arguments` must stay a string — an object is HTTP 500.
3. The response is a JSON-RPC wrapper whose `result.result.content[0].text` is
   itself a JSON string (double-escaped). Parse twice to get
   `{"retrievedChunks":[…]}`.
4. Send `conversation.item.create` with
   `{"type":"function_call_output","call_id":<call_id>,"output":"<json string>"}`.
5. Send `response.create`.

**This HTTPS POST must not run on the peer task.** Queue the call to a
dedicated `ring-geodis-tool` task (8–16 KB stack, TLS needs it) or the app's
connect task. The module exposes a `tool_cb` so the app can intercept (e.g. to
show "looking up…" or to override the RAG call); the default is the built-in
execution above. `name` is ignored by the backend — always
`rechercher_base_geodis`.

### 4.6 Audio (`ring_geodis_codec.c`)

Copy `ring_net_codec.c` with the module renamed:

- Encoder: Opus 48 kHz, **stereo** (esp_peer offers `opus/48000/2`), 32 kbps,
  20 ms frames, FEC on, complexity 0. `ring_geodis_send_audio()` up-mixes the
  mono API to stereo.
- Decoder: Opus 48 kHz stereo → fold to mono → push onto the PSRAM downlink ring
  (4 s, 384 KB).
- The decoder runs on the peer task under a mutex; the encoder on the app's
  audio task. The audio task therefore needs a **PSRAM stack > 96 KB** (see
  `07`'s `qw-turn`).

### 4.7 Turn-taking

**Primary (PTT):** `set_listening(true)` → stream mic frames → on release
`set_listening(false)` + `commit()`. `commit()` sends
`input_audio_buffer.commit` then `response.create`. This is the WebSocket PTT
flow from `10`, adapted to WebRTC.

**Fallback (server VAD):** if `turn_detection: null` is rejected, keep the
default server VAD. Hold-to-talk still works, but the model may auto-respond
after ~500 ms of silence mid-hold. On release, send a short silence tail (as
`07` does) and let the VAD commit; only send `commit()` if no response has
started. The example should log which mode it is in.

### 4.8 Tasks

| Task | Stack | RAM | Owner |
|---|---|---|---|
| `ring-geodis-signal` | 4 KB | internal | token + SDP exchange |
| `ring-geodis-peer` | 16 KB | internal | `esp_peer` main loop + DTLS |
| `ring-geodis-event` | 6 KB | internal | state + transcript callbacks |
| `ring-geodis-tool` | 12 KB | internal | `/tool-call` HTTPS (off the peer task) |

The app owns the audio task (PSRAM, 128–256 KB), like `07`/`10`.

---

## 5. Example design — `examples/14_geodis_push_to_talk`

### 5.1 Files

```
examples/14_geodis_push_to_talk/
├── CMakeLists.txt          # RING_EXAMPLE_SRCS/INCLUDE_DIRS/REQUIRES
├── include/ring_example.h  # identical to 06's
├── geodis_push_to_talk.c
└── README.md               # design/spec
```

### 5.2 UI (model on `06`)

- Title `GEODIS` (top).
- Big round **HOLD TO TALK** button (centre, radius ~70 px, inside
  `RING_SAFE_RADIUS`).
- Stage line: `connecting…` / `ready — hold to talk` / `listening…` /
  `looking up…` / `waiting…` / `speaking…` / `error: …`.
- User transcript line (orange).
- Assistant reply line (white).
- Mic level bar (bottom).
- All LVGL work under `RING_UI_LOCKED(-1)`; all geometry via
  `ring_ui_place_centered()` / `RING_CX`/`RING_CY`.

### 5.3 Flow

1. `ring_nvs_init()`; read optional endpoint overrides from NVS
   (`ring_secret_get("geodis", "base_url", GEODIS_BASE_URL, …)` and
   `"sdp_url"`), else the compile-time UAT defaults.
2. `ring_geodis_init(state_cb, text_cb, tool_cb, NULL)`.
3. **Connect task** (8 KB internal): Wi-Fi → SNTP → `ring_geodis_start(&cfg)`.
   Retry with a 5 s backoff; show each step.
4. **PTT worker** (PSRAM stack, `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`):
   - On press: `ring_geodis_set_listening(true)`, spawn the worker.
   - Loop: `ring_audio_read()` stereo 20 ms → `ring_audio_stereo_to_mono()` →
     `ring_geodis_send_audio(mono, 960)`; update the level bar.
   - On release: `ring_geodis_set_listening(false)`; `ring_geodis_commit()`.
   - Play the reply: `ring_geodis_read_audio()` → mono → stereo →
     `ring_audio_set_mute(false)` → `ring_audio_write()` → mute. Use the
     `10` pattern (unmute once, grace timer before the first frame, drain on
     `response.done` / state `READY`).
5. Tool call: the module auto-executes; the `tool_cb` (or a state hook) shows
   `looking up…` and the chunk count.
6. Barge-in: a tap while speaking calls `ring_geodis_cancel()` (optional).

### 5.4 Config / secrets

- No API key (the gateway is unauthenticated). Do **not** add a fake key.
- Defaults in `ring_geodis.h`:
  `GEODIS_BASE_URL = "https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1"`,
  `GEODIS_SDP_URL  = "https://geodisopenai.openai.azure.com/openai/v1/realtime/calls"`.
- Optional NVS override via `ring_secret_get()` so the DEV endpoint can be
  targeted without a rebuild.
- `chat_session_id`: fresh per session, e.g. `ring-` + 8 hex digits from
  `ring_rand_u32()` (seeded from `esp_timer_get_time()`).

---

## 6. Phase 0 — host spike (do this first, no firmware)

Extend `scripts/test_geodis_api.py` to settle the four unknowns before writing
any C. The spike is host-only (aiortc + stdlib) and does not touch firmware.

### 6.1 Task list

- [x] **T0.1 — `--mode ptt`.** A WebRTC session that opts out of server VAD
  (`audio.input.turn_detection = null`), streams a clip as the held uplink,
  then sends `input_audio_buffer.commit` + `response.create`, and verifies the
  reply, the transcript and the tool call. If the update is rejected, the
  timeout path reports the `error` event.
- [x] **T0.2 — SDP codec parse.** Parse the answer SDP (`m=audio` + `a=rtpmap`)
  and assert the negotiated audio codec is **Opus** in every WebRTC mode.
- [x] **T0.3 — `--mode tls`.** Verify both hosts (GEODIS gateway + Azure SDP)
  against the Mozilla bundle (`certifi`, falling back to the system store) and
  print the leaf subject/issuer.
- [x] **T0.4 — `--mode token`.** Fetch an `ek_` token, probe it with a bogus
  offer (expect 400), wait `--token-wait`, probe again to see whether it is
  still accepted.
- [x] **T0.5 — run the spike** (`--mode http`, `--mode tls`, `--mode ptt`) and
  record the results in §6.3.
- [x] **T0.6 — document** the new modes in `scripts/README.md`.

### 6.2 What the spike must answer

1. **PTT turn detection.** Does `session.update` with
   `audio.input.turn_detection = null` get echoed by `session.updated` (no
   `unknown_parameter`)? Then: does a clip + `commit` + `response.create`
   produce a reply? If rejected, confirm the server-VAD fallback (stream,
   silence tail, response arrives without an explicit commit).
2. **SDP media codec.** Does `m=audio` negotiate **Opus** (expect
   `a=rtpmap:… opus/48000/2`)? This decides whether the `ring_net` Opus
   pipeline is reusable as-is.
3. **TLS.** Do both hosts validate with the Mozilla bundle? If not, capture
   the chain to `assets/geodis_ca.pem`.
4. **Token/session lifetime.** How long does an `ek_` token live? Does an
   idle session need a keepalive?

Also confirm the exact `session.update` shape the GA endpoint accepts
(`audio.input.transcription`, `audio.output.voice`, `audio.input.turn_detection`)
and that `instructions` belongs in the `/token` body, not `session.update`.

### 6.3 Results (2026-07-30)

All four questions are settled. `--mode ptt` and `--mode all` were run against
the live UAT endpoint; `--mode tls` / `--mode token` are host-only.

| Question | Answer |
|---|---|
| `turn_detection: null` accepted? | **Yes.** `session.updated` echoes `turn_detection: null`; no `unknown_parameter`. |
| PTT clip → commit → reply? | **Yes.** 10/10 checks: STT transcript, assistant reply, RAG tool call (5 chunks), spoken reply saved. |
| SDP audio codec | **Opus.** `m=audio pt=96 rtpmap=opus/48000/2` — the `ring_net` Opus pipeline is reusable as-is. |
| TLS both hosts (Mozilla bundle) | **Yes.** GEODIS leaf ← GTS `WE1` ← `GTS Root R4` ← GlobalSign Root CA; Azure leaf `*.cognitiveservices.azure.com` ← Microsoft TLS G2 RSA CA OCSP 02. Both validate with `certifi` (Mozilla) and the system store. |
| Token lifetime | **> 120 s.** A bogus offer is still accepted (HTTP 400) after 120 s. The module fetches a fresh token per session, so this is sufficient. |

**Operational finding — flaky gateway.** The GEODIS gateway intermittently
resets TLS connections (~1 in 12 requests from the bench;
`SSL: UNEXPECTED_EOF_WHILE_READING`). The Azure SDP host was stable in the
same probe (0/12). `http_request()`, `get_token()` and the SDP POST now retry
transport errors (HTTPError is never retried). **The firmware `ring_geodis`
must retry the `/token` and SDP POSTs** — a single attempt will occasionally
fail.

#### 6.3.1 Raw spike output

`--mode ptt --no-stun` (10/10):

```
WebRTC session (PTT)
  PASS  POST /token  — session_id=40d5cc53-…
  PASS  SDP exchange -> HTTP 201
  PASS  SDP answer negotiates Opus  — m=audio pt=96 rtpmap=opus/48000/2
  PASS  session.created received (STT off by default, server_vad preset)
  PASS  session.updated received (PTT: turn_detection=null)  — turn_detection=None errors=[]
  PTT: streaming clip as held uplink (6.7 s) …
  PTT: release -> input_audio_buffer.commit + response.create
  PASS  STT transcript of the spoken question  — “Hello? Where is my shipment? …”
  PASS  assistant reply transcript  — “I can’t see any shipment-specific tracking details …”
  PASS  RAG tool call executed by the client  — fn_calls=1 tool-call HTTP 200 chunks=5
  PASS  no session errors
  PASS  spoken reply saved  — scripts/geodis_reply.wav (4020558 B)
10/10 checks passed, 0 failed
```

`--mode all --no-stun` (19/19): HTTP surface 8/8, TLS 2/2, WebRTC text turn
9/9 (including the Opus check and the client-executed tool call).

`--mode tls` (2/2):

```
PASS  TLS connect-weu-uat.api.geodis.com  — TLSv1.3 TLS_AES_256_GCM_SHA384
      leaf=connect-weu-uat.api.geodis.com issuer=WE1 [certifi (Mozilla)]
PASS  TLS geodisopenai.openai.azure.com  — TLSv1.3 TLS_AES_256_GCM_SHA384
      leaf=*.cognitiveservices.azure.com issuer=Microsoft TLS G2 RSA CA OCSP 02
      [certifi (Mozilla)]
```

`--mode token --token-wait 120` (3/3): token accepted immediately and after
120 s (bogus offer → HTTP 400).

**Exit criteria:** a deterministic PTT turn (clip → commit → reply, with a tool
call) passes end-to-end, and the SDP/TLS answers are known.

---

## 6A. Phase 1 — `ring_geodis` module (implemented)

### 6A.1 Files

```
components/ringkit/
├── include/ringkit/ring_geodis.h      # public API + state enum + config struct
├── src/ring_geodis_priv.h             # shared context, tool-job type, task sizes
├── src/ring_geodis.c                  # public API, state machine, event task
├── src/ring_geodis_signaling.c        # /token + SDP exchange, peer bring-up, HTTPS retry
├── src/ring_geodis_codec.c            # Opus encode/decode, downlink ring
├── src/ring_geodis_protocol.c         # DataChannel JSON, session.update, tool task
└── src/ring_stack_hook.c              # shared vApplicationStackOverflowHook
```

Supporting changes:

- `ringkit/Kconfig`: new `CONFIG_RINGKIT_GEODIS` (default `y`).
- `ringkit/CMakeLists.txt`: the four `ring_geodis_*.c` files under the gate;
  `ring_stack_hook.c` in the always-compiled list.
- `ringkit/ringkit.h`: includes `ring_geodis.h`.
- `ring_http`: new `ring_http_init_bundle()` (Mozilla root bundle via
  `esp_crt_bundle_attach`) for the Azure/GEODIS public certs.
- `ring_net.c`: `vApplicationStackOverflowHook` moved to `ring_stack_hook.c`
  (a second definition would be a duplicate symbol).
- `examples/12_ringkit_selftest`: init/stop `ring_geodis`.

### 6A.2 Design notes

- **Signalling** — `ring_geodis_start()`: Wi-Fi → SNTP → `POST /token`
  (retry ×3) → `esp_peer_open` → `esp_peer_new_connection` → the signal task
  POSTs the SDP offer to the Azure endpoint with `Bearer ek_…` → install the
  answer → wait for `session.created` → `session.update` → `session.updated`
  → READY.
- **Session schema** — `audio.input.transcription = {"model":"whisper-1"}`
  and `audio.input.turn_detection = null` (PTT), `audio.output.voice`. Never
  send top-level `turn_detection` / `modalities`.
- **Tool call** — on `response.function_call_arguments.done` the peer task
  queues a job; `ring-geodis-tool` (12 KB) POSTs `/tool-call` (arguments as a
  JSON string), extracts the double-escaped `text`, and sends
  `function_call_output` + `response.create`. Scratch buffers (32 KB + 16 KB)
  are PSRAM, allocated once.
- **Audio** — identical to `ring_net`: 48 kHz stereo mic → mono → Opus → RTP;
  RTP → Opus → mono → PSRAM ring. The app's audio task needs a PSRAM stack
  (> 96 KB).
- **Transcript events** — `response.output_audio_transcript.done` / `.delta`
  (model) and `conversation.item.input_audio_transcription.completed` (user).
- **PTT** — `set_listening(true)` → stream → `set_listening(false)` +
  `commit()` (`input_audio_buffer.commit` + `response.create`).
- **Retries** — `/token`, `/tool-call` and the SDP POST retry transport errors
  (the gateway resets ~1 in 12 TLS connections); 4xx is never retried.

### 6A.3 Build status (2026-07-30)

| App | Image | Free | Notes |
|---|---|---|---|
| `12_ringkit_selftest` | 2708 KB | 292 KB | exercises `ring_geodis_init`/`stop` |
| `07_qianwen_realtime_harness` | 2604 KB | 396 KB | `ring_net` consumer, still builds |
| `10_qianwen_ws_voice_chat` | 1592 KB | 1560 KB | `ring_ws` consumer, still builds |
| `00_glyph_grid_reveal` | 736 KB | — | unchanged |
| `09_hello_smoke_test` | 804 KB | — | unchanged (module dropped by `--gc-sections`) |

Vendored overrides still win (`espressif__esp_codec_dev` 300 refs,
`viewesmart__smartring_plus` 66 refs, all from `components/`). Host tests:
`make check` → 1425 checks, 0 failures.

### 6A.4 Hardware blocker

**On-device verification is blocked by a pre-existing touch-controller
failure.** `ring_board_init()` aborts in `bsp_display_init()` with
`CST816S: I2C read failed` / `ESP_ERR_INVALID_RESPONSE` before any app code
runs. The known-good `09_hello_smoke_test` fails identically, so it is not
caused by the new module. The CST816S needs a **physical power cycle**
(unplug/replug USB); a software reset via the USB-JTAG RTS pin does not clear
it. Phase 1's firmware verification (boot log + `ring_geodis_init`/`stop`)
is pending that.

### 6A.5 Review fixes (2026-11-12, after `b7e0065`)

`notes/geodis-code-review.md` read the module against how example 14 will use
it. The P0/P1 findings are fixed; the module still has never run on hardware,
so the fixes are compile-verified only (`12_ringkit_selftest` links and builds,
host tests 1480 checks / 0 failures).

Contract changes an app author must know:

- **Callbacks moved off the peer task.** `state_cb`/`text_cb` run on
  `ring-geodis-event`, `tool_cb` on `ring-geodis-tool`. Previously all three
  ran on the 16 KB peer task that also runs DTLS, which contradicted the
  header's "may take the LVGL lock".
- **The event queue carries transitions, not a state cell.** `READY →
  LISTENING → SPEAKING` is three callbacks; the old 1-deep overwrite queue
  delivered only the last one.
- **`text_cb` gained `bool final`** (same shape as `ring_ws`): deltas append,
  a final replaces the line.
- **`ring_geodis_cfg_t` defaults are explicit**: `NULL` = documented default,
  `enable_stt`/`enable_ptt` are tri-state (`-1` = on). Use
  `RING_GEODIS_CFG_DEFAULT()`; a zero-initialised struct still means "off".
  `ring_geodis_start()` logs the effective config.
- **The uplink is a separate flag from the state.** `send_audio` no longer
  stops working when the model starts answering mid-hold, and `set_listening
  (true)` cancels + flushes, so pressing while the reply plays is a real
  barge-in instead of the ring talking into its own microphone.
- **Peer lifetime has one owner.** `stop()`/retry ask `ring-geodis-peer` to
  close and wait (`RING_GEODIS_PEER_CLOSE_TIMEOUT_MS`), instead of freeing the
  DTLS state from the caller's task while the main loop is inside it.
- **Signalling is generation-stamped.** `start()` bumps `session_gen`; a slow
  token+SDP round trip whose session was replaced is dropped instead of
  installing an answer for a dead peer. The SDP POST retries twice
  (`RING_GEODIS_SDP_ATTEMPTS`) so its worst case fits inside
  `RING_GEODIS_SESSION_TIMEOUT_MS` (25 s) — previously start() timed out while
  the signal task kept going.
- **Tool-call failures are honest.** Unreadable/truncated `arguments` no
  longer degrade to `{}` (a confident answer to an empty query); the model
  gets `lookup_unavailable` with a reason, which it can say out loud. Truncated
  tool output is detected (`ring_json_get_str_ex`) instead of being sent as
  broken JSON.

`ring_net` still has the peer-lifetime and callback-task problems; it was left
alone deliberately (07 works, and the fix is best made once in a shared
`ring_rtc_core`). See the review's §7 for the recommended order.

---

## 7. Phases

| Phase | Deliverable | Done when |
|---|---|---|
| **0** ✅ | `scripts/test_geodis_api.py --mode ptt` | PTT + codec + TLS confirmed — **done 2026-07-30**, 10/10 |
| **1** ✅ | `ring_geodis` module in `ringkit` | builds; `12_ringkit_selftest` can init/stop it — **built 2026-07-30**, hardware check pending |
| **2** | `examples/14_geodis_push_to_talk` | builds; boots to `ready` on a UDP-capable network |
| **3** | Docs + verification | README, app table, ringkit map, boot log, human voice check |

Phase 0 is complete: `turn_detection: null` is accepted, the PTT clip → commit →
reply flow works end-to-end (with the client-executed RAG call), the SDP
negotiates Opus/48000/2, and both hosts validate against the Mozilla bundle.
Phase 1 is unblocked; the module is mostly a mechanical adaptation of
`ring_net`.

---

## 8. Risks / open questions

1. **UDP/DTLS blocked on the bench LAN.** `07` cannot complete DTLS there; GEODIS
   is the same WebRTC transport. On-device verification needs a network with
   outbound UDP (a phone hotspot is the usual workaround). This is the biggest
   practical risk — the example may be unverifiable on the bench.
2. **Flaky GEODIS gateway.** The gateway resets ~1 in 12 TLS connections from
   the bench. `ring_geodis` must retry the `/token` and `/tool-call` POSTs and
   the SDP POST on transport errors (the host spike now does).
3. **PTT — resolved.** `audio.input.turn_detection = null` is accepted and
   echoed by `session.updated`; the clip → commit → reply flow works. No
   fallback needed, but keep the server-VAD path in mind if the endpoint
   changes.
4. **TLS — resolved.** Both hosts validate against the Mozilla bundle
   (`certifi`): GEODIS ← GTS WE1 ← GTS Root R4 ← GlobalSign Root CA; Azure ←
   Microsoft TLS G2 RSA CA OCSP 02. `ring_http` still needs a bundle-capable
   init (`esp_crt_bundle_attach`); no embedded CA is required unless the ESP
   bundle is later trimmed.
5. **Token/session lifetime.** The `ek_` token is valid for > 120 s; the module
   fetches a fresh one per session, so this is sufficient. The idle session
   lifetime is undocumented (`idle_timeout_ms: null`); a keepalive is prudent
   but not required for the example.
5. **Image size.** `07` is 2640 KB / 3000K. The new module adds ~40–60 KB when
   `CONFIG_RINGKIT_GEODIS=y`. Measure `idf.py -B build/14_geodis_push_to_talk
   size`; if it is too close, consider `default n` + a per-app Kconfig mechanism
   or the shared-core extraction.
6. **Unauthenticated, metered endpoint.** Every session costs GEODIS's Azure
   budget. No looping tests; no automatic retries that start new sessions.
7. **Console back-pressure.** Drain `/dev/ttyACM0` continuously; a capture script
   that sleeps before reading will stall the firmware and mimic a hang.
8. **Duplicate `vApplicationStackOverflowHook`.** Move it to a shared file
   before adding the second WebRTC module.
9. **Two HTTPS hosts.** The module needs two HTTP clients with different trust
   anchors; `ring_http` currently assumes one pinned CA.
10. **Tool-call latency.** The RAG POST blocks the reply; surface a `looking up…`
    state so the user knows it is not hung.

---

## 9. Verification checklist

- **Host spike:** `scripts/.venv/bin/python scripts/test_geodis_api.py --mode ptt`
  passes (commit → reply, tool call, transcript).
- **Builds from clean dirs:** `00_glyph_grid_reveal`, `09_hello_smoke_test`,
  `07_qianwen_realtime_harness`, `10_qianwen_ws_voice_chat`,
  `14_geodis_push_to_talk`.
- **Vendored overrides still win:** `espressif__esp_codec_dev` and
  `viewesmart__smartring_plus` (see `AGENTS.md`).
- **Boot log:** `ringkit:` summary all `ok`, `app: 14_geodis_push_to_talk`,
  `session ready`, no `panic` / `assert` / `Guru Meditation`.
- **On-device (needs a human + UDP-capable network):** hold the button, speak,
  release, hear the reply, see both transcripts. The log proves the pipeline
  runs, not that the conversation is intelligible.
- **Unit tests:** `cd tests && make check` if any pure-logic helper changes
  (unlikely; the module is transport-bound).

---

## 10. Docs to update

- `examples/README.md` — add `14_geodis_push_to_talk` to the app table.
- `components/ringkit/README.md` — module map (`ring_geodis`), task table,
  state machine, and the voice transport decision guide (GEODIS = Azure
  Realtime over WebRTC, client-executed tools).
- `AGENTS.md` — architecture tree (new module + example), app table, and the
  `vApplicationStackOverflowHook` move.
- `scripts/README.md` — document `--mode ptt`.
- `notes/geodis-ai-api.md` — link this plan and record the Phase 0 results.
