# Plan — `14_geodis_push_to_talk` (GEODIS voice agent, push-to-talk)

**Status (2026-11-21 19:30):** Phase 0 ✅ (host spike, 2026-07-30); Phase 1 ✅ built
(`ring_geodis` in `ringkit`, code-review fixes 2026-11-12; §5.6 diagnostics
`ring_geodis_stats_t`, `last_err`, `set_tool_done_cb` added 2026-11-20); Phase 1b ✅
**on hardware** — the ring enumerates and boots (§6A.4), CST816S included;
Phase 2a ✅ **verified on hardware** (mock sequence incl. barge-in, §5.5);
Phase 2b ✅ **the real session reaches `READY`** on the ring after three
on-hardware fixes (§6A.6: DTLS client role, `session.type`, comfort-noise) —
**a live user turn is the one remaining step**; real 2,691,792 B / 12% free,
mock 1,903,408 B / 38% free; Phase 3 docs **done**, boot + run logs recorded.
**Next: a human speaks a real turn** (hold → STT → tool → TTS). This note is a
plan + decision record — for the API itself see
`components/ringkit/include/ringkit/ring_geodis.h`.

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

Follow-up: extract `ring_rtc_core` (peer open, Opus codec, downlink ring, state
machine) and make `ring_net` / `ring_geodis` thin protocol adapters. The review's
§7 order puts the app first and this extraction **after**, once the semantics
have stopped moving (the module gained stats/tool callbacks on 2026-11-20); the
app should not be held up by the refactor, but `ring_net`'s latent peer-lifetime
and uplink-open bugs should be fixed in the shared core rather than copied into
`ring_geodis` again — see §5.6.

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
├── src/ring_geodis_protocol.c             # DataChannel JSON, session, tool calls
├── src/ring_geodis_payload.{c,h}          # pure request-body builders (host-tested)
├── src/ring_geodis_mock.{c,h}             # CONFIG_RINGKIT_GEODIS_MOCK transport
└── src/ring_stack_hook.c                  # shared vApplicationStackOverflowHook
```

Add a `CONFIG_RINGKIT_GEODIS` gate (default `y`, consistent with the other four)
in `components/ringkit/Kconfig`, and the sources under that gate in
`components/ringkit/CMakeLists.txt`. Add `ring_geodis.h` to `ringkit.h`.

**Move `vApplicationStackOverflowHook` out of `ring_net.c`** into a shared file
(`ring_board.c` or a new `ring_stack_hook.c`) before adding the second module —
otherwise the link fails with a duplicate symbol.

### 4.2 Public API

The authoritative contract is `components/ringkit/include/ringkit/ring_geodis.h`;
this note does not restate it (an earlier copy drifted). The decisions the header
cannot express:

- **States are a UI hint, not a gate.** `RING_GEODIS_SPEAKING` can overlap an
  open uplink — the model answers while the button is still held. The uplink is
  a separate `bool` owned by `ring_geodis_set_listening()`.
- **`ring_geodis_cfg_t` is tri-state.** Use `RING_GEODIS_CFG_DEFAULT()`; a
  zero-initialised struct means STT **off** and PTT **off**, which silently
  produces a conversation with no user transcript and a stray `commit()`.
- **Callbacks run on module tasks** (`state_cb`/`text_cb` on `ring-geodis-event`,
  `tool_cb` on `ring-geodis-tool`), never on the peer/DTLS task, so they may
  take the LVGL lock. `text_cb` carries `final` and `clipped`: append deltas,
  commit on `final`, and treat a clipped final as a prefix.
- **`keepalive()` does not extend the session** — the `ek_` token is spent at the
  SDP exchange. Real recovery is `stop()` + `start()`.
- **`ring_geodis_is_speaking()`** is the polled truth for playback/barge-in.

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

**TLS:** `ring_http_init()` requires a pinned PEM, so `ring_http` gained
`ring_http_init_bundle()` (`esp_crt_bundle_attach`). The spike confirmed both
hosts validate against the Mozilla bundle, so no embedded CA is needed; if the
ESP bundle is later trimmed, embed the Azure/GEODIS chain as
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

- `turn_detection: null` is the PTT opt-out; the spike confirmed the GA endpoint
  accepts and echoes it (§6.3), so the server-VAD fallback (§4.7) is contingency
  only. Never send `modalities` or top-level `turn_detection`.

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

**This HTTPS POST must not run on the peer task.** It runs on the dedicated
`ring-geodis-tool` task (12 KB stack, TLS needs it). The module exposes a
`tool_cb` so the app can intercept (e.g. to show "looking up…" or to override the
RAG call); the default is the built-in execution above. `name` is ignored by the
backend — always `rechercher_base_geodis`.

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

**Fallback (server VAD):** not needed today — the spike confirmed
`turn_detection: null` is accepted (§6.3). Keep the `enable_ptt=0` path in the
module in case the endpoint changes: hold-to-talk still works, but the model may
auto-respond after ~500 ms of silence mid-hold; on release, send a short silence
tail (as `07` does) and let the VAD commit, and only send `commit()` if no
response has started. The example logs which mode it is in.

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
   (`ring_nvs_get_str("geodis", "base_url", …)` / `"sdp_url"`), else the
   compile-time UAT defaults. Use `ring_nvs_get_str`, not `ring_secret_get()`:
   a URL is not a secret, and `ring_secret_get()` persists its fallback.
2. `ring_geodis_init(state_cb, text_cb, tool_cb, NULL)`.
3. **Connect task** (8 KB internal): `ring_geodis_start(&cfg)` only. The module
   brings up Wi-Fi + SNTP itself (`ring_geodis.c`); the app must not. Retry with
   a 5 s backoff; show each step.
4. **One persistent audio task** (PSRAM stack ≥ 128 KB,
   `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`), created once at startup, with
   an explicit state machine — **not** a task spawned on press, because the reply
   arrives after release:
   - `IDLE`: wait for a UI event.
   - `LISTENING` (button pressed): `ring_geodis_set_listening(true)` (a press
     while speaking is a barge-in — the module cancels and flushes), then loop
     `ring_audio_read()` stereo 20 ms → `ring_audio_stereo_to_mono()` →
     `ring_geodis_send_audio(mono, 960)`; update the level bar from
     `ring_audio_level_pct()`.
   - On release: `ring_geodis_set_listening(false)`; `ring_geodis_commit()`.
   - `SPEAKING`: `ring_geodis_read_audio()` → mono → stereo →
     `ring_audio_set_mute(false)` → `ring_audio_write()` → mute. Use the `10`
     pattern: unmute once, a grace timer before the first frame, drain on
     `response.done` / state `READY`.
   - The task stays alive across turns; only the uplink/playback state changes.
5. Tool call: `tool_cb` shows `looking up…` before the built-in RAG POST. A
   completion path (status + chunk count) is still to be added to the module
   (§5.6); until then the UI infers completion from the first assistant delta.
6. Barge-in: press while speaking. `set_listening(true)` already cancels the
   response and flushes the downlink ring; a separate tap-to-`cancel()` gesture
   is redundant and should not be added.

### 5.4 Config / secrets

- No API key (the gateway is unauthenticated). Do **not** add a fake key.
- Defaults in `ring_geodis.h`:
  `RING_GEODIS_DEFAULT_BASE_URL = "https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1"`,
  `RING_GEODIS_DEFAULT_SDP_URL  = "https://geodisopenai.openai.azure.com/openai/v1/realtime/calls"`.
- Optional NVS override (`ring_nvs_get_str`) so the DEV endpoint can be targeted
  without a rebuild.
- `chat_session_id`: generated by the module per session (`ring-` + 8 hex digits
  from `ring_rand_u32()`, seeded from `esp_timer_get_time()`). The app does not
  generate it; it only reads it if it wants to persist transcripts later.

### 5.5 Mock-first verification (do this before the real network)

`CONFIG_RINGKIT_GEODIS_MOCK` (default `n`) replaces the network half of the
module — token, SDP, peer, HTTPS — with a stand-in that runs the **same** state
machine, event queue, downlink ring, tool task and public API. It needs no UDP,
no Wi-Fi and no metered endpoint, so it is the desk path for example 14:

1. Build `14` with `CONFIG_RINGKIT_GEODIS_MOCK=y` and verify the whole UI:
   connect → `READY`, press → `LISTENING`, release → transcript → tool call →
   canned PCM → `READY`, press-while-speaking → barge-in.
2. The mock's `/tool-call` fails by design (`lookup_unavailable`), which
   exercises the real failure path — the one hardest to provoke against a
   healthy gateway.
3. Only then build with the mock off and run on a UDP-capable network.

This turns the UDP risk from "the example may be unverifiable" into "verify
offline first, then confirm online". A mock-enabled `12_ringkit_selftest` build
(`build/12_mock`, `CONFIG_RINGKIT_GEODIS_MOCK=y`) already compiles and links the
mock transport; example 14 is its first real consumer. Example 14 builds both
ways: real 2,691,792 B (12% free), mock 1,903,408 B (38% free). The mock build
uses a private sdkconfig (`-DSDKCONFIG=build/14_geodis_push_to_talk_mock/sdkconfig`)
so the shared root `sdkconfig` stays on the real transport.

**Verified on hardware 2026-11-21** (mock build of `14`): connect → `READY`;
press → `LISTENING`; release → user transcript → `looking up…` → tool result →
canned reply → `READY`; press while the reply plays → barge-in (`playback
cancelled`, `mock: reply interrupted`). `commit->first audio` 1003–1022 ms,
`frames_dropped=0`, no panic. The log proves the state machine and the tool
path; the on-screen rendering still needs eyes.

**Mock fix found by that run:** `ring_geodis_send_event()` had no mock branch,
so with `s.peer == NULL` every `commit()` returned `ESP_ERR_INVALID_STATE` and
the mock never started a turn. It now takes the same
`#ifdef CONFIG_RINGKIT_GEODIS_MOCK` path as `ring_geodis_uplink_send()`, and the
mock triggers a turn on `input_audio_buffer.commit` only (the tool path's
`response.create` would otherwise start a duplicate reply).

