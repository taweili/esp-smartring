# ringkit refactor — implementation plan

**Status:** phase 1 complete (code + device boot verification).
Phase 2–4 not started.
**Companion to:** the `components/ringkit` review (findings recorded in the
progress log below) and `notes/architecture-rationale.md`.
**Goal:** extract the shared infrastructure that is currently copy-pasted
between the voice modules, split the oversized files, and make the public API
contracts honest — without touching the thin BSP wrappers that already work.

---

## 0. Why

The review of `components/ringkit` found three structural problems and a set of
contract bugs:

1. **Duplication.** `json_get_str`/`json_type_is` exist in `ring_net.c`,
   `ring_ws.c` and a third variant in `ring_chat.c`; the HTTPS client plumbing
   exists in `ring_chat.c` and `ring_net.c`; the downlink ring exists in
   `ring_net.c` and `ring_ws.c`; the SNTP sync is borrowed across modules via a
   mid-file `extern`; the embedded CA symbols are declared three times.
2. **Oversized files.** `ring_ws.c` (1389), `ring_net.c` (1216), `ring_chat.c`
   (656), `ring_bt.c` (678) each mix several responsibilities.
3. **Contract bugs.** `RING_UI_LOCKED` leaks on `return`/`break`;
   `ring_chat_stt()`'s body buffer is 8 KB against a ~683 KB base64 payload;
   `ring_chat_tts()` needs ~20 KB of stack on a 16 KB caller task;
   `build_messages()` overflows and does not escape; the TTS SSE loop can spin
   on EOF; `ring_net_send_audio()` has no frame-size guard; `ring_net_start()`
   does not reset the downlink ring; `ring_ws` never sends `instructions`;
   `ring_chat` never sends `stt_language`.

The BSP wrappers (`ring_board`, `ring_display`, `ring_touch`, `ring_battery`,
`ring_power`, `ring_audio`, `ring_imu`, `ring_sd`, `ring_ui`, `ring_rand`) are
in good shape and are explicitly **out of scope** for restructuring.

---

## 1. Phase 1 — shared modules + contract fixes

### 1.1 New shared modules

Each is a leaf (no dependency on the other new modules unless noted) and gets
added to `components/ringkit/CMakeLists.txt` `SRCS`.

- [x] **`ring_ca.[ch]`** — the embedded `qianwen_ca.pem`. Owns the
      `_binary_qianwen_ca_pem_start/end` symbols; exposes `ring_ca_pem()` /
      `ring_ca_pem_len()`. `ring_net_ca_pem()` / `ring_ws_ca_pem()` become thin
      wrappers so the public API is unchanged.
- [x] **`ring_time.[ch]`** — SNTP sync (`ring_time_sync(timeout_ms)`), moved
      out of `ring_net.c`. Removes the `extern esp_err_t
      ring_net_sync_clock(void);` declaration in the middle of `ring_ws.c`.
- [x] **`ring_nvs.[ch]`** — `ring_nvs_init()` (the `nvs_flash_init` +
      re-format dance) and `ring_nvs_get_or_seed_str()`. Used by `ring_wifi`,
      `ring_secret`, `ring_bt`.
- [x] **`ring_json.[ch]`** — `ring_json_get_str()` (full escape + `\uXXXX`
      handling, from `ring_net.c`), `ring_json_get_int()`,
      `ring_json_get_bool()`, `ring_json_type_is()`, and `ring_json_append()`
      (the overflow-safe formatter from `ring_ws.c`). Replaces all three
      private copies.
- [x] **`ring_byte_ring.[ch]`** — `ring_byte_ring_t` with mutex, newest-wins
      push, pop, count, reset, deinit. Replaces the `dl_push`/`dl_pop` pairs in
      `ring_net.c` and `ring_ws.c`.
- [x] **`ring_resample.[ch]`** — unify the 48k→16k decimator and the 24k→48k
      interpolator, and move both coefficient tables out of
      `ring_ws_resample.c`. Update `tools/gen_ws_resampler.py` to emit the
      tables into the new location. `ring_chat.c` and `ring_ws.c` switch to it.
