# Code review — `ring_geodis` (GEODIS / Azure Realtime transport)

**Date:** 2026-11-12, against `b7e0065`.
**Scope:** `components/ringkit/{include/ringkit/ring_geodis.h, src/ring_geodis*.{c,h}, src/ring_stack_hook.c, src/ring_http.c, src/ring_json.c, src/ring_byte_ring.c}`, plus the consumers (`examples/12_ringkit_selftest`) and the plan note.
**Not in scope:** `scripts/test_geodis_api.py` (host spike, reviewed only where the firmware must match it).
**Method:** static read; no device run (the module has never executed on hardware — see §7 of the plan, and the missing app below).

**Headline:** the transport is a faithful adaptation of `ring_net` and the Phase-0 findings are correctly encoded. The problems are not in the protocol — they are in the **lifecycle and the callback contract**, i.e. exactly the surfaces the app author will lean on. Three of them will produce a hard crash or a silently wrong answer, not a log line.

**Status (2026-11-12):** §1.1–§1.4, §2.1–§2.4 and the barge-in/uplink half of
§3.1–§3.2 are implemented — see `notes/geodis-push-to-talk-plan.md` §6A.5 for the
contract changes. §5 (payload builders + host tests) and §6.2 (mock transport)
are now done as well — see §5 and §6.2 below. Everything from §3.3 onward is
open except where noted. Nothing here has run on hardware: the ring is not
attached in the current environment (`/dev/ttyACM0` absent), so the fixes are
compile-verified (both `RINGKIT_GEODIS_MOCK=n` and `=y` link cleanly) plus host
tests.


---

## 0. The missing piece: there is no example

`examples/14_geodis_voice_chat/README.md` was deleted in `e74209e` ("drop the planned … example READMEs"), and the app itself never existed. So: <!-- docs-ignore -->

- `components/ringkit/README.md:47` still points at **`examples/14`** — a phantom.
- `notes/geodis-push-to-talk-plan.md` §5 specifies `examples/14_geodis_push_to_talk` as if it were next.
- Nothing has ever called `ring_geodis_start()` except the selftest's `init`/`stop`.

That is why the findings below matter now: they are the defaults the app will inherit on its first day. Everything is graded by "what happens when example 14 is written against this header".

---

## 1. P0 — will crash, hang, or lie

### 1.1 `text_cb` / `tool_cb` run on the **peer** task, and the header says they don't

`ring_geodis.h:74-84` — "invoked on the ring_geodis event task". `ring_geodis.h:99-101` — "The callbacks run on a dedicated task, never on a peer thread, **so they may take the LVGL lock**."

Reality: `ring_geodis_protocol.c:179-224` calls `text_cb` and `tool_cb` directly from `ring_geodis_on_data_frame()`, which is an `esp_peer` callback on `ring-geodis-peer` (16 KB stack, also running `esp_peer_main_loop` → DTLS/mbedTLS). Only `state_cb` goes through `event_task` (`ring_geodis.c:98-108`).

An app author who follows the header will render a transcript label **under the LVGL lock on the DTLS thread**. Two failure modes, both nasty:

- **Stack**: LVGL label re-layout with the 48 px font on a 16 KB stack that is mid-DTLS. The shared overflow hook (`ring_stack_hook.c`) aborts — you get a reboot mid-conversation, and the log only says which task died.
- **Stall / deadlock**: the peer task is the only thing calling `esp_peer_main_loop`. Taking the LVGL lock there means every UI repaint (QSPI flush, ~40 lines) blocks RTP receive; if the UI task is already waiting on something the peer task owes it, it deadlocks.

The comment inside `ring_geodis_protocol.c:215-216` ("It runs on the peer task, so it must not take the LVGL lock") is the truth; the header is wrong. Two ways out, pick one and make the other impossible:

1. **Preferred** — queue the events. `event_task` already exists; widen the queue to a small struct `{kind, is_user, final, len, text[..]}` (128 B × 8 = 1 KB internal) and let `on_data_frame` only `xQueueSend(…, 0)`. Then the header becomes true, and the app can render.
2. Cheap — keep the behaviour, fix the header, rename the callback doc to "peer task; copy the text and notify your UI task". Then example 14 needs its own forwarding queue, i.e. every consumer re-implements (1) badly.

Do (1). It also fixes 1.2.

### 1.2 `event_task` is lossy by construction

