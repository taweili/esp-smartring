# 03_gesture_probe — every touch gesture the ring can produce

The CST816S touch controller reports a **single contact point**
(`POINT_NUM_MAX == 1`), so LVGL's multi-touch recognisers (pinch, rotate,
two-finger swipe) can never trigger on this hardware. This app exercises the
complete set of single-finger gestures LVGL *can* report and shows each one's
name on screen the moment it happens:

| Gesture | Event | Meaning |
|---|---|---|
| press | `LV_EVENT_PRESSED` | finger went down |
| release | `LV_EVENT_RELEASED` | finger went up |
| tap | `LV_EVENT_SHORT_CLICKED` | short press + release, no movement |
| hold | `LV_EVENT_LONG_PRESSED` | pressed > `long_press_time` (400 ms) |
| hold … | `LV_EVENT_LONG_PRESSED_REPEAT` | repeats every `repeat_time` while held |
| swipe `<dir>` | `LV_EVENT_GESTURE` | moved > `gesture_limit` (50 px) |

A small green dot follows the finger while it is down.

## Why the screen is non-scrollable

The screen is marked `LV_OBJ_FLAG_SCROLLABLE`-free so a swipe is never consumed
by a scroll handler and always reaches the gesture recogniser.

## Build

```bash
tools/dev.sh build 03_gesture_probe
tools/dev.sh flash 03_gesture_probe
```

## Notes

- Every event is also logged, so a serial capture proves the state machine even
  when the screen cannot be seen.
- **Needs a human** — touch cannot be synthesised from the host.
