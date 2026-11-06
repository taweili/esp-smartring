# 02_imu_dashboard — live IMU dashboard

Every channel the BSP exposes for the QMI8658A, on one screen:

- **bubble level** — the round panel is a natural spirit level, driven by the
  screen-plane tilt vector from `ring_imu_tilt()`
- **accelerometer** X/Y/Z (m/s²), board axes
- **gyroscope** X/Y/Z (°/s), board axes
- **die temperature** (°C; the driver convention is raw/256)
- **level-calibration state** plus its observables (`std_g`, `tilt_deg`,
  elapsed time)

## Interaction

**Tap anywhere** starts or restarts the level calibration. Before it passes, the
bubble shows the raw (un-zeroed) tilt; once it passes on a flat surface the
bubble re-centres, which makes the calibration visibly work.

## Refresh

A single `lv_timer` at **10 Hz**. The BSP already samples at 250 Hz and
EMA-filters, so the UI only reads smoothed values. The `ring_imu_*` reads take
the BSP's own mutex and are independent of the LVGL lock the timer already holds
(`RING_UI_LOCKED` inside an `lv_timer` callback is re-entrant-safe).

## Build

```bash
tools/dev.sh build 02_imu_dashboard
tools/dev.sh flash 02_imu_dashboard
```

## Notes

- Gyro and temperature come from the vendored `smartring_plus` override (see
  `AGENTS.md` § "Critical fixes"); upstream exposes neither.
- Calibration needs a genuinely flat surface and a few seconds of stillness.
  **Needs a human** — the bubble's feel cannot be judged from the log.
