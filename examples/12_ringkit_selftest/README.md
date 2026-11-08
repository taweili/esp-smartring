# 12_ringkit_selftest

A bring-up diagnostic that exercises every ringkit subsystem and reports the
result on the round screen and in the serial log.

`main.c` already brings the board up (display, battery, power, audio, SD, IMU)
and logs the `ringkit:` summary. This app goes one level deeper: it probes each
peripheral through the ringkit API, then initialises the optional modules
(Wi-Fi, BLE, and the three voice modules) and tears the voice sessions back
down.

## What it checks

| Line | Probe |
|------|-------|
| `display` | `ring_display_handle()` is live |
| `touch` | `ring_touch_handle()` is live |
| `audio` | `ring_audio_codec()` is live |
| `imu` | latest accelerometer sample (`ring_imu_accel`) |
| `sd` | `ring_sd_is_mounted()` — `no card` is normal, there is no card-detect pin |
| `battery` | `ring_battery_percent()` |
| `wifi` | `ring_wifi_init` + `ring_wifi_start`, then association / IP (up to 10 s) |
| `bt` | `ring_bt_init` + `ring_bt_start_advertising`, then the GAP state |
| `ring_net` | `ring_net_init` → `ring_net_stop` |
| `ring_ws` | `ring_ws_init` → `ring_ws_stop` |
| `ring_chat` | `ring_chat_init` (no network call) |

The voice modules are only initialised and stopped — a full voice turn needs a
network and a human (see `06`, `07`, `10`). `ring_net` and `ring_ws` have no
full deinit; their PSRAM buffers stay allocated for the life of the app, so
`stop` only proves the session teardown is safe.

## Build / flash / monitor

```bash
idf.py -DRING_APP=12_ringkit_selftest -B build/12_ringkit_selftest build
idf.py -DRING_APP=12_ringkit_selftest -B build/12_ringkit_selftest -p /dev/ttyACM0 flash
idf.py -B build/12_ringkit_selftest -p /dev/ttyACM0 monitor
```

Or, with the secrets/env wrapper:

```bash
tools/dev.sh build 12_ringkit_selftest
tools/dev.sh flash 12_ringkit_selftest
tools/dev.sh monitor
```

## Expected log

```
I (…) smartring: Smart Ring Plus starting (ringkit 1.0.0, app: 12_ringkit_selftest)
I (…) ringkit: display=ok battery=ok(AXP2101/V2) power=ok audio=ok sd=ok imu=ok
I (…) 12_ringkit_selftest: wifi: no ip (…)
I (…) 12_ringkit_selftest: bt: advertising
I (…) 12_ringkit_selftest: ring_net_init: ESP_OK
I (…) 12_ringkit_selftest: ring_ws_init: ESP_OK
I (…) 12_ringkit_selftest: ring_chat_init: ESP_OK
I (…) 12_ringkit_selftest: selftest complete
```

`wifi: no ip` is expected when `secrets.local` has no credentials or no AP is
in range; the point is that `ring_wifi_init`/`start` succeed and the state is
readable.
