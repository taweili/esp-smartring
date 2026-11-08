# WebSocket voice chat — review & implementation plan

**Companion to:** `notes/design-ws-voice-chat.md` (the design).
**Status:** review complete; plan ready to execute.
**App:** `examples/10_qianwen_ws_voice_chat` (planned; WS = 10, QUIC = 11).

---

## 0. Verdict

The design is **sound and unusually well-researched**. The core thesis —
WebSocket is the only realtime transport that works on this UDP-blocked bench
network, and it is the smallest dependency path — is correct and verified. The
protocol mapping, the `ring_net` reuse shape, the numbering decision and the
partition arithmetic are all accurate.

The corrections below are **not** about the approach. They are about three
things the design gets slightly wrong or omits:

1. **Session-lifecycle constraints** in the Qianwen API (`turn_detection`,
   `voice`, audio formats are IDLE-only), which invalidate the runtime
   `ring_ws_set_turn_mode()` API and the "PTT toggle" in M3.
2. **The `esp_websocket_client` execution model** — the event handler runs on
   the WebSocket client task, not a separate event-loop task, and the event
   queue is size 1. The design's "reassemble, parse, push to a ring" in the
   callback is a back-pressure hazard.
3. **Config the design says is unnecessary** — `esp_websocket_client` has
   Kconfig options (`SEPARATE_TX_LOCK`, `ENABLE_DYNAMIC_BUFFER`,
   `ALLOC_IN_EXT_RAM`, `TASK_STACK_IN_EXT_RAM`) that matter for simultaneous
   uplink/downlink and for internal RAM.

None of these are blockers. All are cheap to fix in the plan.

---

## 1. What the design gets right (verified)

| Claim | Evidence |
|---|---|
| Endpoint `wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=<model>`, `Authorization: Bearer` in the handshake | Qianwen WebSocket access doc; `notes/qianwen-ai-platform.md` §3.3 |
| Input PCM **16 kHz / 16-bit / mono**, output PCM **24 kHz / 16-bit / mono** | Qianwen realtime developer guide; model page |
| `smart_turn_v2` is a valid, **recommended** mode; `server_vad` and PTT (`null`) also valid | Client-events doc |
| `session.update` fields: `modalities`, `voice`, `speech_rate`, `instructions`, `input_audio_transcription.language`, `output_audio.language`, `max_history_turns`, `enable_search`, `tools`, `turn_detection` | Client-events doc |
| Server events: `session.created/updated`, `input_audio_buffer.speech_started/stopped/committed`, `response.created/audio.delta/audio_transcript.delta/done`, `conversation.item.input_audio_transcription.completed`, `response.function_call_arguments.done`, `error` | Server-events doc |
| `error` event has `error.type` (`invalid_request_error` / `server_error`) and `error.param` | Server-events doc |
| `esp_websocket_client` **v1.8.0**, IDF ≥ 5.0; `headers`, `cert_pem`, `buffer_size`, `ping_interval_sec`, `disable_auto_reconnect`, `task_stack`, `task_prio`, `esp_websocket_register_events`, `WEBSOCKET_EVENT_ANY`, `payload_offset`/`payload_len`/`fin`/`op_code`, `esp_websocket_client_send_text`, `esp_websocket_client_append_header` | Component `idf_component.yml` + `esp_websocket_client.h` |
| `ring_net`'s JSON helpers are static and the only working JSON path; ring is 4 s / 384 KB newest-wins; task layout | `components/ringkit/src/ring_net.c` |
| `mbedtls_base64_encode/decode` already linked | `build/06_qianwenai_push_to_talk/smartring.map` (4 decode, 5 encode refs) |
| Partition 3000K; 06 = 2,783,680 B (~2718 KB, 282 KB free); 07 = 2,703,872 B; 05 = 1,595,264 B | `partitions.csv`, `build/*/smartring.bin` |
| Numbering WS = 10 / QUIC = 11 applied to `notes/design-quic-voice-chat.md`, `AGENTS.md`, `examples/README.md` | repo state |
| `ring_audio` is 48 kHz / 16-bit / stereo; `ring_audio_stereo_to_mono` / `mono_to_stereo` exist | `components/ringkit/include/ringkit/ring_audio.h` |

---

## 2. Corrections — must fix before coding

### C1. `turn_detection`, `voice` and audio formats are IDLE-only

The docs are explicit: **`turn_detection` can only be modified before the first
audio is sent (IDLE state)**, and `voice` / `input_audio_format` /
`output_audio_format` are first-`session.update`-only. Consequences:

- `esp_err_t ring_ws_set_turn_mode(const char *type)` (design §5) is **not
  viable mid-session**. Remove it, or redefine it as "reconnect with a new
  mode".
