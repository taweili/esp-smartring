# Networking, HTTPS & WebRTC on the Smart Ring Plus

**Status:** reference note — the **transport layer is now implemented**
(`ring_net`, `examples/06_qianwenai_push_to_talk`, `examples/07_qianwen_realtime_harness`,
`examples/05_wifi_https_demo`); the OpenAI REST (§5.1) and WebRTC (§5.3) paths remain
unimplemented research. What ESP-IDF v6.1 offers, what fits this board, and what adding
anything further would take.

**Scope:** the ring is an **ESP32-S3-N16R8** (8 MB octal PSRAM, 16 MB flash),
Wi-Fi 2.4 GHz + BLE 5, **ES8311 audio codec + dual mic + speaker**, round
360×360 LCD, **no camera**. That last point drives the WebRTC conclusion in §4.

---

## 1. Where the repo is today

| Piece | State |
|---|---|
| Wi-Fi STA | `components/ringkit/src/ring_wifi.c` — NVS credentials, auto-reconnect, `ring_wifi_*` API |
| BLE | `components/ringkit/src/ring_bt.c` — NimBLE GATT server (`ring_bt_*`) |
| HTTPS probe | `ring_net_probe()` — `examples/05_wifi_https_demo` |
| WebRTC voice | `components/ringkit/src/ring_net.c` — `esp_peer` + Opus + Qianwen SDP exchange |
| IP apps | `05_wifi_https_demo`, `06_qianwenai_push_to_talk`, `07_qianwen_realtime_harness` |
| Config | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` and a 3000K app partition (`partitions.csv`) are already set for Wi-Fi + WebRTC headroom |

Per the repo rule, apps talk to hardware **only** through `ringkit`. Any new
network stack should get a `ring_net_*` wrapper (or extend `ringkit`), not be
called directly from `examples/<app>/`.

---

## 2. The ESP-IDF v6.1 networking stack

| Layer | Component | Notes |
|---|---|---|
| Link | `esp_wifi`, `esp_netif`, `lwip` | STA/AP; `esp_netif` gives the default STA interface |
| Security | `esp-tls` + `mbedtls` (4.x) | `esp-tls` is the simplified API; mbedTLS 4.x is the backend |
| Trust store | `esp_crt_bundle` (in `mbedtls`) | Mozilla NSS root store, compressed; `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (default) |
| HTTP client | `esp_http_client` | HTTP/HTTPS, mutual auth, keep-alive, async (HTTPS only) |
| HTTP server | `esp_http_server` | plain HTTP |
| HTTPS server | `esp_https_server` | HTTP server + `esp-tls`; `CONFIG_ESP_HTTPS_SERVER_ENABLE=y` |
| OTA | `esp_https_ota` | image download over HTTPS |
| WebSocket | `espressif/esp_websocket_client` | **managed** component; `ws://` and `wss://` |
| MQTT | `espressif/mqtt` | **managed**; MQTT 3.1.1 / 5.0, `mqtts://` |
| mDNS | `espressif/mdns` | **managed**; local service discovery |
| Time | `esp_sntp` (in `lwip`) | **required** for TLS certificate validity checks |
| Real-time media | `espressif/esp_peer` (+ `esp_webrtc`) | **managed**; WebRTC PeerConnection |

`esp_http_client`, `esp-tls`, `esp_https_server`, `esp_https_ota` and
`esp_crt_bundle` are in IDF core (`$IDF/components/...`), so they only need a
`REQUIRES` entry. The WebSocket/MQTT/mDNS/WebRTC pieces are managed components
and must be added to `main/idf_component.yml` (the repo's single manifest).

---

## 3. HTTPS

### 3.1 Outbound (client) — the workhorse

`esp_http_client` + `esp-tls`. HTTPS is on by default
(`CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y`). Server verification is **off unless
you ask for it** — the two supported ways:

* `cert_pem` / `cert_der` — a single CA (PEM string or DER buffer) for one host.
* `crt_bundle_attach = esp_crt_bundle_attach` — the full Mozilla root bundle
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, default `..._DEFAULT_FULL`). This is
  the right choice for a device that talks to arbitrary cloud APIs. If the
  server sends a **cross-signed root** (GlobalSign, used by baidu.com, does),
  also enable `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`:
  without it `esp-x509-crt-bundle` cannot build the chain and the handshake
  dies with `MBEDTLS_ERR_X509_FATAL_ERROR` (-0x3000) even though the root is
  present in the bundle.