### 5.6 Module additions before the app

The review (`notes/geodis-code-review.md` §6.3–§6.5) identified what the app
cannot work around.

**Done 2026-11-20:**

- **`ring_geodis_stats_t` + `ring_geodis_get_stats()`** — `session_setup_ms`,
  `token_ms`, `sdp_ms`, `commit_to_first_audio_ms`, `frames_sent`,
  `frames_dropped`, `tool_calls`, `tool_ms`. Reset by `start()`; written from
  the start/signal/peer/audio/tool tasks, so read it as "last known".
  `frames_dropped` is converted from the byte count `ring_byte_ring_push()`
  already reported; `commit_to_first_audio_ms` is measured in
  `ring_geodis_dl_push()` so the mock reports the same number as a real session.
- **Tool lifecycle** — `ring_geodis_set_tool_done_cb()` registers a
  `ring_geodis_tool_done_cb_t` (`name`, `ok`, HTTP `status`, `chunks`), called on
  `ring-geodis-tool` for module-executed lookups. `chunks` is the
  `retrievedChunks` count via the new `ring_json_count_array()` (host-tested),
  or -1 when unavailable. App-handled calls (`tool_cb` returns true) own their
  result and get no done event.
- **Error reason accessor** — `ring_geodis_last_err()` exposes the context's
  `last_err` (set on start failure, session.update failure and peer loss).