- [x] **`ring_http.[ch]`** — `ring_http_init()` (URL + pinned CA + timeout +
      buffer size), `ring_http_set_bearer()`, `ring_http_read_body()`,
      `ring_http_sse_line()` (with an explicit `*eof` out-param). Used by
      `ring_chat.c` and the SDP exchange in `ring_net.c`.

### 1.2 Consumer refactors

- [x] `ring_net.c` → `ring_ca`, `ring_time`, `ring_json`, `ring_byte_ring`,
      `ring_http`; remove the private JSON/ring/CA/SNTP code.
- [x] `ring_ws.c` → `ring_ca`, `ring_time`, `ring_json`, `ring_byte_ring`,
      `ring_resample`; remove the private JSON/ring/interpolator/CA/SNTP code.
- [x] `ring_chat.c` → `ring_ca`, `ring_http`, `ring_json`, `ring_resample`;
      remove the private HTTP/JSON code.
- [x] `ring_wifi.c` → `ring_nvs`.
- [x] `ring_secret.c` → `ring_nvs`.
- [x] `ring_bt.c` → `ring_nvs`.

### 1.3 Contract fixes

- [x] **`ring_ui.h`** — make `RING_UI_LOCKED` safe on `return`/`break`
      (`__attribute__((cleanup))` on a lock handle) or change the doc to state
      the body must not exit early. The current doc promises the opposite of
      what the code does.
- [x] **`ring_net_send_audio()`** — reject `samples != RING_NET_FRAME_SAMPLES`
      (the `ring_ws_send_audio()` equivalent already does).
- [x] **`ring_net_start()`** — reset `dl_head/dl_tail/dl_count` and
      `send_stream_id` before a (re)connect.
- [x] **`ring_chat_stt()`** — size the JSON body to `B64_CAP` (or allocate
      `wav_len * 4/3 + 512`) instead of `JSON_CAP`.
- [x] **`ring_chat_tts()`** — move `body` / `line` / `data_b64` to PSRAM or
      shrink them; the current ~20 KB of stack overflows the 16 KB caller.
- [x] **`ring_chat build_messages()`** — use `ring_json_append()` (overflow
      checked) and actually escape quotes/newlines in history text.
- [x] **`ring_chat_tts()` SSE loop** — exit on EOF, not only on
      `finish_reason == "stop"`.
- [x] **`ring_ws` `session.update`** — send `instructions` when non-NULL.
- [x] **`ring_chat_stt()`** — send `stt_language` when non-NULL.
- [x] **`ring_bt` READ** — initialise `out_len` to 0, not `max_len`.
- [x] **`ring_wifi_stop()`** — reset `state`/`connected`/`ip`.
- [x] **`ring_net` / `ring_ws`** — guard `s.state` with a mutex (same pattern
      as `ring_wifi`/`ring_bt`) or document single-writer.
- [x] **`ring_net` / `ring_ws`** — free partially-allocated resources on init
      failure.

### 1.4 Verification

- [x] Build `00_glyph_grid_reveal`, `06_qianwenai_push_to_talk`,
      `07_qianwen_realtime_harness`, `09_hello_smoke_test`,
      `10_qianwen_ws_voice_chat` from clean `-B` dirs.
- [x] Both vendored overrides still win (`espressif__esp_codec_dev`,
      `viewesmart__smartring_plus`).
- [x] Boot log of `00` and `09`: no panic/assert; `ringkit:` summary all `ok`.

---

## 2. Phase 2 — split the oversized modules

- [ ] `ring_net.c` → `ring_net.c` (API/state/tasks) + `ring_net_signaling.c`
      (SDP/HTTPS) + `ring_net_codec.c` (Opus) + `ring_net_protocol.c`
      (DataChannel JSON).
- [ ] `ring_ws.c` → `ring_ws.c` (API/state/tasks) + `ring_ws_transport.c`
      (WS lifecycle + reassembly) + `ring_ws_protocol.c` (session/turn JSON).
- [ ] `ring_chat.c` → `ring_chat.c` (cascade + history) + `ring_stt.c` +
      `ring_llm.c` + `ring_tts.c`.
- [ ] `ring_bt.c` → `ring_bt.c` (API/init/state) + `ring_bt_gatt.c` +
      `ring_bt_gap.c`.
