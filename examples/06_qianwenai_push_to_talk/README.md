# 06_qianwenai_push_to_talk — one round button, hold-to-talk over WebRTC

`examples/06_qianwenai_push_to_talk/` is a **push-to-talk voice assistant** for the
Smart Ring Plus. One round button fills the screen: **green when ready to
talk**, **red while recording or waiting on the server response**. Hold it to
talk to `qwen-audio-3.1-realtime-flash` on the [Qianwen AI platform](https://www.qianwenai.com),
**release it** and the model's spoken reply comes out of the ring's speaker.

```
        ┌─────────────────────────────┐
        │          QIANWEN            │
        │      ┌───────────────┐      │
        │      │               │      │
        │      │  HOLD TO TALK │      │  ← the one button (r = 78 px,
        │      │               │      │     solid colour fill)
        │      └───────────────┘      │
        │      ready — press & hold   │
        │      ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁      │  ← level bar
        │      hold = talk · release  │
        └─────────────────────────────┘
```

The button is a **solid colour-filled circle** — the whole disc is the
state colour, not an outline:

| State | Button | Status | What happens |
|---|---|---|---|
| Idle | **green fill**, `HOLD TO TALK` | "ready — press & hold to talk" | session open, mic closed |
| Talking | **red fill**, `TALK` | "listening…" + level | 48 kHz mic → Opus → RTP uplink |
| Waiting | **red fill**, `WAIT` | "waiting…" | released, waiting for the server response |
| Replying | **red fill**, `REPLY` | "replying…" + level | Opus → PCM → speaker, uplink closed |
| Connecting | grey fill, `···` | "connecting to \<SSID\>…" → "connecting…" | Wi-Fi → SNTP → SDP → DTLS → session |
| Error | red fill, `!` | "connection lost — reconnecting" | retries every 5 s |

---

## 1. Why this shape

**WebRTC, not WebSocket.** The model is reachable over AOQ, WebRTC and
WebSocket (`notes/qianwen-ai-platform.md` §3). WebRTC is the transport the platform
documents for non-browser clients, and it carries the model's echo
cancellation / noise suppression — which matters because the ring's speaker
sits centimetres from its microphone.

**Opus at 48 kHz, not PCM in JSON.** The endpoint **accepts only Opus** on
`m=audio`. Offering PCMA/PCMU is rejected server-side with
`{"code":"remote_sdp_failed","message":"SDP format and content error."}` (and
offering Opus *and* PCMA still answers Opus). `esp_peer` carries encoded frames
but has no codec of its own, so `ring_net` runs `esp_audio_codec`'s Opus
encoder/decoder. The ES8311 is fixed at 48 kHz/16-bit/stereo, so this costs
only a stereo↔mono fold — no resampling.

**Button-gated uplink, server VAD.** WebRTC supports only `server_vad` /
`semantic_vad`; **manual turn-taking is WebSocket-only**. So the button opens
and closes the mic, and the model decides where the utterance ends. As soon as
audio starts arriving we stop the uplink, which stops the model hearing its own
reply through the ring's speaker.

---

## 2. Architecture

```
   touch ──► LVGL task ──► 06_qianwenai_push_to_talk (this example)
                             │  PRESSED / RELEASED  → flags + worker task
                             │  ui_*()  ── RING_UI_LOCKED
                             ▼
                        ring_net  (components/ringkit, new module)
                             │  Wi-Fi   (ring_wifi, NVS credentials)
                             │  SNTP    (esp_netif_sntp — TLS needs a clock)
                             │  Opus    (esp_audio_codec, 48 kHz mono)
                             │  SDP     (esp_http_client → HTTPS POST)
                             │  WebRTC  (esp_peer: ICE / DTLS / SRTP / SCTP)
                             ▼
              maas.qianwenaiapi.com /api/v1/webrtc/realtime
                      ?model=qwen-audio-3.1-realtime-flash
```

Applications never include `esp_peer`, `esp_http_client` or the codec — the
`ringkit` rule holds: hardware and network live behind `ring_*` APIs.

| File | Role |
|---|---|
| `examples/06_qianwenai_push_to_talk/qianwenai_push_to_talk.c` | UI, button state machine, stereo↔mono, talk turn |
| `components/ringkit/src/ring_net.c` | Wi-Fi + SNTP + Opus + SDP exchange + esp_peer |
| `components/ringkit/include/ringkit/ring_net.h` | `ring_net_*` API |
| `components/ringkit/assets/qianwen_ca.pem` | pinned trust chain (see §5) |

---

## 3. Connection sequence

1. **Wi-Fi, first and explicitly.** The app calls `ring_wifi_init()`,
   `ring_wifi_start()` and `ring_wifi_wait_connected(20 s)` in its connect task
   *before* starting the session, and shows the link state on screen
   (`connecting to <SSID>…` → `Wi-Fi connected · <IP>`). A failure retries
   every 5 s. `ring_net_start()` then reuses the station
   (`ring_wifi_start()` is idempotent). Credentials come from NVS, seeded
   from `RING_WIFI_DEFAULT_*` on first boot.
2. **Clock — mandatory.** `esp_netif_sntp_init()` against `ntp.aliyun.com`
   then `pool.ntp.org`. The HTTPS POST and the DTLS handshake both validate
   certificates against the wall clock; an unset clock fails every connection.
3. **Opus.** `esp_opus_enc_register_default()`, encoder at 48 kHz mono,
   20 ms frames, 32 kbps, complexity 0 (the S3 is also running Wi-Fi).
4. **Offer SDP.** `esp_peer` gathers candidates and reports the offer through
   `on_msg`, normalised to **CRLF**.
5. **SDP exchange.**
   ```
   POST https://maas.qianwenaiapi.com/api/v1/webrtc/realtime?model=qwen-audio-3.1-realtime-flash
   Content-Type: application/sdp
   Authorization: Bearer <QIANWEN_API_KEY>
   <body = Offer SDP>
   → 200, body = Answer SDP (plain text)
   ```
   Uses the streaming API (`open` / `write` / `fetch_headers` / `read`) —
   `esp_http_client_perform()` consumes the body, and the body *is* the answer.
6. **DTLS + DataChannel.** `esp_peer_send_msg(..., ESP_PEER_MSG_TYPE_SDP)`.
7. **Session.** On `on_channel_open`, send `session.update` and wait for
   `session.updated`:
   ```json
   {"type":"session.update","session":{
     "modalities":["text","audio"],
     "voice":"longanqian_v3.1",
     "input_audio_transcription":{"model":"qwen3-asr-flash-realtime"},
     "turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800}}}
   ```
8. **READY** — the button turns green.

Audio format on the `ring_net` API is **48 kHz / 16-bit / mono PCM**, 960
samples (20 ms) per uplink call. Opus is an implementation detail.

---

## 4. Task & memory layout (hard-won)

The first cut crashed repeatedly; the causes are now design rules:

| Issue | Fix |
|---|---|
| `esp_peer_main_loop` runs the **DTLS handshake** on the peer task | peer task stack = **16 KB** (was 6 KB → stack overflow) |
| Opus `silk_encode_frame_FIX` alone has a **~9.5 KB stack frame**; the SILK path peaks at **~20 KB** (silence/voice) | `qw-talk` stack = **24 KB** (was 8 KB → overflow on the first encoded frame) |
| The peer callback runs on that same stack | `on_msg` copies with `memcpy` only — no 4 KB arrays, no large `%s` logging |
| Signaling held a 4 KB `offer` **plus** two 4 KB buffers in `sdp_exchange` | all SDP buffers are `malloc`'d |
| `state_set()` invoked the app callback (which takes the LVGL lock) **on a peer thread** | state changes are marshalled through a queue to `ring-net-event`, which calls the app |
| Downlink ring buffer is 384 KB (4 s @ 48 kHz) | allocated with `MALLOC_CAP_SPIRAM` |
| Per-frame `malloc` in the audio callback at 48 kHz | decode into a preallocated buffer |
| `uxTaskGetStackHighWaterMark` opaque on overflow | `vApplicationStackOverflowHook` overridden to **name** the offending task |

A `vApplicationStackOverflowHook` in `ring_net.c` prints
`*** STACK OVERFLOW in task '<name>' (hwm N words)` — keep it; it turned a
week of "Guru Meditation" into one-line answers.

**Threads:** `ring-net-peer` (esp_peer main loop + DTLS), `ring-net-signal`
(HTTPS SDP exchange), `ring-net-event` (app callbacks), `qw-talk` (one talk
turn), `qw-connect` (connect + retry). Plus the BSP's 250 Hz `bsp_imu` and the
LVGL task.

---

## 5. TLS trust chain (why a pinned CA)

`maas.qianwenaiapi.com` presents **only its leaf certificate** (`*.maas.qianwenaiapi.com`,
issued by *GlobalSign GCC R46 OV TLS CA 2025*); the intermediate is not sent.
ESP-IDF's bundle contains roots only, so chain building fails with
`mbedtls_ssl_handshake returned -0x3000` (`MBEDTLS_ERR_X509_FATAL_ERROR`).

`components/ringkit/assets/qianwen_ca.pem` pins the intermediate + root and is
embedded via `EMBED_TXTFILES`, then used as `cert_pem`. Regenerate it if the
endpoint changes CA:

```bash
curl -o /tmp/inter.crt http://secure.globalsign.com/cacert/gsgccr46ovtlsca2025.crt
openssl x509 -in /tmp/inter.crt -inform DER -out inter.pem
# append the "GlobalSign Root R46" block from IDF's cacrt_all.pem
openssl verify -CAfile components/ringkit/assets/qianwen_ca.pem \
  <(openssl s_client -connect maas.qianwenaiapi.com:443 </dev/null 2>/dev/null | openssl x509)
```

---

## 6. Configuration

**API key** — NVS, never source. The app looks in NVS (`qianwen`/`api_key`)
first; if absent it takes a build-time key and **persists it**, so later builds
don't need it:

```bash
idf.py -DRING_APP=06_qianwenai_push_to_talk -DQIANWEN_API_KEY="sk-ws-..." \
       -B build/06_qianwenai_push_to_talk build
```

`main/CMakeLists.txt` turns the `-D` into a compile definition. Do **not**
commit a real key (`sdkconfig`/`build/` are gitignored; source is not).

**`main/idf_component.yml`**
```yaml
espressif/esp_peer: "^1.5.8"          # WebRTC PeerConnection (+ esp_libsrtp)
espressif/esp_audio_codec: "^2.6.2"   # Opus
```

**`sdkconfig.defaults`** (shared by all apps)
```ini
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
CONFIG_MBEDTLS_X509_CREATE_C=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y      # TLS buffers in PSRAM
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_LWIP_SNTP_MAX_SERVERS=3           # aliyun + pool.ntp.org
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y        # N16R8
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

**`partitions.csv`** — WebRTC + DTLS + SRTP + Opus push the image past the
1500K `SINGLE_APP_LARGE` partition, so the project carries a 3000K app
partition:

```
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        3000K,
```

Measured: `06_qianwenai_push_to_talk` ≈ 2.78 MB, 9 % free of the 3000K partition.

---

## 7. Build & flash

```bash
idf.py -DRING_APP=06_qianwenai_push_to_talk -DQIANWEN_API_KEY="sk-..." \
       -B build/06_qianwenai_push_to_talk build
idf.py -B build/06_qianwenai_push_to_talk -p /dev/ttyACM0 flash
idf.py -B build/06_qianwenai_push_to_talk -p /dev/ttyACM0 monitor
```

Use the **same `-B`** for build/flash/monitor. The other apps share `ringkit`
and `sdkconfig.defaults`, so verify them too (`00_glyph_grid_reveal`, `09_hello_smoke_test`).

---

## 8. Known limitation: this network blocks UDP

The full handshake was exercised on the bench and **everything up to the media
path works**:

```
ring_net: opus encoder: in 1920 B out 180 B
ring_net: clock synced: 2026-11-04 13:20:38
ring_net: remote audio: codec 3, 48000 Hz, 2 ch     ← Opus negotiated
ring_net: local SDP ready
ring_net: SDP answer: 1259 bytes                    ← HTTP 200, Answer parsed
PEER_DEF: Extract SDP: stream_count=2 ...
AGENT: 0 Select pair 47.117.138.191:3478
AGENT: 0 Candidate responded
AGENT: 0 Connection OK 47.117.138.191:3478          ← ICE complete
PEER_DEF: Start DTLS role as 0
DTLS: CLient handshake fail ret -0x6800             ← MBEDTLS_ERR_SSL_TIMEOUT
```

The media endpoint (`47.117.13x.xxx:3478`, `typ host`) is **unreachable from
this LAN**: TCP/443 to the API works, but *all* outbound UDP is dropped (STUN
binding requests and a synthetic DTLS ClientHello both time out, on 3478, 443
and 5060). So the DTLS handshake cannot complete here.

**To finish verification, run on a network without UDP filtering** — a phone
hotspot is enough. Expected next log lines: `DTLS: handshake success`,
`data channel open`, `session configured`, `state -> ready`, then hold the
button.

If the media endpoint is still silent on an open network, the next thing to
check is NAT traversal: the Answer pins a single `typ host` candidate, so
hairpinning/firewall behaviour on the router matters. TURN is not offered by
this endpoint.

---

## 9. Verification checklist

1. `00_glyph_grid_reveal`, `09_hello_smoke_test` and `06_qianwenai_push_to_talk` all build from clean dirs.
2. Vendored overrides still win (`espressif__esp_codec_dev`,
   `viewesmart__smartring_plus`).
3. Boot log: `ringkit:` summary all `ok`, `app '06_qianwenai_push_to_talk' is on screen`,
   no `panic` / `assert` / `Guru Meditation`.
4. On an unfiltered network: `state -> ready`, then press-and-hold → `listening`
   → release → `replying` → audio out of the speaker.
5. **Touch and voice need a human** — no host-side way to synthesise a press or
   judge the reply audio.

---

## 10. References

- Qianwen platform: <https://www.qianwenai.com> · docs index
  <https://platform.qianwenai.com/docs/llms.txt>
- WebRTC access: `platform.qianwenai.com/docs/api-reference/realtime-api/webrtc-access`
- Client events: `.../api-reference/qwen-audio-realtime/client-events`
- Model page (audio formats): `www.qianwenai.com/models/qwen-audio-3.1-realtime-flash`
- In-repo: `notes/qianwen-ai-platform.md` (models & APIs), `notes/networking-https-webrtc.md`
  (HTTPS/WebRTC background), `examples/04_voice_loopback/` (worker-task pattern)
