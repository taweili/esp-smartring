/*
 * ringkit — power: soft power-off via GPIO47 (V1 and V2 identical).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Soft power-off.
 *
 * Holds the PW_OFF pin for ~3.5 s and normally does NOT return: the device
 * cuts power. Show a "powering off" hint in the UI *before* calling this.
 */
void ring_power_shutdown(void);

#ifdef __cplusplus
}
#endif