- [ ] Target: every source file < ~400 lines.

## 3. Phase 3 — build hygiene

- [ ] Add `CONFIG_RINGKIT_NET` / `_WS` / `_CHAT` / `_BT` Kconfig symbols and
      compile those sources conditionally.
- [ ] Move `esp_peer`, `esp_audio_codec`, `esp_websocket_client`,
      `esp_http_client`, `bt` from `REQUIRES` to `PRIV_REQUIRES` where the
      public header does not expose their types (true for all four voice
      headers and `ring_bt.h`).
- [ ] Measure app 00's size/build-time delta.

> Note: the `00` map already contains no `ring_net`/`ring_ws`/`ring_chat`/
> `esp_peer` symbols, so the linker GC's unused modules. The benefit here is
> compile time and dependency hygiene, not image size.

## 4. Phase 4 — tests + docs

- [ ] Host-side unit tests: `ring_json` (escapes, unicode, truncation),
      `ring_resample` (impulse/step), `ring_byte_ring` (overwrite/alignment),
      `build_messages` (escaping, cap).
- [ ] `examples/12_ringkit_selftest` — board/display/touch/audio/IMU/SD/Wi-Fi/
      BT init + voice `init`/`stop`.
- [ ] `components/ringkit/README.md` — module map, task/thread model, state
      machines, API contract, voice-transport decision guide.
- [ ] Doxygen pass on the public headers.

---

## 5. Progress log

### 2025-11-08 — plan created

- Plan written; phase 1 todo list compiled. No code changes yet.
- Review findings that drive the phase 1 order:
  - `RING_UI_LOCKED` (`ring_ui.h:37`) leaks on `return`/`break` — verified with
    a standalone C test.
  - `ring_chat_stt()` (`ring_chat.c:255`) body buffer is `JSON_CAP` (8 KB) but
    the base64 payload is up to `B64_CAP` (~683 KB) → fails above ~190 ms.
  - `ring_chat_tts()` (`ring_chat.c:522,573,467`) uses ~20 KB of stack
    (`body[8192]` + `line[4096]` + `data_b64[8192]`) on a 16 KB caller.
  - `build_messages()` (`ring_chat.c:345`) `pos += snprintf(...)` overflows;
    no quote escaping despite the comment.
  - `ring_chat_tts()` SSE loop (`ring_chat.c:577`) spins on EOF.
  - `ring_net_send_audio()` (`ring_net.c:1117`) has no `samples` guard.
  - `ring_net_start()` (`ring_net.c:1012`) does not reset the ring / stream id.
  - `ring_ws` `instructions` (`ring_ws.h:48`) and `ring_chat` `stt_language`
    (`ring_chat.h:34`) are never sent.

### 2025-11-08 — phase 1 code complete (pending device boot check)

**New shared modules** (`components/ringkit/{include/ringkit,src}/`):
`ring_ca`, `ring_time`, `ring_nvs`, `ring_json`, `ring_byte_ring`,
`ring_resample` (now decim + interp), `ring_http`. `ring_ws_resample.c` was
renamed to `ring_resample_coeffs.c` and `tools/gen_ws_resampler.py` now emits
the tables there. `ringkit.h` includes the new headers.

**Consumer refactors:** `ring_net`, `ring_ws`, `ring_chat`, `ring_wifi`,
`ring_secret`, `ring_bt` now use the shared modules; the private JSON / HTTP /
ring / NVS / SNTP / CA copies are gone. `ring_net_ca_pem()` and
`ring_ws_ca_pem()` remain as thin wrappers over `ring_ca_pem()`.

**Contract fixes:**
- `RING_UI_LOCKED` now uses a `__attribute__((cleanup))` lock handle, so it
  unlocks on `return` / `break` / `goto` (verified with a host test).
- `ring_net_send_audio()` rejects `samples != RING_NET_FRAME_SAMPLES`.
- `ring_net_start()` resets the downlink ring and `send_stream_id`.
- `ring_chat_stt()` sizes the JSON body to `b64_len + 256` (was a fixed 8 KB
  against a ~683 KB payload) and sends `stt_language`.
