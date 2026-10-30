/*
 * ringkit — shared value types for the Smart Ring Plus.
 *
 * These mirror the viewesmart/smartring_plus BSP structs/enums so that
 * applications never need to include the BSP header directly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- battery */

/** Which power-management path was detected at runtime. */
typedef enum {
    RING_PMIC_UNKNOWN = 0,
    RING_PMIC_V1_ADC,      /**< GPIO1 ADC divider estimate (no AXP2101) */
    RING_PMIC_V2_AXP2101,  /**< AXP2101 fuel gauge / charger / ALDO3     */
} ring_pmic_t;

/** Battery snapshot (thread safe). */
typedef struct {
    float voltage_v;  /**< battery voltage in volts                       */
    int   percent;    /**< state of charge 0..100                          */
    bool  charging;   /**< true = charging or externally powered           */
} ring_battery_data_t;

/* ------------------------------------------------------------- SD card */

typedef enum {
    RING_SD_FILE = 0,
    RING_SD_DIR,
} ring_sd_entry_type_t;

/** One directory entry. `name` is the entry name, not a path. */
typedef struct {
    char *name;
    ring_sd_entry_type_t type;
} ring_sd_entry_t;

/* ------------------------------------------------------------- IMU */

/** Level-calibration state machine (bsp_imu_calib_poll). */
typedef enum {
    RING_IMU_CALIB_IDLE = 0,  /**< not started                          */
    RING_IMU_CALIB_RUNNING,   /**< sampling / deciding                  */
    RING_IMU_CALIB_PASSED,    /**< passed, zero point recorded          */
    RING_IMU_CALIB_FAILED,    /**< conditions not met within 30 s       */
} ring_imu_calib_t;

#ifdef __cplusplus
}
#endif