**Still open:**

- **French STT hint** — test `{"model":"whisper-1","language":"fr"}` in the host
  spike and measure `commit_to_first_audio_ms` with and without; auto-detect
  costs latency and misfires on short utterances. (Needs a live spike; not run.)
- **Shared WebRTC core** — pull `ring_rtc_core` (peer ownership, Opus, downlink
  ring, uplink-open flag, event queue, PTT) out after the app, per the review's
  §7 order; at minimum fix the same latent bugs in `ring_net` (peer lifetime,
  uplink-open vs state) so the two transports stop diverging. The app must not
  encode the current fork's semantics.
- **Session reconnect policy** — review §6.4 (`RING_GEODIS_RECONNECTING`,
  jittered backoff, press-to-retry that skips Wi-Fi/SNTP when they are still
  valid, idle teardown). Not needed for the first app: it uses `stop()` +
  `start()`. Track as a post-app follow-up.

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
├── src/ring_geodis_payload.{c,h}      # pure request-body builders (host-tested)
├── src/ring_geodis_mock.{c,h}         # CONFIG_RINGKIT_GEODIS_MOCK transport
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
| `11_glyph_grid_reveal` | 736 KB | — | unchanged |
| `09_hello_smoke_test` | 804 KB | — | unchanged (module dropped by `--gc-sections`) |

