/*
 * ringkit — IMU: QMI8658A accelerometer with EMA filtering + level calibration.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "ringkit/ring_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Latest filtered acceleration in board axes, m/s² (thread safe). */
void ring_imu_accel(float *ax, float *ay, float *az);

/**
 * @brief Screen-plane tilt vector, m/s², zero point removed (thread safe).
 *
 * @p gx points along screen +x (right), @p gy along screen +y (down).
 * Before calibration succeeds these are the raw screen-plane components.
 */
void ring_imu_tilt(float *gx, float *gy);

/* ---- level calibration (optional; reference implementation) ---- */

/** (Re)start calibration: resets the window and the 30 s deadline. */
void ring_imu_calib_start(void);

/** Poll state — typical UI cadence is every 100..200 ms. */
ring_imu_calib_t ring_imu_calib_poll(void);

/**
 * @brief Calibration observables, for progress UI / debugging.
 *
 * Any pointer may be NULL.
 */
void ring_imu_calib_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms);

#ifdef __cplusplus
}
#endif
