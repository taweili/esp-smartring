# scripts/ — host-side helpers

## test_qianwen_realtime.py

Tests the Qianwen realtime WebRTC API from the host, using the same flow as
`examples/qianwenai-button` (SDP POST → ICE/DTLS/SRTP → session.update →
Opus uplink → data-channel events → spoken reply), with verbose logging and
the model reply saved to a WAV file.

### Setup

The script needs `aiortc` (WebRTC), `aiohttp` (HTTPS), `av` (audio frames) and
`opuslib`. A venv is provided:

```bash
python3 -m venv scripts/.venv
scripts/.venv/bin/pip install aiortc aiohttp av opuslib numpy
```

(`numpy` is used by PyAV's `AudioFrame.to_ndarray()` when writing the reply
WAV.)

### Usage

```bash
# default: use the espeak clip and the key from secrets.local / $QIANWEN_API_KEY
scripts/.venv/bin/python scripts/test_qianwen_realtime.py

# save the reply somewhere else
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --out /tmp/reply.wav

# use a different clip (48 kHz / 16-bit / mono s16le PCM)
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip /tmp/my_clip.pcm

# generate a clip with espeak
espeak -v en -s 150 -w /tmp/clip.wav "the capital of China"
ffmpeg -y -i /tmp/clip.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le /tmp/clip.pcm
scripts/.venv/bin/python scripts/test_qianwen_realtime.py --clip /tmp/clip.pcm
```

### Output

The script prints every step (SDP exchange, ICE/DTLS state, `session.update`,
`dc:` data-channel events, transcript) and writes the spoken reply to
`scripts/reply.wav` (or `--out`).

The default clip is `examples/qianwen-realtime/assets/test_clip.pcm`, which was
generated with:

```bash
espeak -v en -s 150 -w /tmp/capital.wav "the capital of China"
ffmpeg -y -i /tmp/capital.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
       examples/qianwen-realtime/assets/test_clip.pcm
```