`ring_geodis.c:76-81` + `98-108`: `xQueueOverwrite` on a 1-deep queue, `event_task` wakes and **peeks** the latest value. States coalesce. A `READY → LISTENING → SPEAKING` burst delivers only `SPEAKING`; `READY` on release followed immediately by a new `LISTENING` delivers neither.

For a UI that prints a stage line this is visibly wrong ("listening…" never appears on a fast turn). For anything edge-triggered (e.g. "flush the speaker ring when the state becomes SPEAKING", "start the reply timer") it is a correctness bug. Send events, don't overwrite a state cell: keep `ring_geodis_state()` as the polled truth for the current value, and make the queue carry *transitions*.

### 1.3 The peer handle is freed while another task may be inside it

`ring_geodis.c:279-288` (`stop`) and `ring_geodis.c:201-205` (retry path at the top of `start`) do:

```c
esp_peer_disconnect(peer); esp_peer_close(peer); peer = NULL;
```

from the **caller's** task, while `ring-geodis-peer` may be inside `esp_peer_main_loop(peer)` (`ring_geodis_signaling.c:220-229` reads `ring_geodis_s.peer`, then calls in — a TOCTOU on a pointer that `stop()` frees). `esp_peer` documents no thread safety ("default peer realization have no extra thread internally, all is triggered in this loop", `esp_peer.h:647`) and has no internal lock. This is a use-after-free of a struct owning DTLS/SRTP sockets.

It is exactly the path the plan cares about: **the gateway resets ~1 in 12 connections**, so the app *will* call `stop()` + `start()` on a live peer within the first minute of testing. `ring_net` has the identical bug (`ring_net.c:266-275`) — 07 has simply been lucky or never retried.

Fix: make the peer task own the lifetime. `stop()` sets `stop_requested` + notifies; the peer task breaks the loop, calls `esp_peer_close()` itself, clears the handle, and signals completion (a semaphore `stop()` waits on, 2 s). Same for the retry path.

### 1.4 `start()` and the signal task disagree about timeouts → duplicate sessions

`ring_geodis.c:258-274` waits at most `RING_GEODIS_SESSION_TIMEOUT_MS` (20 s) for a non-CONNECTING state. The signal task's SDP POST retries 3× at `RING_GEODIS_HTTP_TIMEOUT_MS` (10 s) with 200/400 ms backoff — worst case **> 30 s** before it either installs the answer or sets ERROR.

So on a slow / flaky gateway: `start()` returns `ESP_ERR_TIMEOUT` → the app shows "error, retrying" → calls `start()` again → the second call kills the peer (1.3) while the *first* signal task is still blocked in its POST, then notifies the signal task for a *second* offer — the task cannot tell generations apart, so it may install the first answer over the second peer, or post the first offer twice.

Fix: (a) a `session_gen` counter stamped into each signalling job and checked before/after the POST; (b) let the signal task be the authority on failure — it sets `ERROR` and `start()` returns as soon as the state leaves CONNECTING, with the 20 s deadline only as a backstop; (c) size the deadline ≥ worst-case retry budget, or run the retries on a session-bounded budget (e.g. 2 × 6 s).

`ring_geodis_signaling.c:258` also calls `esp_peer_send_msg(ring_geodis_s.peer, …)` without a NULL check. On the `stop()`-during-POST path above, `peer` is NULL → `esp_peer_send_msg(NULL, …)` returns `ESP_PEER_ERR_INVALID_ARG` (it checks), so it degrades to a log — but the state machine then reports ERROR for a session that was deliberately stopped.

---

## 2. P1 — silently wrong answers

### 2.1 The cfg struct cannot express its documented defaults

`ring_geodis.h:66-67` — `enable_stt` / `enable_ptt` "(default true)". `ring_geodis.c:191-192` copies them verbatim. A caller writing the natural

```c
ring_geodis_cfg_t cfg = { .base_url = …, .sdp_url = … };
```

gets **STT off and PTT off** — no user transcript, and `turn_detection` left at server VAD, so `ring_geodis_commit()` sends `input_audio_buffer.commit` against a server that has already auto-committed. The header's other two "NULL = default" fields are just as broken: `ring_geodis.h:62-63` says `NULL = default`, `ring_geodis.c:181` returns `ESP_ERR_INVALID_ARG` for NULL.