- `ring_chat_tts()` moved `body`/`line`/`data_b64` to the heap (was ~20 KB of
  stack on a 16 KB caller) and exits the SSE loop on EOF.
- `ring_chat build_messages()` uses `ring_json_append()` (overflow-checked) and
  escapes quotes/newlines/control chars.
- `ring_ws session.update` sends `instructions` (escaped).
- `ring_bt` READ initialises `out_len` to 0 (was `max_len`, which could send
  uninitialised bytes) and bounds it to `RING_BT_MAX_VALUE`.
- `ring_wifi_stop()` resets `state`/`connected`/`ip`.
- `ring_net` / `ring_ws` guard `s.state` with a mutex; init failure releases
  partially-allocated resources (`net_release()` / `ws_release()`).

**Verification:**
- Builds from clean `-B` dirs: `00`, `06`, `07`, `09`, `10` — all pass, no
  warnings (`-Werror`).
- Vendored overrides still win in every build (249/49 refs in `00`).
- `00` image 750,656 B (was 750,576 B) — the shared modules do not bloat it;
  the map still has no `esp_peer`/`ring_net`/`ring_ws`/`ring_chat` symbols.
- Host tests: `ring_json` (escapes, unicode/surrogates, int/bool, truncation,
  append overflow) and `ring_resample` (DC gain, streaming chunk equivalence)
  pass.
- **Pending:** device boot log for `00`/`09` (needs the ring).

**Follow-up fix during self-review:** `ring_chat_llm_complete()` now checks
the `snprintf` return value before `esp_http_client_set_post_field()` (a long
`s.cfg.llm_model` could otherwise make `body_len` exceed the allocation).
Added `ring_json_append_string()` to `ring_json` and used it for the WS
`instructions` field; `ring_chat`'s local `append_message()` uses the same
escaping rules.

**Final rebuild:** all five apps (`00`, `06`, `07`, `09`, `10`) build clean
after the follow-up fix.

### 2025-11-08 — device boot verification complete

Flashed `00_glyph_grid_reveal` and `09_hello_smoke_test` to the ring and
captured the boot log (continuous drain, no back-pressure hang). Both:

- `ringkit: display=ok battery=ok(AXP2101/V2) power=ok audio=ok sd=ok imu=ok`
- app ready line printed (`00_...: ready: 169 glyphs…` /
  `09_...: Hello World on screen`)
- `smartring: app '<name>' is on screen`
- **no** `panic` / `assert` / `Guru Meditation` / stack-overflow hook.

Phase 1 is complete. The ring is currently running `09_hello_smoke_test`.

---

## 6. Files touched

| File | Change |
|---|---|
| `components/ringkit/include/ringkit/ring_{ca,time,nvs,json,byte_ring,resample,http}.h` | new |
| `components/ringkit/src/ring_{ca,time,nvs,json,byte_ring,resample,http}.c` | new |
| `components/ringkit/src/ring_{net,ws,chat,wifi,secret,bt}.c` | refactor to shared modules |
| `components/ringkit/src/ring_ws_resample.c` | tables moved to `ring_resample.c` |
| `tools/gen_ws_resampler.py` | emit tables to the new location |
| `components/ringkit/CMakeLists.txt` | add new SRCS |
| `components/ringkit/include/ringkit/ringkit.h` | include new headers |
| `components/ringkit/include/ringkit/ring_ui.h` | fix `RING_UI_LOCKED` |
| `components/ringkit/include/ringkit/ring_{net,ws,chat,bt,wifi}.h` | contract fixes |

---

## 7. Verification matrix

| Level | Check |
|---|---|
| Build | Clean `-B` dirs for `00`, `06`, `07`, `09`, `10`; no warnings |
| Overrides | `espressif__esp_codec_dev` and `viewesmart__smartring_plus` still win |
| Size | `idf.py size` on `00` and `06` before/after |
| Boot | `00`/`09`: `ringkit:` summary all `ok`, no panic/assert |
| Voice | `06` full cascade; `07`/`10` one turn (needs network + human check) |
| Unit | Host tests pass for `ring_json`, `ring_resample`, `ring_byte_ring` |
