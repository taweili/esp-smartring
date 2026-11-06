# 05_wifi_https_demo — Wi-Fi station bring-up + HTTPS probe

Demonstrates the `ring_wifi` and `ring_net` modules end to end:

1. `ring_wifi_start()` loads credentials from NVS (seeded from the local
   `secrets.local` on first boot, then persisted) and connects in station mode.
2. The screen shows the SSID, the station state and the IP, refreshed at 5 Hz.
3. Once associated, a task syncs the clock over SNTP and GETs
   `https://baidu.com` to prove outbound TLS works. The HTTP status is shown on
   screen.

```
tools/dev.sh build 05_wifi_https_demo
tools/dev.sh flash 05_wifi_https_demo
```

## Credentials

SSID/password come from the git-ignored `./secrets.local` (see
`README.md` → Quick start). Without them `ring_wifi_start()` returns
`ESP_ERR_NOT_FOUND` and the app keeps running with an explanatory status rather
than aborting.

## Why the clock sync first

The public-CA certificate check compares against the wall clock and the ring has
no battery-backed RTC, so the clock must be valid before the TLS handshake. That
is `ring_net_sync_clock()`; the GET itself is `ring_net_probe()`. This app only
owns the retry loop and the UI.

## Why a retry loop

The probe task follows the link for the life of the app: wait for the station,
probe, and on failure or a lost link go back to waiting. Without it the probe
would only ever run once, so an AP that was down at boot would leave the screen
stuck on "no IP" until the next reboot. `ring_wifi` reconnects on its own after a
reboot or an AP dropout.

## Notes

- `log_dns()` is kept because it separates "cannot resolve the host" from
  "cannot connect to it".
- **Needs a human** for the on-screen result; the log shows the same status.