All three are the same defect: **the struct has no way to say "unset"**. Pick a convention and apply it to all five fields — either `NULL`/`-1` = default (`int8_t enable_stt` with `-1`), or a `RING_GEODIS_CFG_DEFAULT()` initialiser macro plus "all fields explicit" in the doc, and drop the word "default" from the field comments. The macro version is less code and cannot rot.

### 2.2 A truncated `arguments` string becomes a *successful* empty query

`ring_geodis_protocol.c:154` copies the frame into `char json[1024]`, truncating. `ring_json_get_str` correctly returns false on an unterminated string (`ring_json.c:136`), so line 205 leaves `job.args_json` empty — and line 212 then substitutes `"{}"`. Lines 222-228 queue the job anyway, so the module POSTs `/tool-call` with `{"arguments":"{}"}` and feeds whatever comes back to the model.

The user hears a confident answer about the wrong thing (or "no shipment found") with no error anywhere. Same silent path for any parse failure.

Fix: distinguish "field absent" from "field unparseable". If `arguments` is present in the frame but extraction failed → do **not** call the tool; send `function_call_output` with `{"error":"arguments_truncated"}` (the model then says it cannot look it up) and log at ERROR. Also raise the frame buffer to 2048 and only allocate the big copy when `frame->size >= sizeof(json)`.

### 2.3 The tool result can be silently truncated into invalid JSON

Three caps, none of which is checked:

- `ring_geodis_protocol.c:304` extracts into `inner` (`RING_GEODIS_TOOL_INNER_CAP` 16 KB). `ring_json_get_str` **silently truncates** when the value does not fit (`ring_json.c:43,121,131` — `if (n + 1 < cap)`), then returns `true`. A 6-chunk answer is ~9 KB, 12 chunks passes 16 KB.
- `ring_json_read_body` caps the raw response at 32 KB (`ring_geodis_signaling.c:92`), so `text` can be cut there first — then `get_str` fails on the unterminated string and the code falls to line 311, which reports **`HTTP 200`** as the failure ("tool-call failed: HTTP 200"). That message sends the next debugger in the wrong direction.
- `ring_json_append` calls everywhere ignore the return value, so any overflow produces syntactically-broken JSON that is still sent.

Fix: make `ring_json_get_str` report truncation (`out_len` may exceed what fit, or add a `bool *truncated`), treat it as an error, and put the actual reason in the fallback JSON (`"parse_error"` vs `"http_%d"`). Cheapest version: have `get_str` set `*out_len = n` *and* return false when `n + 1 == cap` was hit — no API change, one caller to adapt.

### 2.4 The `/token` body can overflow for a long prompt

`ring_geodis_signaling.c:143` — `char body[768]`; `RING_GEODIS_PROMPT_MAX` is 512 (`priv.h:70`). JSON-escaping can double a French prompt (`"` → `\"`, newlines → `\n`); 512 chars of escaped text + the id wrapper exceeds 768, `ring_json_append` fails (`ring_json.c:268-271` sets `pos = cap`), every later append no-ops, and the POST carries **truncated JSON** → HTTP 400 with a log that says only `token: HTTP 400`.

Fix: `char body[RING_GEODIS_PROMPT_MAX * 2 + 256]` and check the last append; on failure return `ESP_ERR_INVALID_SIZE` before the POST.

---

## 3. P2 — behaviour the UI will notice

### 3.1 Barge-in is not implemented, only advertised

`ring_geodis_cancel()` (`protocol.c:262-265`) sends `response.cancel` and nothing else. The 384 KB downlink ring still holds up to 4 s of the reply, and the app keeps draining it → the model is cancelled but the ring keeps talking.

`ring_geodis_set_listening(true)` (`ring_geodis.c:302-317`) does not cancel the active response and does not flush the ring either. So the natural PTT gesture — press while the reply is playing — produces: mic open + speaker still playing the old reply + DTLS uplink. The mic hears the speaker, the model hears itself, and you get a loop. `ring_log`/10's example worked because the app muted/unmuted around playback; that trick only holds if the app owns the ring, and it does.

Fix in the module, where it belongs: on `set_listening(true)` → if `speaking`, `response.cancel` + clear `speaking`; on `cancel()` → `ring_byte_ring_reset(&dl_ring)` (it already takes the mutex). Both are three lines and remove a class of app bugs.

### 3.2 The first audio frame can kill the uplink mid-hold