Other useful `esp_http_client_config_t` fields:

| Field | Purpose |
|---|---|
| `timeout_ms` | network timeout; `0` = 5 s default |
| `buffer_size` / `buffer_size_tx` | RX / TX buffers (watch heap) |
| `keep_alive_enable` + `keep_alive_idle/interval/count` | reuse the TLS session across requests |
| `use_global_ca_store` | share one CA store across connections |
| `skip_cert_common_name_check` / `common_name` | hostname verification control |
| `client_cert_pem` / `client_key_pem` | mutual TLS (see `esp_http_client_mutual_auth` example) |
| `is_async` | non-blocking `perform()` — **HTTPS only** |

Minimal shape:

```c
esp_http_client_config_t cfg = {
    .url = "https://api.openai.com/v1/models",
    .method = HTTP_METHOD_GET,
    .crt_bundle_attach = esp_crt_bundle_attach,   /* needs CONFIG_MBEDTLS_CERTIFICATE_BUNDLE */
    .timeout_ms = 10000,
    .keep_alive_enable = true,
};
esp_http_client_handle_t c = esp_http_client_init(&cfg);
esp_http_client_set_header(c, "Authorization", "Bearer " CONFIG_OPENAI_API_KEY);
if (esp_http_client_perform(c) == ESP_OK) {
    int status = esp_http_client_get_status_code(c);
    int len = esp_http_client_read(c, buf, sizeof(buf) - 1);
    /* ... */
}
esp_http_client_cleanup(c);
```

**TLS versions.** mbedTLS 4.x supports **TLS 1.2 and TLS 1.3** only (TLS 1.0/1.1
and DTLS 1.0 are gone). TLS 1.2 is the default
(`CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y`); TLS 1.3 is opt-in
(`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`) and is what the IDF `esp_http_client`
example CI enables.

**Performance / RAM.** The handshake is the expensive part (X.509 + ECDHE/RSA).
ESP32-S3 has hardware acceleration for MPI/AES/SHA/ECC — leave
`CONFIG_MBEDTLS_HARDWARE_*` at their defaults. Two knobs matter on a PSRAM board:

* `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` — allocate TLS buffers from PSRAM
  (depends on `CONFIG_SPIRAM_USE_MALLOC`). The IDF `https_request` example sets
  this. Strongly recommended here, since the UI already eats PSRAM.
* `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — allocate TX/RX buffers on demand and free
  them after the exchange. **Incompatible with DTLS** (see §4.3) — so it is not
  available if WebRTC is enabled.
* `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y` (RFC 5077) — session resumption
  speeds up reconnects; `MBEDTLS_CLIENT_SSL_SESSION_TICKETS` defaults to `y`.

Reports of multi-second handshakes are mostly from older IDF (4.x). On v6.1
with hardware crypto and PSRAM buffers, a handshake is typically well under a
second on a warm connection; still, budget for it and use keep-alive.

Reference examples in `$IDF/examples/protocols/`: `esp_http_client`,
`https_request`, `https_x509_bundle`, `esp_http_client_mutual_auth`.

### 3.2 Inbound (server)

`esp_https_server` (`CONFIG_ESP_HTTPS_SERVER_ENABLE=y`) is the HTTP server
with an `esp-tls` session layer. It is the natural way to build a **Wi-Fi
provisioning portal** or a local control endpoint on the ring (the round LCD +
touch is enough for a simple "enter SSID/password" page). It supports
`ESP_HTTPS_SERVER_EVENTS`, a cert-selection hook
(`CONFIG_ESP_HTTPS_SERVER_CERT_SELECT_HOOK`), and there is a `wss_server`
example alongside `simple`. Self-signed certs are fine for local use; browsers
will warn.

### 3.3 OTA

`esp_https_ota` downloads a new image over HTTPS (cert via `cert_pem` or the
bundle). `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` defaults to `n` — keep it that way.

### 3.4 WebSocket / MQTT / mDNS / SNTP

* **`esp_websocket_client`** (managed): `ws://` and `wss://`, `cert_pem` /
  `crt_bundle_attach`, `use_global_ca_store`, `skip_cert_common_name_check`,
  `keep_alive_enable`, auto-reconnect, ping/pong. This is the transport for
  signaling and for the OpenAI Realtime WebSocket path (§5.2).
