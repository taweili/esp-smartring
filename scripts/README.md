# scripts/ — host-side helpers

## test_qianwen_realtime.py

Tests the Qianwen realtime WebRTC API from the host, using the same flow as
`examples/07_qianwen_realtime_harness` (SDP POST → ICE/DTLS/SRTP → session.update →
Opus uplink → data-channel events → spoken reply), with verbose logging.

By default it uses the **real microphone and speaker**: speak to the model and
hear its reply. The microphone is stopped as soon as the reply starts, so the
model does not hear itself through the speaker. A clip can be streamed instead
for a deterministic test, and the reply is always saved to a WAV file.

### When to speak

The uplink is gated until the session is configured — the server rejects a
`turn_detection` update once it has started processing audio — so anything said
during the handshake (~5 s) is silently dropped. The script therefore only tells
you to talk once the microphone is actually live:

```
[qianwen-rt + 0.0s] input: microphone — connecting; do not speak until the SPEAK NOW cue
...
────────────────────────────────────────────────────────────────────────
  SPEAK NOW — the microphone is live. Your turn ends after 800 ms of silence;
  the reply follows (one turn per run)
────────────────────────────────────────────────────────────────────────
...
────────────────────────────────────────────────────────────────────────
  REPLY — starts once the reply has fully arrived, then plays at the card's
  rate; microphone muted. Nothing to do, just listen
────────────────────────────────────────────────────────────────────────
```

It is one turn per run: the microphone is closed when the reply starts (so the
model cannot hear itself), hence `DONE — run it again for another turn`.

Between the cue and the reply the script narrates the uplink every few seconds
(see **Output**), so a run where nothing happens tells you *why*: silent capture
device, audio too quiet for the VAD, or audio sent but no turn recognised.

### Setup

The script needs `aiortc` (WebRTC), `aiohttp` (HTTPS), `av` (audio frames and
the Opus codec) and `sounddevice` (microphone / speaker). A venv is provided:

```bash
python3 -m venv scripts/.venv
scripts/.venv/bin/pip install aiortc aiohttp av sounddevice
```

No `numpy`: audio frames are converted with `ctypes` (the g-buddy `frame_bytes`
approach), so PyAV's `AudioFrame.to_ndarray()` is never called.

### Usage

```bash
# live microphone -> model -> speaker (default)
scripts/.venv/bin/python scripts/test_qianwen_realtime.py

# use the espeak clip instead of the microphone (raw PCM or any av-decodable
# audio file — a WAV works directly, no ffmpeg step needed)
scripts/.venv/bin/python scripts/test_qianwen_realtime.py \
    --clip scripts/assets/test_clip.pcm
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip /tmp/clip.wav

# save the reply somewhere else, without playing it
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --no-play --out /tmp/reply.wav

# pick a specific device (see --list-devices)
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --mic-device 0 --speaker-device 1

# try another voice or model
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --voice longanhuan_v3.1
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --model qwen-audio-3.1-realtime-plus

# generate a clip with espeak
espeak -v en -s 150 -w /tmp/clip.wav "the capital of China"
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip /tmp/clip.wav
```

`--list-devices` prints the audio devices and exits. `--timeout` (default 60 s)
bounds the wait for `response.done`; the reply is considered finished after
`--silence` seconds (default 2) without a new audio frame and with the speaker
queue empty, up to the `--drain` hard cap (default 15 s).

### Playback modes

`--playback` chooses how the spoken reply is played:

| Mode | Behaviour |
|---|---|
| `buffer` (default) | wait for the whole reply to arrive, then play it back-to-back at the sound card's rate |
| `live` | stream the reply as the frames arrive |
| `adaptive` | start once `--cushion` seconds (default 0.6) are buffered, and re-cushion after a stall |

`buffer` is the default because a player fed as the frames arrive plays at the
*delivery* rate: g-buddy measured a 1.9 s answer whose last frame arrived at
4.8 s, so a live raw player stretched it to ~40% speed (the "speaker at half
speed" bug). `buffer` trades the delivery time for correct speed.

`--stun` (default `stun:stun.l.google.com:19302`) supplies a STUN server for
ICE; `--no-stun` disables it. Gathering candidates blocks until the STUN
transaction finishes — about **5 s** here, during which the script prints
`gathering ICE candidates …` and nothing else. `--no-stun` POSTs immediately
(the server accepts host candidates) and is the quicker local loop.

> **Feedback:** if you play the reply through speakers while the microphone is
> live, the model can hear itself. The script stops the microphone when the
> first reply frame arrives, but headphones are still the safest option.

### Output

The script prints every step (SDP exchange, ICE/DTLS state, `session.update`,
the model's transcript) and writes the spoken reply to `scripts/reply.wav` (or
`--out`). Each line carries the elapsed time, which is the point of a realtime
test. `-v`/`--verbose` adds the raw `dc:` data-channel JSON for every event
(off by default, so the lines you have to react to are not buried).

While it waits for `response.done` it logs the uplink every 5 s — frames sent,
seconds sent and the peak sample — because "the model never answers" has two
very different causes:

```
uplink: 251 frames (5.0 s) sent, peak 109/32768 (-50 dBFS); no reply yet
uplink: 251 frames (5.0 s) sent, all digital silence — the model cannot hear
        anything. Wrong or muted capture device? check --list-devices / --mic-device
```

A peak of `0` means the capture device delivered pure zeros (muted headset,
wrong device, or the model is on the other end of a monitor source) — the
network and session may still be perfectly fine. A low but non-zero peak means
the VAD is refusing the audio; speak up or check the mic gain.

`--dump-sdp PATH` writes the local SDP offer next to the log, for comparing
against what the ring's `ring_net` generates.

### Exit code

The exit code is the test result, so it can be used from a script or cron job:

| Code | Meaning |
|---|---|
| `0` | a reply was received (and saved, unless `--out` was unwritable) |
| `1` | no reply: `--timeout` expired, the server reported an error, the SDP/ICE/session handshake failed, or the API key / audio device / clip was unusable |

A muted run (`--no-play`) still exits `0` only if audio arrived — the transport
working is not enough.

### `--clip` formats

A file whose header is not a known container magic (`RIFF`, `ID3`/MP3 frame
sync, `OggS`, `fLaC`, `FORM`, `ftyp`, ...) is treated as headerless 48 kHz /
16-bit / mono s16le PCM, which is what the ring examples stream. Everything
else is decoded and resampled with PyAV, so a 44.1 kHz stereo WAV or an MP3
works as-is — no `ffmpeg` step needed. A raw PCM file in any other format
(44.1 kHz, stereo, 8-bit) is sent as-is and will sound wrong; convert it first.

The clip used by the `--clip` examples below is `scripts/assets/test_clip.pcm`
(generated with:

```bash
espeak -v en -s 150 -w /tmp/capital.wav "the capital of China"
ffmpeg -y -i /tmp/capital.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
       scripts/assets/test_clip.pcm
```

## test_qianwen_ws_keepalive.py

Probes the Qianwen realtime **WebSocket** session lifetime and the keep-alive
strategy used by `examples/10_qianwen_ws_voice_chat`. The server closes a
session that has not generated a response for 180 s
(`response_idle_timeout`), so the app re-sends `session.update` every 60 s.

```bash
scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy none
scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy session_update
scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy response_create
scripts/.venv/bin/python scripts/test_qianwen_ws_keepalive.py --strategy silence
```

`none` shows the 180 s timeout; `session_update` is the strategy the app uses
(resets the timer, produces no reply). `--duration` defaults to 210 s.