- The `smart_turn_v2` → `server_vad` fallback (design §6) must happen **at
  session setup**, i.e. on an `error` received in response to the *first*
  `session.update`, before any audio is sent. It is still IDLE then, so the
  retry is legal.
- M3's "PTT mode toggle" and the settings page's "voice" control require a
  **full reconnect** (`ring_ws_stop()` + `ring_ws_start()`), not a runtime
  `session.update`. A session is either PTT or VAD for its lifetime.
- `speech_rate`, `instructions`, `max_history_turns`, `enable_search`,
  `input/output language` are updatable mid-session (no IDLE restriction in
  the docs), but changing the turn mode is not.

**Plan:** make the turn mode and voice part of `ring_ws_session_cfg_t` chosen
at `ring_ws_start()`. Expose `ring_ws_turn_mode()` so the UI can show what was
negotiated. Document that a mode change reconnects.

### C2. The event callback runs on the WebSocket client task

`esp_websocket_client` creates its **own** event loop with `task_name = NULL`
and calls `esp_event_loop_run(..., 0)` from its own task. So the handler
registered with `esp_websocket_register_events()` executes on the WebSocket
client task, and the event queue is `WEBSOCKET_EVENT_QUEUE_SIZE = 1`.

The design's "reassemble, parse, push to a ring" in the callback is therefore a
**back-pressure hazard**: while the handler runs, the client task is not
reading the socket, the TCP window fills, and the server stalls. It also means
the handler must never take the LVGL lock, never call `ring_audio_write`, and
never touch flash/NVS (the `TASK_STACK_IN_EXT_RAM` Kconfig help says so
explicitly).

**Plan:** the handler does only:
1. copy the fragment (`data_ptr`, `data_len`) into a PSRAM reassembly buffer;
2. on `fin`, push the completed message pointer/length to a FreeRTOS queue;
3. handle lifecycle events (`CONNECTED` / `DISCONNECTED` / `ERROR` / `CLOSED`)
   by posting a small command to the same worker.

A dedicated `ring-ws-worker` task (internal RAM, ~6–8 KB) drains the queue and
does JSON parse, base64 decode, resampling and ring pushes. State changes and
transcripts are marshalled to the existing `ring-ws-event` task for the app
callbacks, exactly as `ring_net.c` does.

### C3. New sdkconfig options are required

The design's "No new sdkconfig options are required" is wrong. `esp_websocket_client`
ships a Kconfig menu; the relevant options are:

| Option | Default | Why it matters here |
|---|---|---|
| `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` | n | We stream uplink continuously while receiving downlink. With a single lock, `esp_websocket_client_send_text()` can block on the RX path. **Enable.** |
| `CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER` | n | RX/TX buffers are `malloc(buffer_size)` from internal RAM. Dynamic buffer allocates on demand and frees after use (~2 KB saved idle), at the cost of `realloc` churn. Consider. |
| `CONFIG_ESP_WS_CLIENT_ALLOC_IN_EXT_RAM` | n | Puts the client struct/config in PSRAM (not RX/TX buffers). Consider if internal RAM is tight. |
| `CONFIG_ESP_WS_CLIENT_TASK_STACK_IN_EXT_RAM` | n | PSRAM task stack. **Do not enable** with a heavy handler (see C2); it forbids flash/NVS in the handler. |

**Plan:** add `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y` to `sdkconfig.defaults`.
Measure `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` before/after choosing a
`buffer_size`; do not blindly raise it to 16 KB.

### C4. `buffer_size` is internal RAM

The client does `malloc(buffer_size)` for RX and TX (unless the dynamic-buffer
option is on). Raising from the 1 KB default to 8–16 KB costs **16–32 KB of
internal RAM**, which is tight on this board (LVGL, IMU, Wi-Fi, mbedTLS with
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). The design's "8–16 KB" should be "4 KB,
measured" unless the dynamic buffer is enabled.

### C5. The error shape is nested

The fallback trigger is `error.param`, not a top-level `param`:

```json
{"type":"error","error":{"type":"invalid_request_error","code":"invalid_value",
 "message":"...","param":"session.turn_detection.type"}}
```

The copied `json_get_str(json, "param", ...)` will find it (it searches the
whole string), but the plan should say so explicitly and also read
`error.type` to distinguish `invalid_request_error` (retryable) from
`server_error` (fatal).

### C6. The half-duplex gate needs module support

The design's predicate uses `downlink_buffered > 0` and `last_playback_ms`,
but the app cannot see either. Either:
- expose `ring_ws_playback_pending()` / `ring_ws_buffered_ms()`, or
- have `ring_ws` drop uplink frames internally while
  `responding || buffered > 0 || now - last_playback < 500`.

The second is cleaner (the app never sends a frame the model should not hear).
**Plan:** implement the gate inside `ring_ws_send_audio()`, and expose
`ring_ws_playback_pending()` for the UI.