`ring_geodis_codec.c:148-154` sets `SPEAKING` on *every* received frame — including while the user is holding the button (`LISTENING`). `ring_geodis_send_audio` then returns `ESP_ERR_INVALID_STATE` (`ring_geodis.c:327`) and the app's audio loop stops feeding. With server-VAD fallback this is guaranteed: the model starts answering after ~500 ms of silence, mid-hold.

The state enum conflates two independent facts: **"is the uplink open"** and **"what is the model doing"**. Fix: keep a separate `bool uplink_open` set by `set_listening()` and gate `send_audio` on that; let the state enum be a UI hint only. (Then `LISTENING` and `SPEAKING` can legitimately overlap, which they physically do.)

### 3.3 `response.created` is ignored

The state only enters `SPEAKING` when the first **audio** frame arrives (`codec.c:148`). A reply that is text-only, or a RAG round trip that takes 2 s, leaves the stage line on `READY`/`waiting…` with no feedback. Handle `response.created` → `SPEAKING` (the plan's §4.4 table already lists `response.output_audio_transcript.delta` as a `SPEAKING` trigger; the implementation doesn't do that either).

### 3.4 Debug logging is `INFO`, and it prints the conversation

`ring_geodis_protocol.c:161` logs every DataChannel frame at INFO, truncated to 200 chars. That is: every transcript delta, the user's spoken question, the RAG query and the tool result — on a USB-CDC console at 115200, where writes **block when the host stops reading** (AGENTS.md). Two problems: one person's shipment status in the log, and console back-pressure inside the peer task. Demote to `ESP_LOGD`, keep a one-line summary at INFO.

Same for `ring_geodis_signaling.c:224-227` — `peer task alive (hwm N words)` every 10 s forever. Demote to DEBUG; you only want it while sizing stacks (which is done: 16 KB, measured).

### 3.5 `keepalive()` probably cannot do what it claims

`protocol.c:267-275` re-sends the full `session.update`. Azure rejects `session.update` while a response is active, and re-sending the update *resets* the audio config. Meanwhile the idle-session timeout is undocumented, and the `ek_` token isn't what keeps the SDP session alive — it was spent at the SDP exchange. So the function as written can only cause harm.

Either drop it from the public API until measured, or implement it as the app-visible "reconnect after idle" (stop + start) and call it `ring_geodis_reconnect()`.

### 3.6 Smaller ones

| Where | Issue |
|---|---|
| `ring_geodis.c:136` | `memset(&ring_geodis_s, …)` in `init()`. A second `init()` leaks the Opus handles, 432 KB of PSRAM, four semaphores and three tasks. Add an `initialised` guard (`if already → ESP_OK`) or a real `deinit()`. |
| `ring_geodis.c:166-173` | Three tasks (4 + 6 + 12 KB internal) plus 48 KB PSRAM scratch and a 384 KB PSRAM ring are allocated at `init()` even if the app never starts a session. `12_ringkit_selftest` holds 22 KB of idle stacks for a probe that only tests `init`. Create them in `start()`, delete in `stop()`. |
| `ring_geodis.c:84-87` | `now_ms()` wraps at 49.7 days; `now_ms() >= deadline` then fails open. Compare with a signed difference. |
| `priv.h:103` | `send_stream_id` written by the peer task, read by the app task; `uint16_t` is not atomic in the C sense. A stale id after reconnect sends on a closed stream. Guard it with the existing `sdp_mutex` or make it `_Atomic uint16_t`. |
| `ring_geodis.c:319-329` | `pts` is not reset on a retry mid-call (it is at `start`, `:207`) — fine — but `send_audio` does not reset it after `commit()`, so a second turn's timestamps continue from the first. That is correct for one RTP stream; note it so nobody "fixes" it. |
| `ring_geodis_signaling.c:59-118` | A fresh `esp_http_client` (full TLS handshake, no session reuse) per attempt, and the retry loop rebuilds it. With 3 retries on a flaky gateway that is 3 handshakes. Acceptable, but `esp_http_client` supports keep-alive; the tool call and the token call could share a client. |
| `ring_geodis.c:181` | `start()` doesn't verify `init()` ran (no `initialised` flag): with `base_url == NULL` it builds the URL `"/token"`, `esp_http_client_init` fails, and the error surfaces as `ESP_ERR_NO_MEM` (`signaling.c:66-68`). Return `ESP_ERR_INVALID_STATE` instead. |

