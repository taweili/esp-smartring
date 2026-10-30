/*
 * ringkit — board bring-up.
 *
 * Encodes the ORDER the Smart Ring Plus hardware must be initialised in, which
 * is not obvious from the individual bsp_* calls:
 *
 *   bsp_display_init() creates the shared I2C bus AND starts the LVGL task,
 *   so it must be first. Everything else either reuses that bus (battery V2,
 *   audio codec, IMU) or depends on the LVGL adapter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What came up during ring_board_init(). */
typedef struct {
    bool display_ok;   /**< panel + touch + LVGL adapter + backlight    */
    bool battery_ok;   /**< AXP2101 (V2) or GPIO1 ADC (V1) probed       */
    bool power_ok;     /**< soft power-off GPIO armed                   */
    bool audio_ok;     /**< ES8311 + I2S                                */
    bool sd_ok;        /**< SDMMC mounted; FALSE is normal with no card */
    bool imu_ok;       /**< QMI8658A sampling task running              */
    bool battery_v2;   /**< TRUE when an AXP2101 was found              */
} ring_board_t;

/**
 * @brief Bring the board up in the required order.
 *
 * display / battery / power / audio are mandatory — the first failure is
 * returned and later stages are skipped. SD and IMU are optional (no
 * card-detect pin; IMU may be absent) and only reported in @p out.
 *
 * @param out  optional summary, filled even on partial success
 */
esp_err_t ring_board_init(ring_board_t *out);

/** Log one line per subsystem: OK / FAILED. */
void ring_board_log_summary(const ring_board_t *b);

/** Shared I2C bus (SDA=8/SCL=9/400kHz). NULL before ring_board_init(). */
i2c_master_bus_handle_t ring_board_i2c_bus(void);

#ifdef __cplusplus
}
#endif