* **`espressif/mqtt`** (managed): MQTT 3.1.1 / 5.0, `mqtts://` over `esp-tls`.
  Good for telemetry/control if we ever need a broker.
* **`espressif/mdns`** (managed): advertise the ring on the LAN.
* **`esp_sntp`**: sync the clock after Wi-Fi connects. **Do this before any TLS
  request** — certificate validity periods are checked against the wall clock,
  and an unset clock makes every HTTPS connection fail with a cert error.

---

## 4. WebRTC

### 4.1 Components

| Component | Where | Role |
|---|---|---|
| `esp_peer` | registry (`espressif/esp_peer`, v1.5.8) | PeerConnection: ICE, DTLS, SCTP, RTP/SRTP, TURN. Depends on `espressif/esp_libsrtp` |
| `esp_webrtc` | solution repo (`espressif/esp-webrtc-solution`), not the registry | Orchestration: signaling + peer + media. Uses `override_path` for `esp_peer`/`media_lib_utils` |
| `esp_capture` | registry (`espressif/esp_capture`) | Audio/video capture, ESP-GMF based (pulls `esp-sr`, `esp32-camera`, `esp_codec_dev`) |
| `av_render` | registry (`tempotian/av_render`) | Push-based AV playback (`esp_audio_codec` / `esp_video_codec`) |
| `media_lib_utils` | registry (`tempotian/media_lib_utils`) | Helpers used by `esp_webrtc` |

The official demos live in `espressif/esp-webrtc-solution/solutions/`:
`peer_demo` (bare `esp_peer`), `openai_demo`, `whip_demo`, `janus_demo`,
`doorbell_demo`, `videocall_demo`, etc.

### 4.2 What `esp_peer` gives you

* **ICE** (controlling or controlled), **DTLS**, **SCTP** data channels,
  **RTP/SRTP**, **TURN** (RFC 5766 / 8656), ICE Lite, RTP NACK/RTX, SCTP
  fragmentation + SACK. Send and receive run in independent tasks.
* **Audio codecs:** G.711A (PCMA), G.711U (PCMU), **Opus**.
  **Video codecs:** H.264, MJPEG. **Data channels:** reliable/unreliable,
  ordered/unordered.
* API: `esp_peer_open()` → `esp_peer_new_connection()` → exchange SDP via
  `on_msg` → `esp_peer_send_msg()` → `ESP_PEER_STATE_CONNECTED`; then
  `esp_peer_send_audio/video/data()` and receive via
  `on_audio_info/on_audio_data`, `on_video_info/on_video_data`, `on_data`.
  `esp_peer_main_loop()` **must** run in its own task.
* `esp_peer_pre_generate_cert()` generates the DTLS certificate up front so the
  first call does not stall.
* **Signaling is not included.** `esp_peer` only produces/consumes SDP and ICE
  candidates. You supply the transport (`esp_peer_signaling_impl_t`): AppRTC
  WebSocket, WHIP (HTTPS POST), OpenAI Realtime, Janus/Kurento/KVS, or custom.

### 4.3 Requirements and config

```ini
# IDF v6 / mbedTLS 4.x
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y        # DTLS 1.2 (depends on TLS 1.2)
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y         # DTLS-SRTP (RFC 5764)
CONFIG_MBEDTLS_X509_CREATE_C=y         # required on IDF v6
```

