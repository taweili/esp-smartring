# Host-side unit tests

Pure-logic ringkit modules are compiled unmodified on the development machine
and exercised against minimal ESP-IDF / FreeRTOS stubs. No hardware, no
`idf.py`.

```bash
cd tests
make check
```

## What is covered

| Test | Module | Cases |
|------|--------|-------|
| `test_ring_json` | `ring_json.c` | string extraction (escapes, `\uXXXX`, surrogate pairs, truncation), int/bool, `type_is`, `count_array` (nested arrays/objects, commas in strings, empty, unterminated), `append` overflow, `append_string` escaping |
| `test_ring_resample` | `ring_resample.c` + `ring_resample_coeffs.c` | coefficient DC gain, decim/interp DC steady state, impulse response shape, streaming-chunk equivalence, NULL/empty guards |
| `test_ring_byte_ring` | `ring_byte_ring.c` | push/pop, partial reads, newest-wins overflow, even-drop 16-bit alignment, reset, NULL guards |
| `test_ring_chat_messages` | `ring_llm.c` (`build_messages`) + `ring_chat.c` | empty/system-only output, message order, escaping, JSON buffer cap, history storage cap, `max_history_turns` window |
| `test_ring_geodis_payload` | `ring_geodis_payload.c` + `ring_json.c` | token/session-update/tool body builders, escaping, buffer caps |
| `test_ring_http` | `ring_http.c` | body reader (exact fit, truncation, transport error, empty), buffered SSE (CR strip, final line, long-line growth, transport error), bearer length |

## Layout

```
tests/
├── Makefile            # build + run
├── stub_esp.c          # malloc/heap-caps, dummy mutex, no-op HTTP client,
#                       #   no-op ring_ca / ring_http
├── stubs/              # esp_err.h, esp_log.h, esp_heap_caps.h,
#                       #   esp_http_client.h, freertos/{FreeRTOS,semphr,task}.h
├── test_ring_json.c
├── test_ring_resample.c
├── test_ring_byte_ring.c
├── test_ring_chat_messages.c
├── test_ring_geodis_payload.c
├── test_ring_http.c        # brings its own scriptable HTTP-client stub
└── ring_http_test_stubs.c
```

`test_ring_chat_messages.c` includes `ring_llm.c` directly and renames the
static `build_messages()` to expose it (white-box). The real `ring_chat.c` is
linked in so the history API and `ring_chat_s` are the production ones.

## Adding a test

1. Add `test_<module>.c` with a `main()` that prints
   `"<module>: N checks, M failures"` and returns `M ? 1 : 0`.
2. Add a target to the `Makefile` listing the ringkit sources it needs.
3. Add the binary name to `TESTS`.
