# 13_button_probe — every BOOT-button event the ring can produce

The Smart Ring Plus has one button the ESP32 can read: the **BOOT button**,
wired to **GPIO0** (active-low, 10 kΩ pull-up to 3V3, button to GND). The other
tactile switch is the power key, which goes to the EC190707 PMIC and is not a
GPIO — firmware cannot read it, only simulate its long press through GPIO47
(`ring_power_shutdown()`).

`ring_button` decodes the raw level into four events and this app shows each
one on screen the moment it happens:

| Event | Meaning |
|---|---|
| `press` | debounced press-down (20 ms settle) |
| `long press` | held for `long_press_ms` (800 ms), once per press |
| `click` | short press + release — fires immediately on release |
| `dbl click` | a second press + release within `double_click_ms` (300 ms) of the last release |

The screen shows the latest event big in the centre, a live **held N ms** line
while the button is down, a running count of each event, and a green dot that
is lit while the button is pressed. Every event is also logged to the serial
console.

## Why CLICK fires immediately

`click` is emitted on release, not after the double-click window, so a
double-click produces `click` then `dbl click` (the usual "click + double
click" pair). A long press suppresses the `click` for that press, and a long
second press suppresses the `dbl click`.

## Timings

| Parameter | Default | Meaning |
|---|---|---|
| `debounce_ms` | 20 | level must be stable this long before it is accepted |
| `long_press_ms` | 800 | hold before `long press` (0 disables) |
| `double_click_ms` | 300 | window for the second click (0 disables) |
| `poll_ms` | 10 | button task sample rate |

All are fields of `ring_button_config_t`; `RING_BUTTON_CONFIG_DEFAULT()` fills
the standard values.

## Build

```bash
idf.py -DRING_APP=13_button_probe -B build/13_button_probe build
idf.py -DRING_APP=13_button_probe -B build/13_button_probe -p /dev/ttyACM0 flash
```

## Notes

- **Needs a human** — the button cannot be synthesised from the host.
- **GPIO0 is a strapping pin.** Holding BOOT while the ring powers up or resets
  enters the ROM bootloader. Do not hold it across a reset.
- The callback runs on `ring_button`'s own task, so it takes
  `RING_UI_LOCKED(-1)` before touching LVGL objects. `ring_button_is_pressed()`
  and `ring_button_held_ms()` are lock-free snapshots for a UI timer.
- To use a different button, change `cfg.gpio` (and `active_high` / the pulls
  if needed) before `ring_button_init()`.