`esp_peer` needs `CONFIG_MBEDTLS_SSL_PROTO_DTLS`, which **disables
`CONFIG_MBEDTLS_DYNAMIC_BUFFER`** (`depends on !MBEDTLS_SSL_PROTO_DTLS` in
`components/mbedtls/Kconfig`). So with WebRTC on, TLS handshakes keep their full
TX/RX buffers — another reason to use `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.

**Version compatibility (important).** `esp_peer` ships a **closed-source
prebuilt `libpeer_default.a`**. It must be compiled against the same
IDF/mbedTLS you link:

* v1.3.x and earlier: built against IDF v5.5 / mbedTLS 3.x → **crashes on
  IDF v6.x** (ABI mismatch, `Load access fault` in
  `esp_peer_pre_generate_cert()`). See
  [esp-webrtc-solution#167](https://github.com/espressif/esp-webrtc-solution/issues/167).
* **v1.4.0+**: recompiled for IDF v6.0 / mbedTLS 4.x.
* **v1.5.3+**: fixes IDF v6 / mbedTLS 4.1 first-boot DTLS handshake failures.

Use the latest v1.5.x. There is no source release for the prebuilt, so you
cannot rebuild it against a different mbedTLS — pin the IDF version.

**Resources.** `esp_peer` minimal setup is **< 60 KB RAM without PSRAM**, and
the `peer_demo` runs **two peers concurrently on an ESP32-S3**. The ring's 8 MB
PSRAM is plenty; tune `esp_peer_default_cfg_t` (`agent_recv_timeout`,
`data_ch_cfg`, `rtp_cfg` buffer sizes) down if needed.

### 4.4 Fit for this board

* **No camera → no video path.** H.264/MJPEG encode/decode and the video
  solutions (doorbell, video call, WHIP/KVS/Janus) are not applicable. Audio +
  data channel is the sweet spot.
* **Audio is a good match.** The ring already has an ES8311 codec and I2S
  mic/speaker behind `ring_audio_*`. `esp_peer` takes *encoded* frames
  (`esp_peer_audio_frame_t`), so the app can:
  * send **G.711A/U** — companding is trivial in software, zero extra
    dependencies; or
  * send **Opus** — better quality/bandwidth, but needs an Opus codec
    (`esp_audio_codec` / libopus) and more RAM.
* **`esp_webrtc` + `esp_capture` + `av_render` is the heavy path.** It pulls in
  ESP-GMF, `esp_audio_codec`, `esp-sr`, camera drivers, and more RAM. For the
  ring, driving `esp_peer` directly with `ring_audio_*` is leaner and keeps the
  "apps talk to hardware through ringkit" rule.
* **Signaling / NAT.** WebRTC needs a signaling server, and P2P across home
  NATs usually needs STUN (and sometimes TURN). The ring is a Wi-Fi STA behind
  a router, so budget for a signaling service and a STUN/TURN config.
* **Wi-Fi + BLE coexistence** is already in use (`ring_bt` + `ring_wifi`);
  WebRTC adds sustained UDP traffic — watch for throughput/latency regressions.

### 4.5 Risks

* Prebuilt ABI: pin `esp_peer` ≥ v1.4 (latest v1.5.x) and match IDF v6.1.
* DTLS vs `MBEDTLS_DYNAMIC_BUFFER`.
* No camera: don't plan a video feature.
* Signaling server is a hard requirement; TURN may be needed for reliable NAT
  traversal.
* App partition is 1500K (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`). WebRTC
  + TLS + an audio codec will grow the image; check the size after integrating.

---

## 5. OpenAI API support

There are three viable paths. All are HTTPS/TLS at the edge, so §3 applies.

### 5.1 REST API over HTTPS — `espressif/openai` (component)

`espressif/openai` v1.2.0 (managed; port of the OpenAI-ESP32 Arduino library,
IDF ≥ 4.4) wraps the REST endpoints with cJSON + `esp_http_client` +
`esp_crt_bundle`:

