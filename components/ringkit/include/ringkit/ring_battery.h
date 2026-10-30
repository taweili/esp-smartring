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

/** Which power path was detected. */
ring_pmic_t ring_battery_pmic_type(void);

/** Latest snapshot (voltage / percent / charging). */
void ring_battery_get(ring_battery_data_t *out);

/** Convenience queries. */
float ring_battery_voltage(void);
int   ring_battery_percent(void);
bool  ring_battery_is_charging(void);

#ifdef __cplusplus
}
#endif