---

## 4. Structure

The module is a fork of `ring_net`: `ring_geodis_codec.c` is `ring_net_codec.c` with a rename, the peer task, the state cell, the PSRAM ring and the PTT semantics are all copies. The fork bought a working 07, which was the right call at the time. Two things have changed:

1. **There are now two WebRTC transports.** Every fix above (§1.1, §1.3, §3.2, §3.6) has to be made twice, and `ring_net` has the same latent bugs today — 07 has just never retried a session.
2. **The differences are now precisely enumerable** (signalling shape, CA, session JSON, event names, tool call). That is a protocol adapter, not a transport.

The plan's "extract `ring_rtc_core` later" should be pulled forward *before* example 14 is written, otherwise example 14 will encode the broken semantics and the refactor will have to preserve them. Suggested split:

```
ring_rtc_core.c   peer open/main loop/ownership, task lifecycle, peer-mutex,
                  Opus encode/decode, downlink ring, uplink-open flag,
                  event queue (transitions, not a state cell), PTT/commit
ring_net_proto.c  Qianwen signalling + session.update + transcript events
ring_geodis_proto.c  token + SDP + Azure schema + tool call
```

If that is too much before the demo, the minimum is: fix §1.1, §1.2, §1.3 **in `ring_net` as well** so the two modules don't diverge further.

Two smaller structural notes:

- **`ring_geodis_transcript_cb_t` has no `final` flag**, while `ring_ws_transcript_cb_t` got one in `b9f90b7` (`ring_ws.h:86-87`). The geodis callback delivers deltas and finals through the same signature (`is_user` true = complete utterance, false = delta *or* final), so a UI cannot tell when to stop appending or when to commit the line to scrollback. Add `bool final` — `.delta` → false, `.done` → true — and match `ring_ws`. This is the kind of asymmetry that gets cemented by the first consumer.
- **`ring_geodis.h` is where the app author will look, and it is currently the least accurate file.** Every defect in §2.1 and §3.2 is a header claim the implementation does not honour. After the fixes, add one line per callback stating the task and the max stack you may use there.

---

## 5. Testing

**Done (2026-11-12).** The three builders now live in
`components/ringkit/src/ring_geodis_payload.c` behind `ring_geodis_payload.h`,
as pure functions (no globals, no FreeRTOS — `ring_json` is the only
dependency), and `tests/test_ring_geodis_payload.c` asserts their exact bytes:
62 checks, wired into `make check`.

| builder | what the test pins |
|---|---|
| `ring_geodis_payload_session_update()` | never emits a top-level `modalities`/`turn_detection`; exact JSON for all four flag combinations; `"input"` omitted entirely when both flags are off; `turn_detection: null` present (not omitted) when PTT is on |
| `ring_geodis_payload_token_body()` | empty prompt is omitted rather than sent as `""`; a 512-byte prompt of pure `"` (2× escaped) still fits and closes |
| `ring_geodis_payload_tool_body()` | `arguments` is a **string**, never spliced raw; quotes/backslashes/UTF-8 survive; NULLs never become `null` |
| `ring_geodis_payload_tool_output()` | oversized output degrades to a closed `lookup_unavailable` item, not truncated JSON |

The prompt-limit constant is duplicated in the test (it cannot include
`ring_geodis_priv.h`), so `test_capacity_constants()` guards the copy.

Everything else (peer lifecycle, state transitions, tool ordering) is covered
by the mock in §6.2, not by unit tests.

---

## 6. Brainstorm — what to build next, in value order

### 6.1 The missing app: build it, but make it *the face*, and be honest about flash

`14_geodis_push_to_talk` as specified in the plan is a text UI: title, HOLD TO TALK circle, stage line, two transcripts. It is the right *test harness* and a weak *demo*. The demo this hardware wants is the one already in the repo: `01_gbuddy_face` is titled "the GEODIS buddy face" — four expressions, blinking, gaze, sleep on hold. A face that listens, looks up, and answers is the product; a text screen is the debug view.

But the arithmetic says you cannot just merge them:

| | bytes |
|---|---|
| `00_glyph_grid_reveal` (base: ringkit + LVGL + Wi-Fi) | 750,672 |
| `07_qianwen_realtime_harness` | 2,704,928 |
| WebRTC/Opus/cert-bundle delta | **1,954,256** |
| `01_gbuddy_face` (assets 424,200; size from its README) | 1,153,600 |
| 01 + delta | **3,107,856** |
| `factory` partition | 3,072,000 (3000K) |