| API | Support |
|---|---|
| Chat Completions (text / image / audio input) | yes (no streaming chat) |
| Audio: Speech (TTS, streaming) & Transcription (Whisper) | yes |
| Images: create / edit / variations | yes |
| Embeddings | yes |
| Moderations | yes |
| Responses API | no (use Chat Completions) |

Usage: `OpenAICreate(api_key)` → get a `chatCompletion` / `audioTranscription`
/ `audioSpeech` / `imageGeneration` / `embedding` object → call its methods.
`OpenAIChangeBaseURL()` points it at a proxy or an OpenAI-compatible endpoint.
Audio transcription buffers need PSRAM — the ring has 8 MB, so fine.

Add with `idf.py add-dependency "espressif/openai=*"` (or a line in
`main/idf_component.yml`). Store the API key in NVS, not in source.

**Best for:** text Q&A, TTS playback, Whisper STT, embeddings. Not realtime
(conversation latency = full request/response).

### 5.2 Realtime API over WebSocket — recommended for voice

The OpenAI Realtime API accepts a plain WebSocket:

```
wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1
Authorization: Bearer <OPENAI_API_KEY>
```

* Audio is **PCM16 (24 kHz) base64** in both directions (also `g711_ulaw` /
  `g711_alaw`); JSON events for session config, function calls, etc.
* Full-duplex, low-latency speech-to-speech.
* Transport is `esp_websocket_client` + `esp-tls` — **no `esp_peer`, no DTLS,
  no ICE/SRTP, no signaling server.** This is the least-dependency realtime
  path and a natural first voice feature on the ring.

**Reference implementations** (all read for this note):

