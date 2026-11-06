# QUIC voice chat — design brainstorm for `examples/11_quic_voice_chat`

**Status:** research / design only (no code yet). Companion to
`notes/qianwen-ai-platform.md` (§1 protocols, §2.10 S2S models, §3 transports,
§3.4 token auth, §3.5 QUIC/AOQ internals) and `notes/design-ws-voice-chat.md`
(the TCP/WebSocket baseline this example is the A/B counterpart to).

**Goal.** A hands-free voice conversation with
`qwen-audio-3.1-realtime-flash` where the ring↔cloud audio leg runs over
**QUIC** instead of TCP/WebSocket or SRTP/DTLS — and where that choice is
*visible and measurable* on the 360×360 screen (RTT, loss, 0-RTT resumes).

---

## 1. The hard constraint that shapes everything

`notes/qianwen-ai-platform.md` §3 / §3.6: the platform's QUIC transport is **AOQ
(AI over QUIC, `x-dashscope-rtc-transport: moq`)**, and its client SDKs exist
only for **Android / iOS / HarmonyOS**. The wire protocol (ALPN, token
exchange, track layout on top of MoQ) is not documented, and the API key must
stay server-side — the flow is *app server → `aoqTokenForClient` + `sid` +
relay endpoints → client SDK connects*.

So there are exactly two honest options:

| Option | Verdict |
|---|---|
| **A. Device speaks AOQ directly** | **No.** Undocumented MoQ profile, no C SDK, would require reverse-engineering + cert-fingerprint/token handling nobody publishes. Weeks-to-never, and it breaks the token-auth contract. |
| **B. Device speaks our own QUIC to a local gateway (`ringgate`), gateway speaks WebRTC to Qianwen** | **Yes.** Everything QUIC is good for (0-RTT, datagram audio, per-track independence, migration) is preserved on the leg that actually suffers — Wi-Fi with a tiny antenna — while the cloud leg stays a transport the platform documents. |

Important side effect of B: the portal-LAN problem from
`examples/06_qianwenai_push_to_talk/README.md` §8 (all outbound UDP dropped on
this network, DTLS times out at `47.117.13x.xxx:3478`) **disappears**. With
`ringgate` on the LAN there is no NAT, no STUN, no TURN, and no reason for a
router to drop UDP. The example becomes demoable on the bench.

```
 ring (ESP32-S3)                            ringgate (Rust/Go, LAN host)
 ───────────────                            ────────────────────────────
 mic ─ rt|st ─► Opus 48k mono ─┐
                               ├─ QUIC DATAGRAM  track 1 (mic, unrel.) ─┐
 spk ◄─ st|rt ◄─ Opus ─────────┘                                        │
                               └─ QUIC bidi stream (control, JSON) ─────┤   WebRTC (SDP + DTLS-SRTP,
 transcript ◄─────────────────────────────────────────────────────────┤   DataChannel events)
 stats ◄── ACK / loss / PATH_RESPONSE ──────────────────────────────────┘        │
                                                                                 ▼
                                              maas.qianwenaiapi.com/api/v1/webrtc/realtime
                                                ?model=qwen-audio-3.1-realtime-flash
```

`ringgate` holds the API key, translates our two tracks into the SDP that
`esp_peer` currently generates for us, and forwards `session.update` /
`response.audio.*` events verbatim. Device-side token: short-lived, issued by
`ringgate` (same shape as AOQ's model, so the architecture stays compatible if
Alibaba ever ships a C SDK).

---

## 2. Why QUIC on *this* device (not cargo cult)

1. **No head-of-line blocking on audio.** Today's WebSocket realtime path puts
   JSON events *and* audio in one ordered TCP byte stream: one lost packet
   stalls the next 20 ms frame behind a retransmit (200 ms + on 2.4 GHz Wi-Fi
   with a body-worn antenna). QUIC DATAGRAM frames (RFC 9221) carry Opus
   frames unreliably and unordered; a loss costs exactly one frame, which the
   jitter buffer conceals. This is literally the "60 % packet loss" claim in
   AOQ's own marketing.
2. **0-RTT / cheap reconnects.** TLS-over-TCP is 2–3 RTT before the first
   JSON; QUIC resumes in 0–1 RTT from a stored **session ticket**, which
   matters for a wrist device that sleeps, roams, and reconnects constantly.
   (Ticket must be persisted in NVS — the fun part, see §5.)
3. **Track separation.** Control JSON (must be reliable+ordered), audio (must
   not be), transcript log (reliable, latency-tolerant) ride one connection
   with independent reliabilities. A long `response.audio_transcript.delta`
   cannot block audio.