**~36 KB over.** Three ways out, in order of preference:

1. **Raise the partition to 3400K** — 16 MB of flash, no OTA, `partitions.csv` is already custom. One line, zero product cost, keeps all four moods.
2. **Cut to two moods** (happy + joy): assets 424,200 → 159,000, i.e. −265 KB → fits with ~229 KB spare. Costs the sad/surprised expressions, which are exactly the ones a "shipment delayed" answer wants.
3. Strip `whisper-1` STT and show no user transcript — no, that is a feature, not a byte.

Recommendation: **two apps, one codebase**. `14_geodis_push_to_talk` stays the bench harness (text UI, NVS endpoint override, state/latency dump, used for §7 verification and for anyone without the sprite assets). Then `15_geodis_face` reuses `ring_geodis` + the 01 sprite layer, with the partition at 3400K. The harness is what you debug with; the face is what you show.

Either way, the app must not re-derive what the module should own: uplink-open, ring flush on barge-in, and transcript finality all belong in `ringkit` (§1.1, §3.1, §4).

### 6.2 A mock transport — the biggest unblock

**Done (2026-11-12).** `CONFIG_RINGKIT_GEODIS_MOCK` (default n) replaces the
network half of the module and nothing else:

| file | role |
|---|---|
| `src/ring_geodis_mock.c` | `peer_open/close/connect/disconnect`, `fetch_token`, `signal_task`, the HTTPS POSTs, and the `transport_on_event` hook — the symbols `ring_geodis_signaling.c` would have provided |
| `src/ring_geodis_mock.h` | `ring_geodis_mock_config()` timing overrides (connect / reply / tool delays, or drop the audio) |
| `assets/geodis_mock_reply.pcm` | 1.6 s of 48 kHz mono from `scripts/reply.wav`, peak-normalised; embedded with `EMBED_TXTFILES` (150 KB of flash, no generated C array) |

Everything above the transport is the real code — the same state machine, the
same event queue, the same downlink ring, the same tool task, the same public
API — so an app developed against the mock is the app that runs against the
gateway. Behaviour:

```
start()              CONNECTING -> READY          ~300 ms
set_listening(true)  LISTENING
commit()             user transcript (final) -> SPEAKING
                     "Je vérifie votre colis…" delta -> tool call
                     1.6 s of canned PCM, one 20 ms frame at a time -> READY
cancel()             playback stops at the next frame boundary (barge-in)
```

The `/tool-call` POST is offline by design and returns `ESP_FAIL`, which
exercises the real §2.2/§6.5 failure path: the model is told
`lookup_unavailable` instead of getting a fabricated answer. That is a feature —
it is the one code path most likely to be wrong and the hardest to provoke
against a healthy gateway.

Turning it on:

```
# sdkconfig
CONFIG_RINGKIT_GEODIS_MOCK=y
```

Both configurations link cleanly. The compile-time switch is a `#ifdef` in
`ring_geodis.c` (skip Wi-Fi/SNTP), `ring_geodis_codec.c` (`uplink_send` counts
and returns OK), and the CMake source list (signalling **or** mock, never both).
`ring_geodis_protocol.c` gains a `transport_on_event()` no-op, which is how the
mock learns about commit/cancel without duplicating `send_event()`.


### 6.3 Make latency measurable, not felt

The perceived latency budget is `commit → response.create → RAG POST (1–2 s) → first audio frame`. Right now there is not one timestamp in the module. Add `ring_geodis_stats_t` — `session_setup_ms`, `token_ms`, `sdp_ms`, `commit_to_first_audio_ms`, `frames_sent`, `frames_dropped`, `tool_calls`, `tool_ms` — filled from the existing `now_ms()` and printed by the harness on every turn. Three benefits: the "looking up…" state gets an honest threshold (show it after 300 ms, not immediately), regressions become visible, and the demo can quote a number instead of "it feels fast".

`frames_dropped` is already computed inside `ring_byte_ring_push` and thrown away (`ring_geodis_dl_push` discards `dropped`); wire it up — a rising count means the audio task is starved, which is the single most likely cause of choppy replies.

### 6.4 Session lifecycle as a first-class state

