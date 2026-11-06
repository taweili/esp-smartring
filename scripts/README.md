# scripts/ — host-side helpers

## test_qianwen_realtime.py

Tests the Qianwen realtime WebRTC API from the host, using the same flow as
`examples/qianwenai-button` (SDP POST → ICE/DTLS/SRTP → session.update →
Opus uplink → data-channel events → spoken reply), with verbose logging.

By default it uses the **real microphone and speaker**: speak to the model and
hear its reply. The microphone is stopped as soon as the reply starts, so the
model does not hear itself through the speaker. A clip can be streamed instead
for a deterministic test, and the reply is always saved to a WAV file.

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
    --clip examples/qianwen-realtime/assets/test_clip.pcm
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
ICE; `--no-stun` disables it.

> **Feedback:** if you play the reply through speakers while the microphone is
> live, the model can hear itself. The script stops the microphone when the
> first reply frame arrives, but headphones are still the safest option.

### Output

The script prints every step (SDP exchange, ICE/DTLS state, `session.update`,
`dc:` data-channel events, transcript) and writes the spoken reply to
`scripts/reply.wav` (or `--out`).

The clip streamed by `examples/qianwen-realtime` is
`examples/qianwen-realtime/assets/test_clip.pcm`, generated with:

```bash
espeak -v en -s 150 -w /tmp/capital.wav "the capital of China"
ffmpeg -y -i /tmp/capital.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
       examples/qianwen-realtime/assets/test_clip.pcm
```