### C7. M0 "dump the reply to a file/log" is a known trap

Logging raw PCM over the USB-Serial-JTAG console **blocks** when the host is
not reading (the repo's #1 gotcha) and will look like a hang. **Plan:** log
only the transcript and wire stats; if audio must be captured, write it to SD
only when `ring_sd_is_mounted()` is true, or play it through the speaker.

### C8. The ~8 KB app audio task stack is unverified

The app task also does base64 encode, JSON formatting and
`esp_websocket_client_send_text()` (which can block). **Plan:** start at
**8 KB**, log `uxTaskGetStackHighWaterMark()` after the first turn, and raise
if the headroom is under ~1 KB. Keep the `vApplicationStackOverflowHook`
pattern from `ring_net.c`.

### C9. `ring_ws` must be wired into `ringkit`

Add `src/ring_ws.c` to `components/ringkit/CMakeLists.txt` `SRCS`, add
`#include "ringkit/ring_ws.h"` to `ringkit.h`, and add `esp_websocket_client`
to `REQUIRES`. The design mentions the header but not the build wiring.

### C10. Minor API inconsistencies in the design

- `ring_ws_turn_mode()` is referenced ("Returned by `ring_ws_turn_mode()`") but
  not declared in the API list.
- `ring_ws_session_cfg_t.turn_detection` is a string; `NULL` = PTT is fine, but
  the doc should state that an unknown string is a configuration error.
- The design's `ring_ws_init()` takes a tool callback even though M4 is
  optional; keep it (the callback can be `NULL`), but do not implement the tool
  path until M4.

### C11. Do NOT define `vApplicationStackOverflowHook` in `ring_ws.c`

`ring_net.c` already defines it (strong symbol, overriding the weak FreeRTOS
hook). Both files are compiled into `ringkit`, so a second definition is a
**multiple-definition link error**. Reuse the existing hook; do not copy it.
If the hook is ever moved, move it to a shared file (e.g. `ring_board.c`) and
remove it from `ring_net.c` in the same change.

### C12. `ring_ws_start()` must own Wi-Fi + SNTP, not the app

`ring_net_start()` brings up Wi-Fi, waits for the IP and syncs the clock before
opening the peer. `ring_ws_start()` must do the same (reuse
`ring_wifi_start()` / `ring_wifi_wait_connected()` / `ring_net_sync_clock()`),
otherwise TLS certificate validation fails. The design's app flow implies this
but never states it.

---

## 3. Gaps / underweighted risks

| # | Gap | Mitigation |
|---|---|---|
| G1 | No host-side protocol probe. The design jumps straight to device M0. | Add **P0**: a ~100-line Python probe using `aiohttp` (already in `scripts/.venv`) to validate auth, event names, PCM rates, the `smart_turn_v2` fallback trigger and function-calling event names **before** any C code. |
| G2 | Reconnect is under-specified. A reconnect needs a fresh TLS handshake + `session.created` → `session.update`; the app must not assume the old session survives. | State machine owns retries; on `DISCONNECTED`/`ERROR` go to `RING_WS_ERROR`, then the app calls `ring_ws_start()` again. Log the attempt. |
| G3 | Uplink send can block. `esp_websocket_client_send_text()` with `portMAX_DELAY` on a 20 ms cadence will fall behind. | Use a short timeout (10–20 ms) and drop frames when the socket is busy; count drops in `ring_ws_stats_t`. |
| G4 | Downlink ring size is quoted as 384 KB (48 kHz mono). If we resample on receive (design §2) that is right; if we store 24 kHz and resample on read (recommended, see D2) it is **192 KB**. | Pick one and state it. Recommend storing 24 kHz and resampling on read. |
| G5 | `smart_turn_v2` rejection may not surface as `error.param == "session.turn_detection.type"`. It could be a different code, or the server may silently fall back. | P0 must probe this. The device fallback should trigger on any `error` during the first `session.update` whose `param` mentions `turn_detection`; otherwise log the raw error and let the user pick `server_vad` manually. |
| G6 | Function-calling event names are unverified against a live session. | P0 probes `response.function_call_arguments.done`; M4 stays optional until the event shape is confirmed. |
| G7 | Power/thermal: ~120 KB/s continuous + Wi-Fi + LVGL will heat the ring. | Not a blocker; note it. Keep sessions short in the demo. |
| G8 | `ring_ws` and `ring_net` will share `json_get_str`/`json_type_is`. | Copy for v1 (design is right); dedupe into `ring_json.[ch]` as a standalone change later, rebuilding 06 and 07. |

---

## 4. Decisions to lock before coding

| # | Decision | Recommendation |
|---|---|---|
| D1 | Where does the event callback do work? | Copy fragments only; parse on a worker task (C2). |
| D2 | Downlink ring rate | **Store 24 kHz mono** (192 KB / 4 s), resample to 48 kHz on read. Keeps the WS task lean and halves the ring. |
| D3 | Mic gate ownership | Inside `ring_ws_send_audio()` (C6). |
| D4 | Turn-mode fallback | At first `session.update`, on `error.param` containing `turn_detection`; retry once with `server_vad`; log the negotiated mode (C1, G5). |
| D5 | PTT toggle | Requires reconnect. Expose it as a settings option that restarts the session, not a runtime switch (C1). |
| D6 | `buffer_size` | 4 KB, measured; enable `SEPARATE_TX_LOCK` (C3, C4). |
| D7 | Resampler coefficient generation | `tools/gen_ws_resampler.py` (Kaiser sinc), commit the table in `ring_ws_resample.c`. |
| D8 | Host probe language | Python + `aiohttp` (already in `scripts/.venv`); no new dependency. |

---

## 5. Revised `ring_ws` API

```c
typedef enum {
    RING_WS_OFF = 0, RING_WS_CONNECTING, RING_WS_READY,
    RING_WS_LISTENING, RING_WS_SPEAKING, RING_WS_ERROR,
} ring_ws_state_t;

typedef struct {
    const char *voice;            /* "longanqian_v3.1"; IDLE-only */
    const char *instructions;
    const char *turn_detection;   /* "smart_turn_v2" | "server_vad" | NULL = PTT */
    bool        enable_search;
    const char *tools_json;       /* optional; M4 */
    const char *input_language;   /* optional, e.g. "zh" */
    const char *output_language;  /* optional */
    float       speech_rate;      /* 0.5..2.0; 0 = server default */
    int         max_history_turns;/* 1..50; 0 = server default */
} ring_ws_session_cfg_t;

typedef struct {
    uint32_t    bytes_up, bytes_down;
    uint32_t    turns;
    uint32_t    dropped_up;       /* frames dropped: socket busy */
    uint32_t    dropped_down;     /* samples dropped: ring overflow */
    uint32_t    ring_high_ms;     /* high-water mark of buffered downlink */
    const char *turn_mode;        /* negotiated mode, for the UI */
} ring_ws_stats_t;

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
const char *ring_ws_turn_mode(void);
esp_err_t ring_ws_get_stats(ring_ws_stats_t *out);

esp_err_t ring_ws_set_listening(bool on);
esp_err_t ring_ws_send_audio(const int16_t *pcm48k, size_t samples);
esp_err_t ring_ws_read_audio(int16_t *pcm48k, size_t samples,
                             size_t *out, uint32_t timeout_ms);
bool     ring_ws_is_speaking(void);
bool     ring_ws_playback_pending(void);   /* half-duplex gate for the UI */
uint32_t ring_ws_buffered_ms(void);

esp_err_t ring_ws_commit(void);          /* PTT: commit + response.create */
esp_err_t ring_ws_response_create(void);
esp_err_t ring_ws_cancel(void);          /* barge-in */
esp_err_t ring_ws_send_json(const char *json);
```

Changes vs the design: `set_turn_mode` removed (C1); `turn_mode` getter,
`get_stats`, `playback_pending`, `buffered_ms` added (C6, G3); session config
gains language / speech-rate / history fields (settings page).

---

## 6. Phased implementation plan

### P0 — Host-side protocol probe (½ day, **do this first**)

**Goal:** validate the protocol and the fallback trigger with zero C code.

- Add `scripts/test_qianwen_ws.py` using `aiohttp.ClientSession().ws_connect()`
  (already in `scripts/.venv`; no new dependency).
- Connect to `wss://maas.qianwenaiapi.com/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash`
  with `Authorization: Bearer <key>` (from `secrets.local` / `$QIANWEN_API_KEY`).
  If the host probe hits the same missing-intermediate TLS issue as
  `examples/06` README §5, pass `ssl=False` (protocol probe only) or the
  embedded `components/ringkit/assets/qianwen_ca.pem`.
- Log `session.created`; send `session.update` with
  `turn_detection={"type":"smart_turn_v2","threshold":0.5}`; log
  `session.updated`.
- Stream a 1–2 s 16 kHz / 16-bit mono clip (`scripts/assets/test_clip.pcm` is
  48 kHz; resample with `av.AudioResampler`, already used by the WebRTC
  script). Collect `response.audio.delta`, base64-decode, save a 24 kHz WAV,
  print the transcript and the byte counts.
- **Fallback probe:** open a second connection, send an invalid
  `turn_detection.type`, and log the full `error` object. Confirm whether
  `error.param` is `session.turn_detection.type`.
- **Function-calling probe (optional):** send a tool definition and a prompt
  that triggers it; log the event names.
- **Exit criteria:** a saved reply WAV, a printed transcript, and a captured
  error object. This is the reference for M0/M4.

### P1 — Transport skeleton (M0, ½ day)

**Goal:** `session.updated` and `state -> ready`, no audio, no UI.

- `main/idf_component.yml`: add `espressif/esp_websocket_client: "^1.8.0"`.
- `sdkconfig.defaults`: add `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y`.
- `components/ringkit/CMakeLists.txt`: add `src/ring_ws.c` and
  `esp_websocket_client` to `REQUIRES`.
- `components/ringkit/include/ringkit/ring_ws.h` + `src/ring_ws.c`:
  - WS config: `uri`, `cert_pem = ring_net_ca_pem()`, `cert_len`,
    `headers = "Authorization: Bearer <key>\r\n"` (built at runtime, never
    logged), `buffer_size = 4096`, `disable_auto_reconnect = true`,
    `task_stack = 8192`, `task_prio = 5`.
  - Event handler: fragment copy → PSRAM reassembly buffer → queue; lifecycle
    events → queue. No JSON, no base64, no LVGL, no logging of payloads.
  - `ring-ws-worker` task: drain queue, parse JSON (copy `json_get_str` /
    `json_type_is` from `ring_net.c`), handle `session.created` → send
    `session.update`; `session.updated` → READY; `error` → log + fallback;
    `response.done` → READY.
  - `ring-ws-event` task for app callbacks (state/transcript), as `ring_net`.
  - `ring_ws_start()` owns Wi-Fi + SNTP + WS (reuse `ring_wifi_*` and
    `ring_net_sync_clock()`), like `ring_net_start()`.
  - **Do not** define `vApplicationStackOverflowHook` here (C11).
  - `ring_ws_send_json()`.
- A temporary `examples/10_qianwen_ws_voice_chat/` app that only calls
  `ring_ws_init()` / `ring_ws_start()` and logs state (the real UI lands in P3).
- After editing `main/idf_component.yml`, run `rm -f sdkconfig && idf.py reconfigure`
  (per `AGENTS.md`) and confirm `esp_websocket_client` resolves from
  `managed_components/` and `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` is visible.
- **Exit criteria:** build from a clean `-B build/10_qianwen_ws_voice_chat`;
  boot log shows `session.updated`, `state -> ready`, the negotiated turn mode,
  no panic; 00 / 06 / 07 / 09 still build; vendored overrides still win.

### P2 — Audio path + one turn, **PTT mode** (M1, 1 day)

**Goal:** one end-to-end spoken turn. **Use push-to-talk (`turn_detection=NULL`)**
here, not `smart_turn_v2`: PTT gives a sequential 06-style loop (capture+send
while held, then release → commit → play reply) which is the lowest-risk way to
prove the audio + WS + resampler path. Hands-free (concurrent capture+playback)
is a different task topology and lands in P3.

- `tools/gen_ws_resampler.py`: Kaiser-windowed sinc polyphase tables
  (48k→16k decimate-by-3, 24k→48k interpolate-by-2). The **downlink
  interpolator must be streaming** (stateful, with history): `response.audio.delta`
  chunks are variable-size and not 20 ms-aligned, so the filter carries state
  across `ring_ws_read_audio()` calls. The uplink decimator is block-aligned
  (960→320) and needs no cross-call state.
- `ring_ws_send_audio()`: 48 kHz mono → 16 kHz mono → base64 →
  `{"type":"input_audio_buffer.append","audio":"..."}`. **Concurrent sends
  are safe** (`send_text` serializes on a recursive mutex), but use a short
  timeout (10–20 ms) and drop+count when the socket is busy (B-3 below).
- `ring_ws_read_audio()`: 24 kHz mono ring (192 KB / 4 s, newest-wins, **with a
  `dl_mutex` like `ring_net`**) → 48 kHz mono; `playback_pending()` /
  `buffered_ms()`. In PTT the half-duplex gate is trivial (uplink closed
  during reply), so leave the smart_turn gate for P3.
- `ring_ws_set_listening()`, `commit()` (commit + response.create),
  `response_create()`, `cancel()` (guard: only cancel when `is_speaking()`).
- App: a 06-style sequential worker — hold-to-talk: capture 48 kHz stereo →
  fold to mono → `send_audio`; release → `commit()`; play the reply → expand
  to stereo → `ring_audio_write`. Log wire stats and stack HWM.
- **Exit criteria:** one PTT turn works on the bench network; log shows
  `~32 KB/s up / ~48 KB/s down + base64`; stack HWM logged; no overflow.

### P3 — UI + turn-taking, **hands-free** (M2, 1 day)

**Goal:** the design's screen, hands-free by default. This is where the task
 topology changes from P2's single sequential worker to **concurrent capture
 + playback** (the smart_turn loop needs the mic open while the reply plays).

- **Two app tasks** (or one that round-robins every 20 ms): a capture+send task
  and a read+play task. The capture task still calls `ring_audio_read` every
  20 ms even while the model speaks (needed for the local barge-in level check,
  see B-1); `ring_ws_send_audio()` applies the half-duplex gate internally.
- Orb (level arc, green = mic / blue = assistant), rolling transcript, stats
  strip (up/down bytes/s, turn count, active mode, last `response.done` status).
  **Throttle the transcript label** to the last ~80 chars, refreshed ≤ ~10 Hz —
  `audio_transcript.delta` fires many times per second and a relayout on every
  delta thrashes LVGL.
- Default `smart_turn_v2` (no `silence_duration_ms` — it is ignored outside
  `server_vad`) with the P0-verified `server_vad` fallback at first
  `session.update` (preserve all non-`turn_detection` fields on the retry).
- Half-duplex predicate incl. the 500 ms tail, internal to `ring_ws_send_audio()`:
  drop uplink while `responding || buffered>0 || now-last_playback<500`.
- **Barge-in is CLIENT-side, not `speech_started`** (B-1): the mute gate keeps
  the server from hearing the user during playback, so `speech_started` cannot
  fire. Detect barge-in from local mic energy (the capture task already has the
  level): if energy > threshold while `playback_pending()`, call
  `ring_ws_cancel()` + flush the downlink ring + send `input_audio_buffer.clear`;
  the gate then reopens as `responding` clears. Tap also calls `cancel()`.
- **Playback cushion** (biggest voice-quality win): `ring_ws_read_audio()` returns
  TIMEOUT until `buffered_ms >= ~200 ms`, then drains normally — absorbs the
  bursty arrival of `response.audio.delta` so playback doesn't stutter on a slow
  link. Make the cushion a constant (not per-call).
- **Session watchdog**: if `responding` stays true > 15 s, force `cancel()` + go
  to ERROR (a stuck `response.done` would otherwise mute the mic forever).
- **Exit criteria:** human check of voice quality, turn-taking and barge-in;
  the log proves the pipeline, not the conversation.

### P4 — Settings + PTT (M3, ½ day)

**Goal:** the design's settings page, with the reconnect constraint.

- Settings: voice, input/output language, `speech_rate`, `instructions`,
  turn mode.
- Changing voice or turn mode calls `ring_ws_stop()` + `ring_ws_start()` with
  the new `ring_ws_session_cfg_t`; the UI shows "reconnecting…".
- Long-press = PTT: restart the session in PTT mode, then hold to talk.
- **Exit criteria:** human check; the UI shows the negotiated mode after a
  fallback.

### P5 — Optional (M4 / M5)

- **M4:** function calling (`get_ring_status`: battery %, charging, voltage,
  tilt, IMU temp) via `session.tools`; on
  `response.function_call_arguments.done`, run the ringkit APIs, write a
  `function_call_output` item, then `response.create`. Only after P0 confirms
  the event shape.
- **M4:** `ring_json.[ch]` dedupe as a standalone change (drop the static
  copies in `ring_net.c`, rebuild 06 and 07).
- **M5:** `ring_voice_*` facade over `ring_ws` + `ring_net` for A/B.

---

## 7. Files touched

| File | Change |
|---|---|
| `main/idf_component.yml` | add `espressif/esp_websocket_client: "^1.8.0"` |
| `sdkconfig.defaults` | add `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y` |
| `components/ringkit/CMakeLists.txt` | add `src/ring_ws.c`, `esp_websocket_client` |
| `components/ringkit/include/ringkit/ring_ws.h` | new |
| `components/ringkit/src/ring_ws.c` | new |
| `components/ringkit/src/ring_ws_resample.c` | new (generated tables) |
| `components/ringkit/include/ringkit/ringkit.h` | include `ring_ws.h` |
| `tools/gen_ws_resampler.py` | new |
| `scripts/test_qianwen_ws.py` | new (P0 probe) |
| `examples/10_qianwen_ws_voice_chat/` | new app (3 files + README) |
| `examples/README.md`, `AGENTS.md` | mark 10 as implemented when done |
| `notes/design-ws-voice-chat.md` | optional: fold in C1–C12 as a revision note |

---

## 8. Verification matrix

| Level | Check |
|---|---|
| Build | Clean `-B` dirs for `00`, `06`, `07`, `09`, `10`; vendored overrides win; `esp_websocket_client` resolves from `managed_components/` |
| Size | `idf.py -B build/10_qianwen_ws_voice_chat size` vs 06's 282 KB headroom; **also measure app 00's delta** from adding the WS client to ringkit (see B-6) |
| Boot | `ring_ws: session.updated`, `state -> ready`, negotiated mode logged, no panic |
| Network | Full turn on the UDP-blocked bench (the key advantage) |
| Wire | On-screen up/down bytes/s ≈ 32/48 KB/s + base64 |
| Audio | Mic → model → speaker, no echo/answer-itself, barge-in works |
| Memory | Stack HWM logged for `ring-ws-worker`, `ring-ws-event`, **app capture task**, **app playback task**, and the **WS client task at `WEBSOCKET_EVENT_CONNECTED`** (the TLS handshake runs there — B-4) |
| Human | Voice quality, turn-taking, barge-in, (M4) function calling |

---

## 9. Self-review: bugs in this plan (found on re-read)

These are bugs in the **plan/design as written**, not in the platform. Each is
fixed in the phases/decisions above; recorded here so the reasoning survives.

### B-1 (critical). Barge-in via `speech_started` is incompatible with the mute gate

The design and the first cut of this plan said barge-in fires on
`input_audio_buffer.speech_started`. But the docs define that event as VAD
detecting speech **in the received audio**, and the half-duplex gate drops the
uplink while the model speaks (`responding || buffered>0 || tail<500ms`). So
the server receives silence during playback and can never fire `speech_started`.
A server-side barge-in and a full-mute gate are mutually exclusive.

The ring has no AEC, so you cannot have both echo-free playback and
server-heard barge-in. **Fix (folded into P3): barge-in is client-side** — the
capture task already reads the mic every 20 ms; if its level exceeds a
threshold while `playback_pending()`, call `ring_ws_cancel()` + flush the
downlink ring + send `input_audio_buffer.clear`; the gate then reopens as
`responding` clears. A soft alternative (unmute briefly on loud local speech so
the server can fire `speech_started`) is more fragile and needs voiceprint to
avoid the echo retrigger — leave it for a future full-duplex/AEC effort.

### B-2 (critical). P2's sequential loop is not P3's hands-free

The first cut of P2 described a "minimal audio loop — capture, send; play the
reply", which is 06's **sequential** worker (talk, then reply). Hands-free
(`smart_turn_v2`) needs the mic open **while** the reply plays — concurrent
capture+playback. Building the sequential loop in P2 then turning it concurrent
in P3 is rework. **Fix (folded into P2/P3): P2 ships in PTT mode**
(`turn_detection=NULL`), where the sequential loop is correct and the turn
boundary is explicit (`commit`); P3 is where the topology goes concurrent and
switches to `smart_turn_v2`. P2 is now the low-risk proof of the audio+WS+resampler
path; P3 isolates the genuinely hard part (half-duplex + barge-in).

### B-3. Reconnect must teardown before re-init

`ring_ws_start()` on a live client leaks (a second `esp_websocket_client_init`
without `stop`/`destroy` of the first). With `disable_auto_reconnect=true` the
WS task stops itself on error, but the handle and its transport/buffers remain.
**Fix (P1, also applies to the P4 settings restart):** `ring_ws_start()` must, if
`s.client != NULL`, `esp_websocket_client_stop()` + `esp_websocket_client_destroy()`
and NULL the handle before re-initialising — mirror `ring_net_start()`'s
`esp_peer_disconnect`/`esp_peer_close` before `peer_open()`.

### B-4. The 8 KB WS task stack needs a different anchor

The plan justified `task_stack = 8192` by analogy to `ring_net`'s 16 KB peer
task, but that 16 KB is for **DTLS** (esp_peer). The WS client runs **TCP TLS**
(esp-tls) in its own task — lighter, but still a real handshake. The default
4 KB usually works for `wss://`, so 8 KB is probably generous, but it is
**unmeasured**. **Fix (P1): log `uxTaskGetStackHighWaterMark` of the WS task
right after `WEBSOCKET_EVENT_CONNECTED`** (handshake complete) and trim/raise
from evidence, not by analogy.

### B-5. The downlink interpolator must be streaming

`response.audio.delta` base64 decodes to **variable-size** 24 kHz chunks (not
20 ms-aligned). A block polyphase filter that assumes fixed input size will
click at chunk boundaries. **Fix (P2): the 24k→48k interpolator holds filter
history across calls** (state + overlap buffer); only the 48k→16k decimator is
block-aligned (960→320) and stateless. `tools/gen_ws_resampler.py` must emit a
streaming API (`interp_process(in, n_in, out, &n_out)`), not a fixed-frame one.

### B-6. Reassembly: pre-allocate, filter by op_code

Doing `heap_caps_malloc` on every `WEBSOCKET_EVENT_DATA` fragment on the WS
task is needless churn, and the plan didn't say to ignore non-text frames.
**Fix (P1): pre-allocate one PSRAM reassembly buffer at `ring_ws_init()`
(~64 KB, per design §9); accumulate `data_len` at `payload_offset`; only act on
`op_code == 1` (text) frames; on `fin && payload_offset+data_len>=payload_len`,
push the message to the worker queue and reset the buffer. Drop+log oversized
messages.**

### B-7. `response.cancel` must be guarded

Cancelling when no response is in progress returns an `invalid_request_error`
("Cannot create response while another response is in progress" is the inverse;
cancelling with nothing running is the symmetric reject). **Fix (P3):**
`ring_ws_cancel()` is a no-op unless `is_speaking()` / `responding` is true.

### B-8. The fallback must preserve the rest of the config

D4 says "retry once with `server_vad`". If the first `session.update` carried
`voice`/`tools`/`enable_search`/languages, the retry must keep all of them and
swap only `turn_detection`. **Fix (P1): the retry reuses the same
`ring_ws_session_cfg_t`, replacing only the turn-detection field.**

### B-9 (minor). `speech_rate = 0` as "server default" is out of range

The valid range is `[0.5, 2.0]`; 0 is not. Using 0 as a sentinel works (just
means "don't include the field") but is slightly confusing. Prefer not sending
the field when unset, or use `-1.0` as the sentinel. Cosmetic.

---

## 10. Improvements (brainstorm)

* **Playback cushion (P3).** Start playback only after ~200 ms of downlink is
  buffered. `response.audio.delta` arrives in bursts; playing at delivery rate
  stutters on a slow link. A cushion turns the 4 s newest-wins ring from a cap
  into an actual jitter buffer. Single biggest voice-quality win.
* **Uplink micro-queue.** G3 drops a 20 ms frame the instant `send_text` can't
  take the lock — that is lost user speech. A 1–2 frame uplink queue lets the
  audio task never block and only drop when the network is genuinely backed up.
  Pairs with the cushion: drop uplink gracefully, cushion the downlink.
* **Session watchdog (P3).** If `responding` stays true > 15 s with no
  `response.done`, force `cancel()` → ERROR. Without it a lost `response.done`
  mutes the mic forever (the gate never reopens).
* **Transcript throttle.** Cap the transcript label to the last ~80 chars and
  refresh ≤ ~10 Hz. `audio_transcript.delta` fires dozens of times per second;
  a full LVGL relayout on each thrashes the render task on a 40-line buffer.
* **`input_audio_buffer.clear` on barge-in.** When the user barges in mid-reply,
  clear the uncommitted buffer so the model doesn't process the partial audio
  that was in flight. Send it right after `response.cancel`.
* **Don't send `silence_duration_ms` with `smart_turn_v2`.** The docs say it is
  ignored outside `server_vad`; sending it is harmless but misleading. Send it
  only on the fallback retry.
* **Measure the cost to all apps of putting the WS client in ringkit.**
  AGENTS.md warns that a `ringkit` REQUIRES change grows every image. The repo
  precedent (`ring_net` already pulls `esp_peer`+Opus into all apps) says accept
  it, but the WS client (~small, reuses `esp-tls`) should be **measured on app
  00** and the number recorded. If the delta is large, the alternative is a
  guarded compile (`RING_BUILD_WS` Kconfig) so non-voice apps don't link it.
* **Concurrent sends are safe — document it.** `esp_websocket_client_send_text`
  serialises on a recursive mutex (`client->lock`, or `tx_lock` with
  `SEPARATE_TX_LOCK`). So worker + audio task can both call it; the audio task
  just has to use a short timeout. This is *why* C3 (`SEPARATE_TX_LOCK`) matters:
it lets RX (downlink) keep running while we send uplink, instead of our uplink
  blocking the WS task's receive loop.
* **Stats reset.** `ring_ws_get_stats()` should offer a reset (or reset per turn)
  so `ring_high_ms` is interpretable.
* **Future full-duplex = AEC.** The only way to have server-side barge-in and
  echo-free playback at once is acoustic echo cancellation. ESP-IDF's `esp-sr`
  AFE has AEC. That is a large addition (M5+) and is the real fix for B-1; the
  client-side energy detector is the pragmatic half-duplex substitute.

---

## 11. Open questions

1. Does `smart_turn_v2` rejection surface as `error.param ==
   "session.turn_detection.type"`? (P0 answers this; B-8 keeps the fallback
   robust to a different `param`.)
2. Is the 500 ms echo-suppression tail enough on this hardware, or does the
   ring still hear itself? (P3 human check.)
3. Is 4 KB `buffer_size` enough, or does the audio stutter? (P2 measure; the
   P3 cushion is the real fix.)
4. Does the `ring_ws` worker task need to be pinned to a core to avoid
   starving the WS task? (Measure; `tskNO_AFFINITY` first.)
5. Should `CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER` be on? (Measure
   internal heap with it off first.)
6. What is the right barge-in energy threshold on this mic/speaker combo, and
   does the speaker's own output cross it? (P3 human check — this is the live
   part of B-1.)
