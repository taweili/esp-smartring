# 07_qianwen_realtime_harness — hands-free Qianwen voice loop

A reference example for the complete speech-to-speech path on the Smart Ring
Plus:

```
board init → Wi-Fi → SNTP → HTTPS probe → WebRTC session
            → [ mic → mono → Opus → Qianwen → Opus → speaker ] → repeat
```

It uses the **real microphone and speaker** and the **Qianwen realtime model**
(`qwen-audio-3.1-realtime-flash`) over WebRTC.

## The network gate

The audio loop is not entered until networking is proven. `qw-connect` runs:

1. **Wi-Fi** — `ring_wifi_init()` + `ring_wifi_start()` +
   `ring_wifi_wait_connected()`. Credentials come from NVS (seeded from
   `secrets.local` on first boot).
2. **Clock** — `ring_net_sync_clock()` (SNTP). TLS certificate validation needs
   a correct wall clock; without this every HTTPS connection fails.
3. **Probe** — `ring_net_probe("https://baidu.com")`: a real HTTPS GET that
   exercises DNS, routing, the Mozilla root bundle and HTTP.
4. **Session** — `ring_net_start()`: the Qianwen SDP exchange, DTLS-SRTP and the
   DataChannel.

Each step is shown on the screen (`wi-fi…`, `network check…`, `session…`) and
retried with a 5 s backoff, so a failure is diagnosable instead of silent.

## Audio pipeline

`ring_audio` is 48 kHz / 16-bit / **stereo interleaved**; `ring_net` is
48 kHz / 16-bit / **mono**. The example only folds/expands at the boundary:

* **Uplink** — `ring_audio_read()` a 20 ms stereo frame, fold L/R to mono,
  `ring_net_send_audio()`.
* **Downlink** — `ring_net_read_audio()`, expand mono to stereo,
  `ring_audio_set_mute(false)` → `ring_audio_write()` → `ring_audio_set_mute(true)`
  (the mute sequence avoids speaker pops).

## Turn-taking: hands-free, half-duplex

WebRTC supports only server-side VAD (`server_vad`); manual turn-taking is
WebSocket-only. So:

1. Open the uplink with `ring_net_set_listening(true)`.
2. Stream mic frames while `ring_net_state() == RING_NET_LISTENING`.
3. The server VAD detects the end of speech; the first reply frame flips the
   state to `RING_NET_SPEAKING`, which ends the uplink — **the model never hears
   itself**.
4. Play the reply until the ring is drained and the state returns to
   `RING_NET_READY`.
5. Repeat.

This is half-duplex: the uplink is closed while the model speaks. The ring has
no acoustic echo cancellation (the ES8311 has no AEC and the speaker sits next
to the mic), so full-duplex would need an AEC feed — see
`notes/networking-https-webrtc.md` §5.2 for the ESP-SR AFE recipe.

## Task layout

| Task | Stack | RAM | Role |
|---|---|---|---|
| `qw-connect` | 8192 words (32 KB) | internal | Wi-Fi + SNTP + probe + session, retries |
| `qw-turn` | 65536 words (256 KB) | **PSRAM** | the audio loop |
| `ring-net-peer` | 16384 words | internal | `esp_peer` main loop + DTLS |
| `ring-net-signal` | 4096 words | internal | HTTPS SDP exchange |
| `ring-net-event` | 6144 words | internal | app callbacks |

The Opus codec needs **more than 96 KB** of stack. FreeRTOS allocates task
stacks from internal RAM only (`pvPortMalloc` → `MALLOC_CAP_INTERNAL`), so
`qw-turn` is created with `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`. A
plain `xTaskCreate` of that size fails outright.

## API key

The key is read from NVS (`qianwen`/`api_key`); if absent, the build-time
`QIANWEN_API_KEY` (from `secrets.local` via `tools/dev.sh`) is used and
persisted. See `examples/06_qianwenai_push_to_talk/README.md` §3.

## Build & run

```bash
tools/dev.sh build 07_qianwen_realtime_harness
tools/dev.sh flash 07_qianwen_realtime_harness
tools/dev.sh monitor
```

The screen shows the current step, the link state/IP, the recognised user
utterance and the model transcript. The log prints `turn: listen`,
`turn: reply`, `turn: done (N samples)` and the transcript.

> **Needs a human:** the actual voice quality, latency and turn-taking feel
> cannot be verified from the log. The log proves the pipeline runs, not that
> the conversation is intelligible.
