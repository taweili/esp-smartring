# 14_geodis_push_to_talk — GEODIS Voice Agent, push-to-talk

A push-to-talk voice assistant for the **GEODIS Voice Agent**: hold the round
button, speak, release, and the ring streams the microphone to Azure OpenAI
Realtime (`gpt-realtime-2.1`) over WebRTC through the GEODIS gateway, then
plays the spoken reply and shows both transcripts.

```
press → mic 48 kHz stereo → mono → Opus → WebRTC/RTP
      → GEODIS /token → Azure Realtime SDP → gpt-realtime-2.1
      → Opus → mono → speaker
      (client-executed RAG: response.function_call_arguments.done
       → POST /tool-call → function_call_output → response.create)
```

The app only talks to `ringkit/ringkit.h`. It never includes
`bsp/smartring_plus.h`, never calls `bsp_*`, and never touches `esp_peer`,
`esp_http_client` or the codec directly.

See `notes/geodis-push-to-talk-plan.md` (plan + decision record),
`notes/geodis-ai-api.md` (the API) and `components/ringkit/include/ringkit/ring_geodis.h`
(the authoritative module API).

## UI

Model on `06_qianwenai_push_to_talk`, inside `RING_SAFE_RADIUS`:

- Title `GEODIS` (green, top). A mock build shows `GEODIS · MOCK`.
- Big round **HOLD TO TALK** button, centre `(180, 112)`, radius 70 px.
- Stage line: `connecting…` / `ready — hold to talk` / `listening…` /
  `looking up…` / `waiting…` / `speaking…` / `error: …`.
- User transcript (orange) and assistant reply (white).
- Mic level bar (bottom).

All LVGL work is inside `RING_UI_LOCKED(-1)`; geometry uses `RING_CX` /
`ring_ui_place_centered()`.

## Flow

1. `ring_nvs_init()`, then read optional endpoint overrides from NVS
   (`geodis`/`base_url`, `geodis`/`sdp_url`) — a URL is not a secret, so this
   uses `ring_nvs_get_str()`, not `ring_secret_get()`.
2. `ring_geodis_init(state_cb, text_cb, tool_cb, NULL)` +
   `ring_geodis_set_tool_done_cb()`. No API key: the UAT gateway is
   unauthenticated.
3. **Connect task** (8 KB internal): `ring_geodis_start(&cfg)` only, with a
   5 s backoff. The module brings up Wi-Fi + SNTP itself; the app must not.
4. **Persistent audio task** (128 KB PSRAM, `xTaskCreateWithCaps`): one turn
   loop — capture while the button is held, `set_listening(false)` +
   `commit()` on release, then playback until `response.done` and the downlink
   ring drains.
5. Tool call: `tool_cb` shows `looking up…` before the module's built-in RAG
   POST; `tool_done_cb` shows the result (`ok`, HTTP status, chunk count).
6. Barge-in: press while the reply plays. The module cancels the response and
   flushes the decoded-audio ring; the audio task breaks out of playback and
   starts the new turn.

`RING_GEODIS_CFG_DEFAULT()` is used, so STT and PTT are both **on** (they are
tri-state; a zero-initialised struct would silently disable both).

## Tasks

| Task | Stack | RAM | Owner |
|---|---|---|---|
| `geodis-connect` | 8 KB | internal | `ring_geodis_start()` + retry |
| `geodis-audio` | 128 KB | PSRAM | capture / commit / playback turn loop |
| `ring-geodis-signal` | 4 KB | internal | token + SDP exchange (module) |
| `ring-geodis-peer` | 16 KB | internal | `esp_peer` + DTLS (module) |
| `ring-geodis-event` | 6 KB | internal | state + transcript callbacks (module) |
| `ring-geodis-tool` | 12 KB | internal | `/tool-call` HTTPS (module) |

The audio task needs a large stack because the Opus **encoder** runs on it
(`ring_geodis_send_audio()`); FreeRTOS stacks are internal-RAM-only, so it is
allocated in PSRAM.

## Build

```bash
# real transport (needs Wi-Fi credentials and outbound UDP for DTLS)
tools/dev.sh build 14_geodis_push_to_talk
tools/dev.sh flash 14_geodis_push_to_talk

# mock transport (no Wi-Fi, no UDP, no metered endpoint)
mkdir -p build/14_geodis_push_to_talk_mock
sed 's/^# CONFIG_RINGKIT_GEODIS_MOCK is not set$/CONFIG_RINGKIT_GEODIS_MOCK=y/' \
    sdkconfig > build/14_geodis_push_to_talk_mock/sdkconfig
idf.py -B build/14_geodis_push_to_talk_mock \
       -DRING_APP=14_geodis_push_to_talk \
       -DSDKCONFIG=build/14_geodis_push_to_talk_mock/sdkconfig build
```

The mock is `CONFIG_RINGKIT_GEODIS_MOCK` (default `n`) in
`components/ringkit/Kconfig`. It replaces only the network half of the module —
the same state machine, event queue, downlink ring, tool task and public API —
so the app is identical in both builds. Its `/tool-call` fails by design
(`lookup_unavailable`), which exercises the real failure path.

## Verification

- **Builds:** real 2,691,792 B (12% of the 3000K partition free); mock
  1,903,408 B (38% free).
- **Mock run:** ✅ 2026-11-21 on the ring — connect → `READY`; press →
  `LISTENING`; release → user transcript → `looking up…` → tool result →
  canned reply audio → `READY`; press while the reply plays → barge-in
  (`playback cancelled`, `mock: reply interrupted`). `commit->first audio`
  1003–1022 ms, `frames_dropped=0`, no panic. The log proves the state machine
  and the tool path; the on-screen rendering still needs eyes.
- **Real transport:** ✅ 2026-11-21 — the real session reaches `READY` on the
  ring (plan §6A.6). Three on-hardware fixes were needed: offer
  `a=setup:active` so the ring is the DTLS/SCTP client (Azure answers
  `passive`), `session.type: "realtime"` in `session.update`, and no
  `speaking` on comfort-noise frames. **Live user turn pending a human:**
  hold, speak, release, hear the reply, see both transcripts.
- **Stats:** every turn logs `ring_geodis_stats_t` — `session_setup_ms`,
  `token_ms`, `sdp_ms`, `commit_to_first_audio_ms`, `frames_sent`,
  `frames_dropped`, `tool_calls`, `tool_ms`. The struct is cumulative since
  `start()` (reset only by a new session), so `frames_sent`/`tool_calls` grow
  across turns.
- **Mock fix (2026-11-21):** `ring_geodis_send_event()` gained a
  `CONFIG_RINGKIT_GEODIS_MOCK` branch; without it `s.peer == NULL` made every
  `commit()` fail and the mock never started a turn. The mock also triggers a
  turn on `input_audio_buffer.commit` only.

## NVS endpoint overrides

```bash
# from a host shell with idf.py monitor, or via any app that calls:
ring_nvs_set_str("geodis", "base_url", "https://…/corp/voice-agent/v1");
ring_nvs_set_str("geodis", "sdp_url",  "https://…/openai/v1/realtime/calls");
```

Defaults (in `ring_geodis.h`): the UAT gateway
`https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1` and the Azure SDP
exchange `https://geodisopenai.openai.azure.com/openai/v1/realtime/calls`.