> Point-in-time numbers (2026-07-30). `07` is ~2642 KB in `AGENTS.md` and
> 2,704,928 B in `notes/geodis-code-review.md` §6.1; the table above says
> 2604 KB. Re-measure at the end of Phase 2 rather than editing history.
> The `build/*` dirs on disk at 2026-11-20 23:00 were produced 21:16–21:23,
> **before** the diagnostics commit `0cc397a` (22:51), so they verify the
> pre-diagnostics tree; rebuild from clean dirs when 14 lands.
>
> Re-measured at Phase 2a close (2026-11-20, clean dirs): `14` real
> 2,691,792 B (12% free), `14` mock 1,903,408 B (38% free), `12` 2,777,856 B
> (10% free).

Vendored overrides still win (`espressif__esp_codec_dev` 300 refs,
`viewesmart__smartring_plus` 66 refs, all from `components/`). Host tests:
`make check` → 1596 checks, 0 failures at 2026-11-20 (1425 at the 2026-07-30
build; §6A.5's 1480 was the review-fix point).

### 6A.4 Hardware check (resolved 2026-11-20)

**The CST816S failure was a power-cycle issue, not a board fault.** A physical
USB power cycle clears it; a software reset via the USB-JTAG RTS pin does not.
The ring now enumerates on `/dev/ttyACM0` (`303a:1001`) and boots.

**Boot log — mock build of `14`, 2026-11-20 23:45:**

```
I (1161) smartring: Smart Ring Plus starting (ringkit 1.1.0, app: 14_geodis_push_to_talk)
I (1563) CST816S: IC id: 182
I (2124) ringkit: display=ok battery=ok(AXP2101/V2) power=ok audio=ok sd=ok imu=ok
I (2161) ring_geodis: opus encoder: in 3840 B out 180 B
I (2164) ring_geodis: tool task started (hwm 10448 words)
I (2165) ring_geodis: initialised
I (2259) 14_geodis_push_to_talk: audio task started (stack hwm 124632 B)
I (2271) ring_geodis: config: https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1, voice coral, push-to-talk, stt on
W (2272) ring_geodis: MOCK TRANSPORT: no network, canned replies
I (2276) smartring: app '14_geodis_push_to_talk' is on screen
I (2575) ring_geodis: state -> ready
I (2575) ring_geodis: mock: session ready (voice coral, push-to-talk, stt on)
I (2578) 14_geodis_push_to_talk: session ready
```

No `panic` / `assert` / `Guru Meditation`. `ring_geodis_init()` and the mock
session come up on the device; the UI/touch sequence still needs a human (§5.5).
The audio task's 128 KB PSRAM stack reports ~122 KB free at start.

**Run log — mock turn + barge-in, 2026-11-21 00:00:**

```
I (78657)  ring_geodis: state -> listening
I (85109)  ring_geodis: state -> ready
I (85520)  ring_geodis: state -> speaking
I (86120)  ring_geodis: tool 'rechercher_base_geodis' queued (34 B args)
I (86122)  14_geodis_push_to_talk: tool call: rechercher_base_geodis
W (86140)  ring_geodis: tool 'rechercher_base_geodis': http_error (HTTP 0)
I (86141)  14_geodis_push_to_talk: tool done: rechercher_base_geodis ok=0 status=0 chunks=-1
I (87723)  ring_geodis: state -> ready
I (87723)  ring_geodis: mock: turn complete
I (88210)  14_geodis_push_to_talk: stats: setup=304 ms ... commit->first audio=1003 ms sent=324 dropped=0 tools=1 tool=0 ms
...
I (213605) 14_geodis_push_to_talk: playback cancelled (barge-in)
I (213608) ring_geodis: state -> ready
I (213608) ring_geodis: state -> listening
I (213666) ring_geodis: mock: reply interrupted at 0/76800 samples
```

`commit->first audio` 1003–1022 ms across turns, `frames_dropped=0`. The stats
are cumulative since `start()` (as the header documents), so `sent`/`tools`
grow across turns.

### 6A.5 Review fixes (2026-11-12, after `b7e0065`)

`notes/geodis-code-review.md` read the module against how example 14 will use
it. The P0/P1 findings are fixed; the module still has never run on hardware,
so the fixes are compile-verified only (`12_ringkit_selftest` links and builds,
host tests 1596 checks / 0 failures at 2026-11-20).

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

Still open from the review (P2; not blocking the build): `keepalive()` still
re-sends `session.update` instead of being renamed/replaced by a real reconnect;
the three module tasks and the PSRAM buffers are created at `init()` rather than
`start()` (so `12_ringkit_selftest` holds ~22 KB of idle stacks); and
`send_stream_id` is a plain `uint16_t` written by the peer task and read by the
app task. `response.created` → `SPEAKING`, the DEBUG demotion of DataChannel
frame logging, the `init()` guard and the wrap-safe `now_ms()` are done.

### 6A.6 Real-transport fixes (2026-11-21, found on hardware)

Flashing the real build of `14` exposed three bugs the mock could not:

- **DTLS role.** Azure answers `a=setup:active` to `esp_peer`'s
  `a=setup:actpass` offer, making the ESP32 the DTLS/SCTP **server**. esp_peer's
  data-channel role follows the DTLS role, so it waited for the peer to open the
  channel — but the Azure Realtime API expects the client to open it. DTLS and
  SCTP came up, then the session timed out with no `session.created`.
  `ring_geodis_signaling.c` now rewrites the offer to `a=setup:active`
  (`force_dtls_client()`); Azure answers `passive`, the ESP32 is the client, and
  esp_peer sends DCEP automatically (`data channel 'esp_channel' open`).
- **`session.type`.** The GA endpoint rejected `session.update` with
  `missing_required_parameter: session.type`. The payload builder now emits
  `{"type":"session.update","session":{"type":"realtime","audio":{…}}}`.
- **Comfort noise is not a reply.** The endpoint streams silence/comfort-noise
  audio continuously (a host probe counted 645 frames/25 s, starting before
  `session.created`). `ring_geodis_codec.c` marked `speaking` on every audio
  frame, so the UI sat on `speaking…`. Only `response.created` sets it now, and
  `set_listening(true)` flushes the 4 s downlink ring so a press does not play
  back the accumulated silence.

**Boot log — real build, 2026-11-21 (abridged):**

```
I (2395)  ring_geodis: state -> connecting
I (10162) ring_geodis: local SDP ready (949 bytes)
I (22170) ring_geodis: peer connected
I (22201) ring_geodis: data channel 'esp_channel' open (stream 2)
I (22202) ring_geodis: session created -> configuring
I (22208) ring_geodis: session.update sent
I (22268) ring_geodis: session configured
I (22268) ring_geodis: state -> ready
I (22270) 14_geodis_push_to_talk: session ready
```

No `panic`/`assert`. The real session reaches `READY` on the ring; a live user
turn (mic → STT → tool → TTS) is the one remaining unverified step.

---

## 7. Phases

| Phase | Deliverable | Done when |
|---|---|---|
| **0** ✅ | `scripts/test_geodis_api.py --mode ptt` | PTT + codec + TLS confirmed — **done 2026-07-30**, 10/10 |
| **1** ✅ | `ring_geodis` module in `ringkit` | builds; `12_ringkit_selftest` can init/stop it — **built 2026-07-30** |
| **1b** ✅ | Hardware check of `ring_geodis` | the module initialised and a mock session came up on the ring via `14` (§6A.4); `12` not re-run |
| **2a** ✅ | `examples/14_geodis_push_to_talk` against `CONFIG_RINGKIT_GEODIS_MOCK` | **verified on hardware**: full state sequence + barge-in (§5.5); real + mock builds green |
| **2b** ✅ | `14` on a real network | real session reaches `ready` (§6A.6); one real turn end-to-end — **live turn pending a human** |
| **3** | Docs + verification | README, app table, ringkit map, boot + run logs, `make check` — **done**; the on-screen rendering and a real conversation still need eyes/ears |

**Next: the live user turn.** The real build is flashed and reaches
`session ready` (§6A.6). What remains is a human: hold, speak, release, hear the
reply, see both transcripts. The mock path and the real signalling path are
verified; the mic → STT → tool → TTS loop is not.

Phase 0 is complete: `turn_detection: null` is accepted, the PTT clip → commit →
reply flow works end-to-end (with the client-executed RAG call), the SDP
negotiates Opus/48000/2, and both hosts validate against the Mozilla bundle.
Phase 1 is unblocked; after the §6A.5 review fixes the module is an adaptation
of `ring_net`, not a new stack.

---

## 8. Risks

### Resolved

1. **PTT turn detection.** `audio.input.turn_detection = null` is accepted and
   echoed by `session.updated`; the clip → commit → reply flow works (§6.3). The
   server-VAD fallback (§4.7) is kept only in case the endpoint changes.
2. **TLS.** Both hosts validate against the Mozilla bundle (`certifi`); the
   module uses `ring_http_init_bundle()` (`esp_crt_bundle_attach`). No embedded
   CA is required unless the ESP bundle is later trimmed.
3. **Token/session lifetime.** The `ek_` token is valid for > 120 s and is spent
   at the SDP exchange; the module fetches a fresh one per session. The idle
   session lifetime is undocumented (`idle_timeout_ms: null`), so `keepalive()`
   is not a lifetime extension — see §4.2.
4. **Duplicate `vApplicationStackOverflowHook`.** Moved to
   `components/ringkit/src/ring_stack_hook.c` before the second WebRTC module.
5. **Two HTTPS hosts.** `ring_http` gained `ring_http_init_bundle()` for the
   Azure/GEODIS public certs; `ring_geodis_signaling.c` uses it.
6. **Flaky gateway retries.** `/token`, `/tool-call` and the SDP POST retry
   transport errors (the gateway resets ~1 in 12 TLS connections); 4xx is never
   retried.

### Open

1. **UDP/DTLS blocked on the bench LAN.** `07` cannot complete DTLS there; GEODIS
   is the same WebRTC transport. On-device verification needs a network with
   outbound UDP (a phone hotspot is the usual workaround). **Mitigation:** verify
   the app first against `CONFIG_RINGKIT_GEODIS_MOCK` (§5.5), which needs no
   network.
2. **Image size.** `07` is ~2642 KB in a 3000K partition; `14` measures
   2,691,792 B (12% free) real and 1,903,408 B (38% free) mock. The face app
   (`15`) needs the partition raised to 3400K.
3. **Unauthenticated, metered endpoint.** Every session costs GEODIS's Azure
   budget, and `/token` is open and transcripts are readable/writable by
   `chat_session_id`. No looping tests; no automatic retries that start new
   sessions; do not log the conversation at INFO. Must be gated before production.
4. **Tool-call latency.** The RAG POST blocks the reply; surface a `looking up…`
   state so the user knows it is not hung. The module has `tool_cb` for the
   start and, since 2026-11-20, `ring_geodis_set_tool_done_cb()` for the
   completion (`ok`, HTTP `status`, `retrievedChunks` count) — wire both into
   the UI stage line.
5. **RAG content gap.** The KB has no live consignment data (see
   `notes/geodis-ai-api.md` §2); the demo must use a KB-backed prompt or inject
   data via `mcp_database`/`mcp_collection`.
6. **Console back-pressure.** Drain `/dev/ttyACM0` continuously; a capture script
   that sleeps before reading will stall the firmware and mimic a hang.
7. **Hardware verification.** ✅ Resolved 2026-11-21: the ring enumerates, boots
   and the real session reaches `READY` (§6A.4, §6A.6). The live user turn
   (mic → STT → tool → TTS) still needs a human.

---

## 9. Verification checklist

- **Host spike:** `scripts/.venv/bin/python scripts/test_geodis_api.py --mode ptt`
  passes (commit → reply, tool call, transcript). `--mode live` (mic/speaker)
  exists for a manual end-to-end check.
- **Mock build:** ✅ 2026-11-21 — `14` builds with `CONFIG_RINGKIT_GEODIS_MOCK=y`
  (1,903,408 B, 38% free; real 2,691,792 B, 12% free) and runs on the ring:
  connect → `READY`, press → `LISTENING`, release → transcript → tool call →
  canned audio → `READY`, press-while-speaking → barge-in (§5.5, §6A.4).
- **Builds from clean dirs:** `11_glyph_grid_reveal`, `09_hello_smoke_test`,
  `07_qianwen_realtime_harness`, `10_qianwen_ws_voice_chat`,
  `14_geodis_push_to_talk` — all green 2026-11-20 (11 750,672 B, 09 823,392 B,
  07 2,707,328 B, 10 1,633,792 B; 14 above).
- **Vendored overrides still win:** `espressif__esp_codec_dev` and
  `viewesmart__smartring_plus` (see `AGENTS.md`).
- **Boot log:** ✅ 2026-11-20 mock build — `ringkit:` summary all `ok`,
  `app: 14_geodis_push_to_talk`, `session ready`, no `panic` / `assert` /
  `Guru Meditation` (§6A.4). ✅ 2026-11-21 real build — reaches `session ready`
  (§6A.6).
- **On-device mock touch sequence:** ✅ 2026-11-21 — verified from the log
  (§6A.4); the on-screen rendering itself still needs eyes.
- **On-device real session:** ✅ 2026-11-21 — reaches `READY` on the ring
  (§6A.6). **Live user turn pending a human:** hold, speak, release, hear the
  reply, see both transcripts. The log proves the pipeline runs, not that the
  conversation is intelligible.
- **Unit tests:** `cd tests && make check` — 1596 checks / 0 failures at
  2026-11-20 (the payload builders are pure and host-tested; 62 of those).
- **Stats:** the harness prints `ring_geodis_stats_t` per turn (§5.6 landed
  2026-11-20); check `commit_to_first_audio_ms` and `frames_dropped` are sane.

---

## 10. Docs to update

- ✅ `examples/README.md` — `14_geodis_push_to_talk` added to the app table.
- ✅ `components/ringkit/README.md` — module map (`ring_geodis`), task table,
  state machine, and the voice transport decision guide (GEODIS = Azure
  Realtime over WebRTC, client-executed tools). Line 47 now points at
  `examples/14` instead of saying "no app yet".
- ✅ `AGENTS.md` — module list, `ring_geodis_*` API, the
  `vApplicationStackOverflowHook` move, this note in the docs map, the
  `examples/14` architecture-tree row and its README in the deep-dive list.
- ✅ `scripts/README.md` — documents `--mode ptt` and `--mode live`.
- ✅ `notes/geodis-ai-api.md` — the Phase 0 findings are recorded (STT opt-in, the
  transcript event, the tool-call shape, SDP 400/201) and the note now links
  this plan.
- ✅ `notes/geodis-push-to-talk-plan.md` — status block current; build/test
  numbers re-measured from clean dirs at Phase 2a close (2026-11-20).
