#!/usr/bin/env python3
"""Regenerate the ring_geodis mock clip from a WAV file.

The mock transport (CONFIG_RINGKIT_GEODIS_MOCK) plays a canned reply so the
UI, the state machine and barge-in can be developed with no network. The clip
is embedded as raw PCM with EMBED_TXTFILES, which keeps it out of the source
tree and off the compiler's critical path.

    tools/gen_geodis_mock_clip.py                     # scripts/reply.wav
    tools/gen_geodis_mock_clip.py --src other.wav --seconds 2.0

The output is mono 48 kHz / 16-bit little-endian, trimmed to --seconds and
peak-normalised to -3 dBFS. Only the mock reads it, so any speech-like clip
works; a real recorded reply is better because it exercises the same format
the speaker path expects.
"""
import argparse
import os
import struct
import sys
import wave

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(REPO, "scripts", "reply.wav")
DEFAULT_DST = os.path.join(REPO, "components", "ringkit", "assets",
                           "geodis_mock_reply.pcm")
TARGET_HZ = 48000
PEAK = 0.7          # ~-3 dBFS


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default=DEFAULT_SRC, help="source WAV")
    ap.add_argument("--dst", default=DEFAULT_DST, help="output raw PCM")
    ap.add_argument("--seconds", type=float, default=1.6,
                    help="length to keep (default 1.6)")
    args = ap.parse_args()

    with wave.open(args.src) as w:
        ch, rate, width = w.getnchannels(), w.getframerate(), w.getsampwidth()
        frames = w.readframes(w.getnframes())
    if width != 2:
        print(f"{args.src}: {width * 8}-bit, expected 16", file=sys.stderr)
        return 1

    n = len(frames) // 2
    samples = list(struct.unpack("<%dh" % n, frames))

    # Fold to mono if the source is stereo.
    if ch == 2:
        samples = [(samples[2 * i] + samples[2 * i + 1]) // 2
                   for i in range(n // 2)]
    elif ch != 1:
        print(f"{args.src}: {ch} channels, expected 1 or 2", file=sys.stderr)
        return 1

    # Resample by simple decimation only when the rate is a clean multiple;
    # otherwise refuse rather than silently pitch-shifting the clip.
    if rate != TARGET_HZ:
        if rate % TARGET_HZ == 0:
            step = rate // TARGET_HZ
            samples = samples[::step]
        elif TARGET_HZ % rate == 0:
            step = TARGET_HZ // rate
            samples = [s for s in samples for _ in range(step)]
        else:
            print(f"{args.src}: {rate} Hz is not a multiple of {TARGET_HZ}",
                  file=sys.stderr)
            return 1

    samples = samples[: int(args.seconds * TARGET_HZ)]
    peak = max(1, max(abs(s) for s in samples))
    gain = (32767 * PEAK) / peak
    samples = [max(-32768, min(32767, int(s * gain))) for s in samples]

    os.makedirs(os.path.dirname(args.dst), exist_ok=True)
    with open(args.dst, "wb") as f:
        f.write(struct.pack("<%dh" % len(samples), *samples))

    print(f"{args.dst}: {len(samples)} samples "
          f"({len(samples) / TARGET_HZ:.2f} s, {len(samples) * 2} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