| Project | Path | Notes |
|---|---|---|
| [RASPIAUDIO/OpenDino](https://github.com/RASPIAUDIO/OpenDino) | raw TLS WebSocket, **direct** | ESP32-WROVER, Arduino, 650-line `opendino.ino`. PCM16 24 kHz, PTT, JSON-Schema `move()` function call, captive portal → NVS. ~400 ms PTT latency. |
| [zouchanglin/embedded-projects](https://github.com/zouchanglin/embedded-projects) | raw TLS WebSocket, direct | ESP32-S3, ESP-IDF C. Qwen Omni / Doubao Realtime (same protocol shape). Best write-up of AEC, fragmentation, jitter buffer, barge-in. |
| [akdeb/ElatoAI](https://github.com/akdeb/ElatoAI) | secure WebSocket **via Deno/Supabase edge** | ESP32-S3, Arduino/PlatformIO. Multi-model (OpenAI, Gemini, xAI, ElevenLabs). Not direct. |
| [DAME (Notre Dame)](http://seniordesign.ee.nd.edu/2025/DesignTeams/tars/documents/DAME_Final_Report.pdf) | WebSocket **via Node.js/Hono relay** | ESP32-S3 senior design; LangChain tools. Relay pattern. |
| [openai/openai-realtime-embedded](https://github.com/openai/openai-realtime-embedded) (`esp32` branch) | WebRTC (libpeer) | C++, Opus 8 kHz. Superseded by Espressif `openai_demo` (§5.3). |
| [t-cool/stackchan-idf](https://github.com/t-cool/stackchan-idf) | WebSocket, direct | M5Stack CoreS3; half-duplex, barge-in, expression tools. |

**Protocol shape** (from OpenDino, verified in source):

```
wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-mini-preview
Authorization: Bearer <key>
OpenAI-Beta: realtime=v1
```

1. `session.created` → send `session.update` (`input_audio_format`/`output_audio_format` = `pcm16`, `modalities: [text, audio]`, `turn_detection: null`, `tools`).
2. Mic PCM16 → `input_audio_buffer.append` (base64). On end-of-turn → `input_audio_buffer.commit` → wait `input_audio_buffer.committed` → `response.create`.
3. Downlink: `response.audio.delta` (base64 PCM16) → decode → playback ring buffer; `response.audio.done` stops.
4. Function calls: on `response.done`, run the tool, then `conversation.item.create` with a `function_call_output`, then `response.create`.

**Hard-won lessons** (zouchanglin write-up, ESP32-S3 + ESP-IDF):

* **WebSocket fragmentation.** `esp_websocket_client` splits large text events across callbacks. Reassemble with `payload_offset`/`payload_len`; only parse JSON when `payload_offset + data_len >= payload_len`. A 20 KB `response.audio.delta` arrives in pieces.
* **Audio clock is I2S, not the network.** Split mic / uplink / WS-callback / RX / playback into separate tasks; the uplink queue drops the oldest frame instead of growing.
* **Playback jitter buffer.** 300–800 ms ring in PSRAM; log `audio delta gap` and `playback underflow`.
* **Barge-in.** Stop local playback fast and send `response.cancel`.
* **VAD.** Use the model's built-in `semantic_vad` rather than rolling your own.

**Echo cancellation is the real work.** The ring has a speaker next to the
mic; without AEC the assistant hears itself. OpenDino sidesteps it with PTT
(half-duplex). For full-duplex, use ESP-SR AFE: `afe_aec_create("MR", 4,
AFE_TYPE_FD, AFE_MODE_LOW_COST)` with **mic + playback reference** interleaved
`M0,R0,M1,R1…`. The reference must be the **volume-scaled** PCM actually sent
to the amp, resampled 24 kHz→16 kHz (3:2), with an ~80 ms acoustic-delay window.
Options: `esp_capture`'s AEC source (ESP-GMF), `esp-sr`, or a software AEC. The
Espressif `openai_demo` uses the ESP32-S3-Korvo-2 board specifically for its AEC
capability.

### 5.3 Realtime API over WebRTC — `esp_webrtc` `openai_demo`

The official Espressif demo (`solutions/openai_demo`) uses the ephemeral-token
flow:

1. HTTPS `POST /v1/realtime/client_secrets` to get a short-lived client secret.
2. HTTPS `POST /v1/realtime/calls` to exchange SDP (WebRTC offer/answer).
3. WebRTC media (Opus) over `esp_webrtc` → `esp_peer`, with `esp_capture` for
   mic input and `av_render` for playback.

Verified on the **ESP32-S3-Korvo-2**. This is the most capable path (and the
one OpenAI recommends for browser/mobile), but it needs the full media stack
(§4.4) and is considerably heavier than §5.2.

> **Realtime beta WebRTC is dead.** OpenAI shut the beta interface down on
> 2026-05-07. Examples that use the beta signaling shape — the
> `openai-realtime-embedded` `esp32` branch and `brookesia_agent_openai` — now
> fail with HTTP 400 `beta_api_shape_disabled` (see
> [esp-brookesia#104](https://github.com/espressif/esp-brookesia/issues/104)).
> This `openai_demo` uses the **GA** ephemeral-token flow and is the current
> official WebRTC path; WebSocket (§5.2) is unaffected.

### 5.4 Recommendation

| Need | Path |
|---|---|
| Text / TTS / Whisper / embeddings | `espressif/openai` over HTTPS (§5.1) |
| Realtime voice assistant (first version) | Realtime over **WebSocket** (§5.2) |
| Realtime voice with P2P media / OpenAI WebRTC flow | `esp_webrtc` + `esp_peer` (§5.3) |

Start with §5.2 for voice: it needs only `esp_websocket_client` + `esp-tls` and
none of the WebRTC config in §4.3. Add WebRTC only if you specifically need
P2P media or the WebRTC transport.

---

## 6. What adding this took (and what is still open)

**Shipped** in `main/idf_component.yml`: `espressif/esp_peer` (WebRTC) and
`espressif/esp_audio_codec` (Opus), plus `esp-tls`/`esp_http_client`/`mbedtls` via
`ringkit`'s `REQUIRES` and the shared `sdkconfig.defaults` block below. Still
**unimplemented** and therefore still optional here: `espressif/openai` (REST),
`espressif/mqtt`, `espressif/mdns`, and `esp_websocket_client` (needed only by the
planned `examples/10_qianwen_ws_voice_chat` — see `notes/design-ws-voice-chat.md`).

```yaml
dependencies:
  idf: ">=5.5"
  viewesmart/smartring_plus: "^1.1.2"
  # HTTPS / REST / voice
  espressif/openai: "*"              # optional, REST
  espressif/esp_websocket_client: "*"# ws/wss (planned 10_qianwen_ws_voice_chat)
  # espressif/mqtt: "*"              # optional
  # espressif/mdns: "*"              # optional
  # WebRTC
  espressif/esp_peer: "^1.5.8"       # pulls espressif/esp_libsrtp
  # espressif/esp_capture: "~1.1"    # only for the full media stack
  # tempotian/av_render: "*"
  # tempotian/media_lib_utils: "*"
```

**`sdkconfig.defaults`** (shared by all apps):

```ini
# HTTPS client (defaults are already y, listed for clarity)
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
# Put TLS buffers in PSRAM (needs CONFIG_SPIRAM_USE_MALLOC)
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
# Session resumption (optional)
CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y

# WebRTC only:
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
CONFIG_MBEDTLS_X509_CREATE_C=y

# HTTPS server only:
# CONFIG_ESP_HTTPS_SERVER_ENABLE=y
```

**`components/ringkit/`**: add a `ring_net` module (or extend `ringkit`) so
apps never include `esp_http_client` / `esp_tls` / `esp_websocket_client`
directly; add `esp-tls esp_http_client` (and `esp_websocket_client` if used)
to `ringkit`'s `REQUIRES`.

**Credentials**: move the hardcoded default SSID/password out of
`ring_wifi.h` (or at least stop committing real credentials), and store API
keys in NVS. `ring_wifi` already persists Wi-Fi credentials in NVS — follow the
same pattern.

---

## 7. Gotchas

* **Console back-pressure** (see `AGENTS.md`): `ESP_LOG*` blocks when the host
  stops reading `/dev/ttyACM0`. Network debugging that floods the log will
  stall the firmware and look like a hang. Drain the port continuously.
* **TLS needs the correct time.** Call `esp_sntp` after Wi-Fi connects, before
  the first HTTPS request, or cert validation fails.
* **`esp_http_client` `is_async` is HTTPS-only.**
* **`esp_websocket_client` fragments large messages.** Reassemble by
  `payload_offset`/`payload_len` before parsing JSON (§5.2).
* **Realtime beta WebRTC is dead** (§5.3). Use the GA token flow or WebSocket.
* **`esp_peer` is a closed-source prebuilt** — version must match IDF/mbedTLS
  (§4.5). Pin `esp_peer` ≥ v1.4 on IDF v6.1.
* **DTLS disables `CONFIG_MBEDTLS_DYNAMIC_BUFFER`.** Plan TLS memory with
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`.
* **WebRTC needs signaling + likely STUN/TURN.** Budget for a server.
* **No camera.** Video WebRTC is out of scope.
* **API keys are secrets.** NVS, not source; the existing `ring_wifi.h`
  default credentials are a bad example to copy.
* **Image size.** App partition is **3000K** (`partitions.csv`); 06 is 2718 KB,
  07 is 2640 KB. Measure after adding any WebRTC/audio dependency.

---

## 8. Verification plan

1. **Apps build** from clean dirs (`00_glyph_grid_reveal`, `09_hello_smoke_test`, plus the
   networking apps `05`/`06`/`07`) — they share `ringkit`, so a change that breaks one can
   break the others.
2. **HTTPS** (shipped): `ring_net_probe()` in `examples/05_wifi_https_demo`.
3. **WebSocket** — still unexercised: connect to a `wss://` echo server, round-trip a message.
4. **WebRTC** (shipped through the SDP + ICE stages): `examples/06`/`07` reach `ICE complete`,
   but DTLS cannot complete on a network that drops outbound UDP (see 06 §8).
5. **OpenAI**: one REST chat completion; one Realtime WebSocket PCM16 loopback — not started.
6. **On-device**: mic/speaker echo, latency and UI behaviour need a human —
   there is no host-side way to synthesise a tap or a voice conversation.
