/*
 * ringkit — battery. See ring_battery.h.
 */
#include "ringkit/ring_battery.h"

#include "bsp/smartring_plus.h"

ring_pmic_t ring_battery_pmic_type(void)
{
    switch (bsp_battery_get_pmic_type()) {
    case BSP_PMIC_V1_ADC:     return RING_PMIC_V1_ADC;
    case BSP_PMIC_V2_AXP2101: return RING_PMIC_V2_AXP2101;
    default:                  return RING_PMIC_UNKNOWN;
    }
}

void ring_battery_get(ring_battery_data_t *out)
{
    if (out == NULL) {
        return;
    }
    bsp_battery_data_t d;
    bsp_battery_get_data(&d);
    out->voltage_v = d.voltage_v;
    out->percent   = d.percent;
    out->charging  = d.charging;
}

float ring_battery_voltage(void)
{
    return bsp_battery_get_voltage();
}

int ring_battery_percent(void)
{
    return bsp_battery_get_percent();
}

bool ring_battery_is_charging(void)
{
    return bsp_battery_is_charging();
}
