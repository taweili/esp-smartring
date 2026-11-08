/*
 * ringkit — battery: AXP2101 fuel gauge (V2) or GPIO1 ADC divider (V1).
 *
 * All getters are thread safe, so they can be called from LVGL callbacks or
 * from a periodic tick without extra locking.
 */
#pragma once

#include "esp_err.h"
#include "ringkit/ring_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which power path was detected.
 *
 * @return RING_PMIC_V2_AXP2101 or RING_PMIC_V1_ADC.
 */
ring_pmic_t ring_battery_pmic_type(void);

/**
 * @brief Latest snapshot (voltage / percent / charging).
 *
 * @param out  Destination snapshot; thread safe.
 */
void ring_battery_get(ring_battery_data_t *out);

/**
 * @brief Battery voltage.
 *
 * @return voltage in volts.
 */
float ring_battery_voltage(void);

/**
 * @brief Battery state of charge.
 *
 * @return 0..100.
 */
int ring_battery_percent(void);

/**
 * @brief Charging state.
 *
 * @return true when charging or externally powered.
 */
bool ring_battery_is_charging(void);

#ifdef __cplusplus
}
#endif
