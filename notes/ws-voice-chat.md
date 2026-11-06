# WebSocket voice chat — design brainstorm for `examples/10_qianwen_ws_voice_chat`

**Status:** research / design only (no code yet). Companion to
`notes/qianwenai.md` (§1 protocols, §2.10 S2S models, §3 transports, §3.3
WebSocket protocol) and `notes/networking.md` §5.2.

**Goal.** A voice conversation with `qwen-audio-3.1-realtime-flash` over the
**WebSocket** realtime protocol — the minimum-dependency path (no `esp_peer`,
no DTLS/ICE, no Opus), and the only realtime transport that works on this
bench network today.

---

## 1. Why this example exists

Three reasons, in order of weight:

1. **This network blocks UDP.** `examples/06_qianwenai_push_to_talk/README.md`
   §8: TCP/443 to the API works, but *all* outbound UDP is dropped, so the
   DTLS handshake times out and 06/07 cannot complete a call here. WebSocket
   is TCP/TLS 443 — it works on this LAN. This is the only way to demo a real
   voice conversation on the bench.
2. **It is the smallest realtime path.** `esp_websocket_client` + `esp-tls` +
   base64; no `esp_peer`, no DTLS-SRTP, no ICE/STUN/TURN, no signaling server,
   no codec. `notes/networking.md` §5.4 recommends exactly this as the first
   voice path ("Realtime voice assistant (first version) → Realtime over
   WebSocket").
3. **It can do things WebRTC cannot.** Manual turn-taking (`turn_detection:
   null` + `input_audio_buffer.commit` + `response.create`), semantic turn
   detection (`smart_turn_v2`), barge-in (`response.cancel` on
   `input_audio_buffer.speech_started`), and function calling
   (`response.function_call_arguments.done` → `conversation.item.create` →
   `response.create`). WebRTC supports only `server_vad`/`semantic_vad`.

Non-goals: acoustic echo cancellation (the ring has none and the WS transport
has none — half-duplex), bandwidth efficiency (~120 KB/s vs ~32 kbps Opus),
and the QUIC comparison (that is `11_quic_voice_chat`).

---

## 2. The hard constraint: PCM at the wrong rates

The WebSocket endpoint speaks **raw PCM**, not Opus:

| Direction | Format |
|---|---|
| Input (client → server) | PCM, **16 kHz**, 16-bit, mono, base64 in `input_audio_buffer.append` |
| Output (server → client) | PCM, **24 kHz**, 16-bit, mono, base64 in `response.audio.delta` |

The ring's audio path is fixed at **48 kHz / 16-bit / stereo** (`ring_audio`).
So a new `ring_ws` module must resample at the boundary:

```
mic 48k stereo ──fold──► 48k mono ──decimate 3:1──► 16k mono ──base64──► JSON
speaker 48k stereo ◄─expand── 48k mono ◄─interp 1:2── 24k mono ◄─base64── JSON
```

Both ratios are exact integers, so a small polyphase FIR is enough:

* **48k → 16k (decimate by 3):** anti-alias lowpass, cutoff ~7.5 kHz,
  ~64–128 taps at 48 kHz (21–43 per phase). ~1.3M MAC/s — trivial for the S3.
* **24k → 48k (interpolate by 2):** half-band lowpass, cutoff ~11 kHz,
  ~24–48 taps at 48 kHz (12–24 per phase). ~0.6M MAC/s.

Generate the coefficients offline with `tools/gen_ws_resampler.py`
(Kaiser-windowed sinc) and commit the table in `ring_ws_resample.c` — the
same pattern as `tools/lvgl_img_pack.py`. Group delay is ~1 ms, negligible
against the 300–800 ms playback jitter buffer.

The module hides this: the app-facing API stays **48 kHz mono**, identical to
`ring_net`, so 06/07-style app code ports almost unchanged.

---

## 3. Transport: `esp_websocket_client`

Managed component `espressif/esp_websocket_client` **v1.8.0** (requires
IDF ≥ 5.0 — fine on v6.1). Add to `main/idf_component.yml`.

| Need | API / field |
|---|---|
| Endpoint | `uri = "wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash"` |
| Auth | `config.headers = "Authorization: Bearer sk-...\r\n"` (each header CRLF-terminated; `esp_websocket_client_append_header()` also exists for pre-start use) |
| TLS | `cert_pem` = the embedded `ring_net_ca_pem()` (the server sends only its leaf, so the intermediate must be supplied) |
| Fragmentation | `esp_websocket_event_data_t.payload_offset` / `payload_len` / `fin` / `op_code` — reassemble into a PSRAM buffer before parsing |
| Send | `esp_websocket_client_send_text()` |
| Buffer | `config.buffer_size` — raise to 8–16 KB to cut the event count |
| Keep-alive | `ping_interval_sec` ~10–30 s |
| Reconnect | `disable_auto_reconnect = true` — a reconnect needs a fresh `session.update`, so our state machine owns retries |
| Events | `esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, cb, arg)` (needs the default event loop; `ring_wifi_init()` already creates it) |

The WS task stack/priority are configurable (`task_stack`, `task_prio`). The
event callback must stay lean — reassemble, parse, push to a ring — with state
changes marshalled to an event task exactly like `ring_net.c` does.

**Bandwidth honesty.** 16 kHz × 16-bit = 32 KB/s uplink, 24 kHz × 16-bit =
48 KB/s downlink, +33 % base64 + JSON overhead ≈ **~120 KB/s**, versus ~32 kbps
for Opus. That is the price of the simple path, and it is exactly the
head-of-line exposure the QUIC note worries about. The example should show
the wire rate on screen so the tradeoff is visible.

---

## 4. Protocol mapping

Endpoint: `wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash`,
`Authorization: Bearer <key>` in the handshake. Lifecycle:

```
connect ──► session.created ──► session.update ──► session.updated ──► READY
                                                                  │
                       input_audio_buffer.append (16k PCM b64) ◄──┤ LISTENING
                                                                  │
                       response.audio.delta (24k PCM b64) ───────►│ SPEAKING
                       response.audio_transcript.delta           │
                       response.done ─────────────────────────────► READY
```

Turn-taking modes (`session.turn_detection`):

| Mode | Config | Use |
|---|---|---|
| Push-to-talk | `null` | button gates the mic; `input_audio_buffer.commit` + `response.create` |
| Acoustic VAD | `{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}` | hands-free, latency-tolerant |
| Semantic | `{"type":"smart_turn_v2"}` | hands-free, ignores "嗯/啊"; **recommended by the platform** |

Context: 50 audio turns / 300 s max; `max_history_turns` default 20.

Events the module handles:

| Server event | Action |
|---|---|
| `session.created` | send `session.update` (modalities, voice, turn_detection, instructions, tools) |
| `session.updated` | → READY |
| `input_audio_buffer.speech_started` | barge-in: `response.cancel()`, flush downlink, → LISTENING |
| `input_audio_buffer.speech_stopped` | log only |
| `input_audio_buffer.committed` | log only |
| `response.created` | → SPEAKING |
| `response.audio.delta` | base64 → 24 kHz PCM ring |
| `response.audio_transcript.delta` / `.done` | transcript cb (assistant) |
| `response.done` | → READY (status `completed`/`cancelled`) |
| `conversation.item.input_audio_transcription.completed` | transcript cb (user) |
| `response.function_call_arguments.done` | tool cb → app writes `function_call_output` → `response.create` |
| `error` | log; `server_error` → ERROR (fatal), `invalid_request_error` → log |

`session.update` fields worth using: `modalities: ["text","audio"]`,
`voice: "longanqian_v3.1"`, `speech_rate`, `instructions` (system prompt),
`input_audio_transcription.language`, `output_audio.language`,
`max_history_turns`, `enable_search`, `tools`.

---

## 5. ringkit: a new `ring_ws` module

Mirror `ring_net`'s shape so app code is portable:

```c
/* components/ringkit/include/ringkit/ring_ws.h */
typedef enum {
    RING_WS_OFF = 0, RING_WS_CONNECTING, RING_WS_READY,
    RING_WS_LISTENING, RING_WS_SPEAKING, RING_WS_ERROR,
} ring_ws_state_t;

typedef struct {
    const char *voice;           /* "longanqian_v3.1" */
    const char *instructions;    /* optional system prompt */
    const char *turn_detection;  /* "server_vad" | "smart_turn_v2" | NULL = PTT */
    bool        enable_search;
    const char *tools_json;      /* optional function-calling tools array (M4) */
} ring_ws_session_cfg_t;

/* Returned by ring_ws_turn_mode() so the UI can show which mode won. */
esp_err_t ring_ws_set_turn_mode(const char *type);   /* runtime switch/fallback */

typedef void (*ring_ws_state_cb_t)(ring_ws_state_t state, void *user);
typedef void (*ring_ws_transcript_cb_t)(const char *text, size_t len,
                                        bool is_user, void *user);
typedef void (*ring_ws_tool_cb_t)(const char *name, const char *args_json,
                                  void *user);

esp_err_t ring_ws_init(ring_ws_state_cb_t, ring_ws_transcript_cb_t,
                       ring_ws_tool_cb_t, void *user);
esp_err_t ring_ws_start(const char *model, const char *api_key,
                        const ring_ws_session_cfg_t *cfg);
esp_err_t ring_ws_stop(void);
ring_ws_state_t ring_ws_state(void);
const char *ring_ws_state_name(ring_ws_state_t);

esp_err_t ring_ws_set_listening(bool on);          /* open/close the mic */
esp_err_t ring_ws_send_audio(const int16_t *pcm48k, size_t samples);  /* 960 = 20 ms */
esp_err_t ring_ws_read_audio(int16_t *pcm48k, size_t samples,
                             size_t *out, uint32_t timeout_ms);
bool     ring_ws_is_speaking(void);

esp_err_t ring_ws_commit(void);          /* PTT: commit + response.create */
esp_err_t ring_ws_response_create(void);
esp_err_t ring_ws_cancel(void);          /* barge-in */
esp_err_t ring_ws_send_json(const char *json);
```

Tasks: the `esp_websocket_client` owns its own task; `ring_ws` adds a
`ring-ws-event` task to marshal callbacks, plus the app's own audio task. The
resampler and base64 live in `ring_ws`; use `mbedtls_base64_encode/decode`
(already linked — verified: `mbedtls_base64_encode/decode` appear in
`build/06_qianwenai_push_to_talk/smartring.map`) rather than hand-rolling
base64.

### Stack and buffer budget (state it, don't discover it)

`ring_ws` has **no codec**, which is the whole point of this transport, and it
changes the memory story completely from 06/07:

| | 06/07 (`ring_net`) | `ring_ws` |
|---|---|---|
| Audio task stack | **64 KB**, PSRAM (`xTaskCreateWithCaps`) — stereo Opus | **~8 KB**, internal RAM — no codec |
| Downlink ring | 4 s / 384 KB PSRAM (`RING_NET_DL_BYTES`), newest-wins | 4 s / 384 KB PSRAM, newest-wins (**same shape**) |
| Uplink scratch | 20 ms mono→stereo up-mix | 20 ms 48k→16k decimate |

FreeRTOS stacks come from internal RAM only, which is why 06/07 needed
`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`. Without Opus, a plain ~8 KB
`xTaskCreate` suffices — do not copy 06's 64 KB, and do not copy its
PSRAM-stack workaround either.

**Downlink jitter ring.** At 24 kHz mono the model streams ~48 KB/s, so a 4 s
ring is 384 KB — the same order as `ring_net`'s. Keep an explicit equivalent
with the same **newest-wins** overflow policy; a reply arriving faster than
playback must drop old audio, never grow unbounded. Without this the reply
stutters on a slow link.

**JSON helpers — copy for v1, dedupe separately.** `ring_net.c`'s
`json_get_str`/`json_type_is` are the only working JSON path in the repo and
are linked by both 06 and 07. Extract them into `ring_json.[ch]` **as a
standalone change** later (drop the static copies in `ring_net.c`, include the
header, rebuild 06 *and* 07), not as part of `ring_ws`. Coupling a known-good
transport to a brand-new one during bring-up is how the WebRTC examples break.

---

## 6. The app: `examples/10_qianwen_ws_voice_chat`

The simplest of the three voice examples: no button-gated mic by default, a
rolling transcript, and the transport stats on screen.

```
        ┌─────────────────────────────┐
        │        ◠◠◠◠◠◠◠◠◠◠◠          │  ← orb: level arc, green = mic,
        │      ◜             ◝        │     blue = assistant
        │     │   listening…   │      │
        │      ◟      “你好，        │  ← rolling transcript
        │        ◡◡◡◡◡◡◡◡◡◡◡        │
        │─────────────────────────────│
        │ ws · pcm 16k→24k · 118 KB/s │  ← the transport story
        │ smart_turn_v2 · turn 3      │
        └─────────────────────────────┘
```

* **Hands-free by default** (`smart_turn_v2`), half-duplex. The mic gate is
  not "am I speaking" but the docs' own echo-suppression predicate:

  ```
  mute_mic = responding || downlink_buffered > 0
             || (now - last_playback_ms) < 500
  ```

  `responding` is set on `response.created` and cleared on `response.done`;
  `last_playback_ms` is refreshed on **every** playback write. Without the
  500 ms tail the ring hears the end of its own reply and answers itself.
  A loud utterance during playback is a barge-in: `response.cancel()` + flush
  the downlink ring.
* **Turn-mode fallback (required).** `smart_turn_v2` is not guaranteed for
  every model/account. On an `error` whose `param` is
  `session.turn_detection.type`, retry `session.update` once with
  `{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}` and
  record the downgrade for the UI. Never hard-fail on the preferred mode.
* **Tap = barge-in**, **long-press = push-to-talk** (one manual turn), and a
  settings page for voice / language / `speech_rate` / `instructions`.
  Touch is a control, not a talk key.
* **Function calling (M4, optional):** register `get_ring_status` (battery %,
  charging, voltage, tilt, IMU temperature) via `session.tools`. On
  `response.function_call_arguments.done`, `ring_ws_tool_cb` runs the ringkit
  APIs, writes a `function_call_output` item, then `response.create`. This is
  a ring-native capability the WebRTC examples cannot show — but it is a whole
  subsystem (callback, item writer, re-trigger, live verification of the event
  names), so it lands last, after the voice loop is trustworthy.
* **Stats strip:** wire bytes/s (up/down), turn count, active turn mode, last
  `response.done` status. Makes the PCM-over-JSON cost visible next to the
  WebRTC examples' silence.

### App flow (push-to-talk variant, for comparison)

1. `ring_ws_start(model, key, &cfg)` with `turn_detection = NULL` → READY.
2. Press → `ring_ws_set_listening(true)` → stream 48 kHz mono →
   `ring_ws_send_audio()`.
3. Release → `ring_ws_set_listening(false)` → `ring_ws_commit()` +
   `ring_ws_response_create()`.
4. Play 24 kHz → 48 kHz audio until `response.done` → READY.

---

## 7. Numbering (decided: WS = 10, QUIC = 11)

`notes/quic-voice-chat.md` originally reserved `examples/10_quic_voice_chat`.
**This note takes `10_`; QUIC moves to `examples/11_quic_voice_chat`**, and
that rename has been applied to `notes/quic-voice-chat.md` (title, §6 heading,
§10 checklist), `AGENTS.md` and `examples/README.md`.

Rationale: WS is the baseline transport the QUIC note itself says to keep for
the A/B comparison, it is small (~3 days) and demoable on this network, while
QUIC is ~2 weeks and hostage to a wolfSSL/ngtcp2 port. The A/B facade
(`ring_voice_*`) then grows a third backend later.

---

## 8. Dependencies & config

`main/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.5"
  viewesmart/smartring_plus: "^1.1.2"
  espressif/esp_peer: "^1.5.8"          # existing (WebRTC examples)
  espressif/esp_audio_codec: "^2.6.2"   # existing (Opus for WebRTC)
  espressif/esp_websocket_client: "^1.8.0"   # NEW (this example)
```

`components/ringkit/CMakeLists.txt`: add `esp_websocket_client` to
`REQUIRES` (`mbedtls`/`esp-tls` are already there). No new sdkconfig options
are required — `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, the cert bundle, SNTP
and HTTPS are already set for `ring_net`. The WS client uses the embedded
Qianwen CA via `cert_pem`, so the bundle is not needed for this endpoint.

**Partition — verify, don't assume.** `partitions.csv` has a **3000K** app
partition. Measured images: `06_qianwenai_push_to_talk` = **2718 KB** (only
**282 KB** free), `07` = 2640 KB, `05_wifi_https_demo` = 1558 KB. This example
drops `esp_peer` + libsrtp + Opus and adds the websocket client and a small
resampler table, so it is *expected* to come in well under 06 — but that is an
expectation, not a measurement. **Check the built binary against 06's 282 KB
of headroom before declaring the partition fine**, and raise it only if the
build actually exceeds it.

---

## 9. Risks / unknowns (rank before coding)

| # | Risk | Mitigation |
|---|---|---|
| 1 | `headers` config + `wss://` + Bearer works? | Probe first: connect, log `session.created`. On failure check `WEBSOCKET_EVENT_ERROR` / `esp_ws_handshake_status_code` (401/403). |
| 2 | `smart_turn_v2` rejected by the model/account | One-shot fallback to `server_vad` on `param == session.turn_detection.type`; show the downgrade. |
| 3 | Server sends text frames (JSON) — confirm `op_code == 1`; handle binary gracefully. | Log the first event's opcode; reassemble by `payload_offset`. |
| 4 | No AEC → the ring answers itself | Half-duplex with the documented predicate incl. the 500 ms tail (§6); loud-speech barge-in. |
| 5 | Downlink ring unbounded / stutter | 4 s / 384 KB PSRAM ring, newest-wins, same as `ring_net`. |
| 6 | Stack copied from 06 (64 KB) unnecessarily | No codec → ~8 KB internal; do **not** use `xTaskCreateWithCaps`. |
| 7 | Resampler quality / CPU | Exact integer ratios; ~150 lines; profile the audio task. |
| 8 | ~120 KB/s bandwidth | Show the rate; keep sessions short; note the Opus alternative. |
| 9 | Fragmentation > reassembly buffer | Size the buffer 64 KB (PSRAM); drop + log oversized messages. |
| 10 | `session.update` must precede the first audio | Send it on `session.created`; wait for `session.updated` before READY. |
| 11 | Function-calling event names (M4 only) | Verify `response.function_call_arguments.done` / `conversation.item.create` against a live session. |

---

## 10. Milestones

| M | Deliverable | Effort |
|---|---|---|
| **M0** | Add the component; `ring_ws` connect (WS client + `session.created`) + `session.update` + log `session.created/updated`; send a 1 s PCM clip, dump the reply to a file/log. **No UI, no `ring_json` refactor.** | ½ day |
| **M1** | Audio path: resampler + base64 + mic/speaker; explicit jitter ring and task-stack numbers (see §5 budget); one end-to-end turn. | 1 day |
| **M2** | UI, `smart_turn_v2` **with the `server_vad` fallback**, the half-duplex predicate incl. 500 ms tail, barge-in, transcript, stats. | 1 day |
| **M3** | PTT mode toggle + settings (voice / language / `speech_rate` / `instructions`). | ½ day |
| **M4** | **Optional:** function calling (`get_ring_status`); and the `ring_json` dedupe as its own standalone change (rebuild 06/07). | open |
| **M5** | **Optional:** `ring_voice_*` facade over `ring_ws` + `ring_net` for A/B. | open |

Realistic: **~3 days to M3** for someone who knows `ringkit`: the voice loop
ships before either optional subsystem.

---

## 11. Verification checklist

1. From clean `-B` dirs: `00_glyph_grid_reveal`, `06_qianwenai_push_to_talk`,
   `07_qianwen_realtime_harness`, and the new app all build — the first three
   because they share `ringkit`.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`); confirm `esp_websocket_client` resolved from
   `managed_components/`.
3. New image size measured against 06's 282 KB of partition headroom.
4. Boot log: `ring_ws: session.updated`, `state -> ready`, no
   `panic`/`assert`; the turn mode actually negotiated is logged (`smart_turn_v2`
   or the fallback).
5. **On this bench network** (UDP blocked): a full turn works — the key
   advantage over 06/07.
6. Wire stats on screen roughly match 32 KB/s up / 48 KB/s down + base64
   overhead.
7. Human check: voice quality, turn-taking, barge-in, and (M4) function
   calling. The log proves the pipeline, not the conversation.

---

## 12. Alternatives considered

1. **WebRTC (06/07)** — built-in AEC, but UDP-blocked here and heavier. Keep
   as the other transport.
2. **QUIC/AOQ** — best weak-network behaviour, but no C SDK; gateway project.
   `11_quic_voice_chat`.
3. **HTTP chunked PCM** — no full-duplex, no turn events. No.
4. **Opus over WS** — the endpoint only accepts PCM; not possible.
5. **Reuse `ring_net` with a WS backend** — a facade is the end state, but a
   separate `ring_ws` is simpler first and keeps 06/07 stable.

---

## 13. Sources

* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/websocket-access`
  — endpoint, auth header, interaction modes (server_vad / smart_turn_v2 /
  push-to-talk).
* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/client-events`
  — `session.update` fields, `input_audio_buffer.append/commit/clear`,
  `response.create/cancel`, `conversation.item.create`.
* `platform.qianwenai.com/docs/api-reference/qwen-audio-realtime/server-events`
  — `session.created/updated`, `response.audio.delta`,
  `response.audio_transcript.delta/done`, `response.done`, `error`,
  function-calling events.
* `platform.qianwenai.com/docs/developer-guides/speech/qwen-audio-realtime`
  — 16 kHz in / 24 kHz out, context limits, echo-suppression demo.
* `docs.espressif.com/projects/esp-protocols/esp_websocket_client` — config
  fields (`headers`, `buffer_size`, `ping_interval_sec`), fragmentation
  (`payload_offset`/`payload_len`/`fin`), `esp_websocket_register_events`.