4. **Migration.** Connection IDs survive a local address change
   (`PATH_CHALLENGE`/`PATH_RESPONSE`), so a Wi-Fi re-association + DHCP renewal
   can continue the call instead of re-handshaking — the classic wearable
   scenario, and trivially testable against `ringgate` by bouncing the STA.

Non-goals: throughput, browser interop, HTTP/3 for its own sake.

---

## 3. What to actually build on the device: a proven stack, not a new one

The blocker: **vendored mbedTLS (IDF v6) has no QUIC support** — no
per-level secret export, no record-layer bypass, no way to carry
`quic_transport_parameters`. Upstream won't have it soon (`Mbed-TLS/mbedtls`
#6178, #9421). So the TLS stack for QUIC must be **wolfSSL**, which *does*
implement a QUIC API and is in the ESP registry.

That combination is already proven on ESP32 silicon:

| Candidate | Language | Datagrams | ESP32 port | Verdict |
|---|---|---|---|---|
| **ngtcp2 + wolfSSL** | C | yes (`ngtcp2_conn_writev_datagram`, `recv_datagram` cb) | **[emqx/ESP32-QUIC](https://github.com/emqx/ESP32-QUIC)** — ngtcp2 + `wolfssl/wolfssl` on **ESP32-C3, IDF 6.0**, MQTT-over-QUIC, ~1.47 MB image, 1700K partition. Ships `ngtcp2.patch` + `wolfssl.patch` + a libev compat shim. | **Pick this.** |
| picoquic (+picotls minicrypto) | C | yes, + WebTransport | none public; no-malloc/constrained design is attractive, crypto is software-only | Plan B if ngtcp2's timer/RAM model fights us |
| quiche | Rust | yes | no (needs BoringSSL + std) | No |
| msquic / mvfst | C / **C++** | yes | no | No (and repo rule: no C++) |
| [78/esp-http3](https://github.com/78/esp-http3) | **C++**, mbedTLS-forked | no | claims ESP32 + HTTP/3 GET/POST | Reference only — no duplex audio, wrong language |

Concrete plan: vendor ngtcp2 as `components/ngtcp2/` (patched copy, same
"project `components/` outranks `managed_components/`" mechanism already used
for `espressif__esp_codec_dev` and `viewesmart__smartring_plus`), and pull
`wolfssl/wolfssl` from the registry with a project `user_settings.h`
(`WOLFSSL_QUIC`, HW AES/SHA, `NO_SESSION_CACHE` **off** — that macro is what
breaks the emqx build with `undefined reference to TlsSessionCacheGetAndRdLock`,
and we *want* the cache for 0-RTT + migrations).

Two crypto stacks in one image (mbedTLS for HTTPS/SNTP/DTLS, wolfSSL for QUIC)
costs ~250 KB flash. Acceptable on a 16 MB part; the device side no longer
needs `esp_peer` at all, which claws much of it back.

---

## 4. Wire protocol: "MOQT-shaped", not MOQT (yet)

Two layers, deliberately separated:

**(a) Transport framing — hand-rolled, ~80 lines.** ALPN `ringquic/1`.
* one reliable **bidi stream**: newline-delimited JSON control events
  (`hello`/`welcome`, `session.update`, `response.audio_transcript.delta`, …)
* **DATAGRAMs**: 8-byte header `{u8 track, u16 seq, u32 ts_ms}` + Opus payload.
  Track 1 = mic→gateway, track 2 = gateway→speaker, track 3 = telemetry.
Sequence numbers give us the loss counter for the UI and let the jitter buffer
order frames (Opus itself is loss-tolerant, not reorder-tolerant by much).

**(b) Future-proofing.** Keep the *semantics* MOQT-shaped (track alias,
group/object, priorities) so swapping in [draft-ietf-moq-transport-23]
(control stream + subgroups + object datagrams, relay-capable) later is a
header change, not a rewrite. Full MOQT buys interop with `moq-rs`/relay
ecosystem and nothing else we need today. Do **not** start here.

WebTransport-over-HTTP/3 would add browser interop at the cost of nghttp3
(EXTENDED CONNECT + CONNECT-WebTransport handling). Out of scope for v1.

---

## 5. The interesting engineering problems (rank them before coding)

| # | Problem | Notes / mitigation |
|---|---|---|
| 1 | **Opus PLC** | `esp_audio_codec`'s decoder wrapper may not expose `opus_decode(..., decode_fec, ...)`. Concealing a lost 20 ms frame by repeating the last one is audible. Spike this *first*: if the wrapper is opaque, link `espressif/opus` directly for decode (encode can stay in `esp_audio_codec`). |
| 2 | **Session-ticket persistence** | 0-RTT only works across boots if the wolfSSL session is `wolfSSL_get_session`'d into NVS and restored with `wolfSSL_set_session`. Needs an NVS blob + versioning, plus a sane fallback to 1-RTT on any mismatch. |
| 3 | **ngtcp2 timer model on FreeRTOS** | One `quic-net` task doing `select()` on the UDP socket with `ngtcp2_conn_get_expiry()` as the timeout; all ngtcp2 callbacks (incl. `recv_stream_data`) run on it. Same lesson as `esp_peer`: **no LVGL, no 4 KB stack arrays, `memcpy` only** in callbacks; marshal to the app through a queue (the `ring-net-event` pattern in `ring_net.c`). |
| 4 | **No AEC on the ring** | Unchanged from 06/07: ES8311 has no AEC, speaker sits next to the mic. Start **half-duplex** (energy/VAD gate closes the mic while the speaker is live) even though QUIC could do full duplex. Full duplex is the stretch goal and needs ESP-SR AFE (`notes/networking-https-webrtc.md` §5.2) — big, pulls `esp-sr`, do not put it on the critical path. |
| 5 | **Flash** | 06 is 2.78 MB inside a 3000K partition (9 % free). wolfSSL + ngtcp2 do not fit in the slack → raise `partitions.csv` to **4000K** (it is shared by every app; that is fine, 16 MB part). |
| 6 | **CPU** | Opus SILK encode already peaks ~20 KB stack and real time on the S3 while Wi-Fi runs. Add AES-GCM per QUIC packet (wolfSSL HW accel) — small, but profile the audio task before adding a second crypto stack's spin. |
| 7 | **This network blocks UDP** | Already proven in 06 §8. `ringgate` on the LAN is the answer; document "phone hotspot or LAN gateway" as a prerequisite, and keep the 06-style explicit network gate (Wi-Fi → SNTP → probe → handshake) on screen. |

---

## 6. The app: `examples/11_quic_voice_chat`

Deliberately different from its neighbours: 06 is push-to-talk (button gates
the mic), 07 is a headless-ish harness. This one is **an always-on
conversation with a transport you can watch**.

```
        ┌─────────────────────────────┐
        │        ◠◠◠◠◠◠◠◠◠◠◠          │  ← orb: level-arc ring, pulses with
        │      ◜             ◝        │     mic (green) or model (blue) level
        │     │   listening…   │      │
        │      ◟      “好的，        │  ← 1–2 line rolling transcript of
        │        ◡◡◡◡◡◡◡◡◡◡◡        │     response.audio_transcript.delta
        │─────────────────────────────│
        │ quic · 0-RTT · rtt 12 ms    │  ← status strip: the QUIC story
        │ loss 3% · rec 1 · 00:41     │
        └─────────────────────────────┘
```

* **Turn-taking:** server/semantic VAD at the gateway; mic gated by a local
  energy gate while audio is playing (half-duplex, honest about no AEC).
* **Tap = barge-in** (stop playback, flush jitter buffer, `response.cancel`);
  **long-press = end call**. Touch is a control, not a talk key.
* **Status strip is the point of the example:** RTT comes from real QUIC ACKs,
  `loss` from DATAGRAM sequence gaps, `0-RTT`/`1-RTT`/`resumed` from the
  handshake, `rec` from reconnect count (path migration / ticket reuse).
* **A/B switch:** build flag `RING_VOICE_TRANSPORT=quic|webrtc`. Both live
  behind a tiny `ring_voice_*` facade (`ring_net` = WebRTC backend,
  `ring_quic` = QUIC backend, both PCM-in/PCM-out at 48 kHz mono, 20 ms
  frames). Same screen, same app code, two transports — that is what makes
  the loss/latency claim checkable on the device instead of in a blog post.

### ringkit surface (keeps the "apps never touch the network" rule)

```c
/* components/ringkit/include/ringkit/ring_quic.h */
typedef struct {
    uint32_t rtt_ms, loss_permille, datagrams_in, datagrams_lost;
    bool     resumed;          /* 0-RTT used */
    uint32_t reconnects;
    const char *alpn;
} ring_quic_stats_t;

esp_err_t ring_quic_init(const ring_quic_cfg_t *cfg);  /* host, port, token, ca */
esp_err_t ring_quic_connect(void);                     /* 0-RTT if ticket in NVS */
esp_err_t ring_quic_send_audio(const int16_t *pcm, size_t samples);
size_t    ring_quic_read_audio(int16_t *pcm, size_t samples);
esp_err_t ring_quic_send_json(const char *json);
ring_quic_stats_t ring_quic_stats(void);
esp_err_t ring_quic_ticket_save(void);
esp_err_t ring_quic_stop(void);
```

Same shape as `ring_net.h` on purpose: same app code, swapped backend.

---

## 7. `ringgate` (the other half)

Deliberately boring: **Go + Pion** (or Rust + `webrtc-rs`), ~600 lines.

* QUIC listener (quinn-rs / quic-go), ALPN `ringquic/1`, self-signed or
  `ringgate`-issued cert pinned by the device.
* Handshake → token check → spawns one WebRTC PeerConnection per device:
  `POST /api/v1/webrtc/realtime?model=qwen-audio-3.1-realtime-flash`,
  Content-Type `application/sdp`, `Authorization: Bearer $DASHSCOPE_API_KEY`.
* Bridges: mic datagrams → Opus RTP track; model Opus track → datagrams
  (drop-to-latest if the device's jitter buffer is deep); DataChannel JSON ↔
  control stream.
* Instrumentation for the demo: `tc qdisc add dev lo root netem loss 5%`,
  a `/stats` JSON endpoint, and a `--fake-ring` mode that replays a WAV so the
  gateway can be developed with no hardware.

---

## 8. Milestones (and the schedule risk)

| M | Deliverable | Effort |
|---|---|---|
| **M0** | wolfSSL + ngtcp2 build as components on **esp32s3 / IDF v6.1**; a `quic_probe` that handshakes with `nghttp2.org:4433` (ALPN h3) or `ringgate` and logs RTT/ALPN/ticket. No app changes. | ½–1 day — **this is the risk**. If it fails, stop and reassess. |
| **M1** | `ringgate` skeleton: fake-ring WAV replay ⇄ Pion ⇄ Qianwen WebRTC, full S2S turn working from a laptop. | 2–3 days |
| **M2** | `ring_quic` + device audio path (reuse `ring_net`'s Opus setup); one end-to-end turn. | 2 days |
| **M3** | UI (orb, transcript, stats strip), barge-in, ticket persistence, reconnect. | 1–2 days |
| **M4** | Stretch: real MOQT, WebTransport/browser client, path-migration demo, full-duplex with ESP-SR AFE. | open |

Realistic: **~2 weeks to M3** for someone who already knows `ringkit`; the
whole estimate is hostage to M0.

---

## 9. Alternatives considered

1. **WebSocket (TCP) + own AEC** (`notes/networking-https-webrtc.md` §5.x) — simplest, no new
   stack, but the head-of-line stall under 2.4 GHz loss is exactly what we are
   trying to remove. Keep as the baseline in the A/B build.
2. **WebRTC (`esp_peer`, example 06)** — has built-in AEC and is already wired
   up, but no 0-RTT, ICE/TURN machinery we don't need against a LAN gateway,
   and the platform's media endpoint was unreachable from here. Keep it as the
   other A/B backend.
3. **AOQ direct** — blocked (§1).
4. **HTTP/3 only for signalling** (POST the SDP over QUIC) — trivial gain, one
   request per call. Not worth a second TLS stack on its own; only interesting
   as an M0 smoke test.
5. **picoquic** — genuinely attractive for constrained devices (no-malloc
   arenas, `picoquic_set_memory_limit`, H3 + WebTransport + datagrams in one
   repo) but no public ESP32 port and software-only crypto. Plan B.

---

## 10. Verification checklist (when it exists)

1. Boot log: `ringkit:` summary all `ok`, `ring_quic: handshake done
   rtt=12ms resumed=0`; **second** connect after reboot: `resumed=1`.
2. From a clean `-B` dir: `00_glyph_grid_reveal`, `09_hello_smoke_test`,
   `06_qianwenai_push_to_talk` (WebRTC backend) and
   `11_quic_voice_chat` both transports.
3. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`) + new: `components/ngtcp2/` wins over a
   manager copy.
4. Loss test: `netem loss 5%` at the gateway → audio continues, `loss ≈ 5%`
   on screen, no gap > 100 ms; WebRTC backend under the same loss stalls
   audibly. Screenshot-worthy.
5. **Touch, barge-in and speech quality need a human** — say so, as always.

---

## 11. Doc debt this creates

* `notes/qianwen-ai-platform.md` §5 should gain a line: *AOQ is unreachable from
  device firmware (no C SDK, undocumented MoQ profile) → use a gateway; the
  QUIC win belongs on the device↔gateway leg.*
* `notes/networking-https-webrtc.md` needs a **§6 QUIC** section: wolfSSL-vs-mbedTLS,
  ngtcp2 timer model, the emqx ESP32 port as prior art, flash/RAM budget.
* `partitions.csv` comment (1500K → 3000K → 4000K) gets a third reason.
* `notes/architecture-rationale.md` §6 gains a trap: *"mbedTLS has no QUIC interface —
  adding QUIC means adding wolfSSL, and two TLS stacks is a flash decision,
  not a library decision."*

[draft-ietf-moq-transport-23]: https://datatracker.ietf.org/doc/html/draft-ietf-moq-transport