Today any disconnect lands in `ERROR` and only the app can recover, by calling `start()` again — which redoes Wi-Fi, SNTP, token, SDP. For a ring that is held in a hand across a room, losing Wi-Fi for 2 s is normal. Add:

- `RING_GEODIS_RECONNECTING` (or reuse CONNECTING with a reason), with exponential backoff jittered 1/2/4/8 s, max 3 attempts;
- a "press to retry" path that goes straight to token+SDP (skip Wi-Fi/SNTP if the IP and clock are still valid — `ring_wifi_wait_connected(0)` and a clock check);
- idle policy: after N minutes with no turn, tear the session down and keep only the Wi-Fi (a live DTLS + SRTP session costs battery for nothing). Re-establish on the next press; with §6.2's mock this is testable offline, and with the measured `session_setup_ms` you can decide whether the reconnect is even noticeable.

### 6.5 The tool call is the product risk, not the transport

The transport is now proven; the RAG call is the part that decides whether the demo lands. Four cheap improvements:

- Emit a real `TOOL` state (or a callback with `"looking up…"`) *before* the POST — with §1.1 fixed, this is a UI-visible stage rather than a log line. This is plan risk #10 and it is the difference between "thinking" and "hung".
- Language: the agent is French (`rechercher_base_geodis`, French web client). `whisper-1` auto-detects, which costs latency and misfires on short utterances. Azure accepts a language hint in the transcription config — test `{"model":"whisper-1","language":"fr"}` in the session update and measure `commit_to_first_audio_ms` with and without.
- Failure text: when `/tool-call` fails, the model currently gets `{"error":"tool-call failed: HTTP %d"}`, which it paraphrases into a confident denial. Give it something actionable (`"lookup unavailable, tell the user to try again"`) so the spoken answer is honest.
- Don't retry the tool POST on 5xx more than once. The user is waiting in real time; a second 1 s attempt is worse than "try again".

### 6.6 Documentation repairs (10 minutes)

- `components/ringkit/README.md:47` — `examples/14` does not exist. Point at the plan note, or at the app once it lands.
- `01_gbuddy_face/README.md` — "leaving 382,912 B free in the 1500 KiB partition" is stale; `partitions.csv` is 3000K and 01 is 1,153,600 B.
- `notes/geodis-push-to-talk-plan.md` §7 — Phase 1 is marked ✅ but "hardware check pending" and no app exists; the phase table should say so, and §6A.4's blocker (CST816S) needs a resolution line either way.
- `AGENTS.md` boot-log example still shows `app: 09_hello_smoke_test` as the sample — fine — but the module list should state that `ring_geodis` has no consumer app yet.

---

## 7. Suggested order of work

| # | Change | Why first |
|---|---|---|
| 1 | §1.1 + §1.2 — event queue carries transitions and transcript text | Every consumer depends on it; cheap; unblocks the UI |
| 2 | §2.1 — cfg defaults (`RING_GEODIS_CFG_DEFAULT()`) | Prevents the first app from shipping STT-off |
| 3 | §1.3 + §1.4 — peer ownership + session generation | The gateway flakes 1 in 12; retry is a normal path, not an edge case |
| 4 | §3.1 + §3.2 — barge-in flush, separate uplink-open | The core PTT gesture is broken without them |
| 5 | §5 — extract payload builders + host tests | Cheap regression net before the app |
| 6 | §6.2 — mock transport | Makes 7–9 testable at the desk |
| 7 | app (harness, then face) + §6.3 stats | The thing being asked for |
| 8 | §4 — `ring_rtc_core` extraction, applied to `ring_net` too | After the semantics stop moving |

## 8. What I could not verify

- **Nothing has run on the device.** Every claim above is from reading; the CST816S blocker (§6A.4) still gates real execution.
- Whether `esp_peer_send_data` from the peer callback is safe (`protocol.c:167` sends `session.update` from inside `on_data_frame`). `ring_net` does the same and 07 works, and `esp_peer` has no lock at all, so this is "empirically fine, contractually undefined". Worth a comment either way.
- Whether the mock's audio path (`ring_audio_write` at 48 kHz) needs the grace timer that 07 uses before the first frame — check with §6.2 before trusting the mock's smoothness.
- Exact `response.created` behaviour on this endpoint (whether it precedes the first audio frame by enough to be a useful UI trigger). One spike run answers it.
