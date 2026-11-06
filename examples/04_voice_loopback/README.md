# 04_voice_loopback — mic + speaker loop-back

One big round button. **Hold** to record from the ES8311 microphone; **release**
to play the captured PCM straight back through the speaker. The progress bar
shows the capture buffer filling while recording and the audio remaining while
playing back. Captures up to **8 s**.

```
tools/dev.sh build 04_voice_loopback
tools/dev.sh flash 04_voice_loopback
```

## Why a worker task

`ring_audio_read()` / `ring_audio_write()` are **blocking**, and button events are
delivered on the LVGL task. Recording inside the event callback would freeze the
LVGL task, so the `RELEASED` event that has to stop the capture could never be
processed. Instead `LV_EVENT_PRESSED` spawns a worker that owns the whole
record → playback sequence, and `LV_EVENT_RELEASED` simply flips a volatile stop
flag.

The worker takes `RING_UI_LOCKED(-1)` only for brief UI updates and **never**
while doing audio I/O, so rendering stays responsive and the audio path is never
stalled by the display.

## Audio

Format is fixed by the BSP: **48 kHz / 16-bit / stereo interleaved** (~192 KB/s).
The capture buffer lives in PSRAM (~1.5 MB) and holds up to 8 s; when it fills,
recording stops and the clip plays back automatically. The clip is deliberately
kept in RAM rather than on the SD card, so this example has no filesystem
dependency (see `ring_sd_*` for persistent storage).

## Notes

- The first chunk is read unconditionally, so even a quick tap records something.
- **Needs a human** — audio quality and the button feel cannot be verified from
  the log.
