/*
 * ringkit — display: ST77916 QSPI panel behind the LVGL adapter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Backlight brightness, 0..100 (LEDC PWM). */
void ring_display_backlight(uint8_t percent);

/** LVGL display handle (for lv_display_set_color_format etc.). */
lv_display_t *ring_display_handle(void);

/**
 * @brief Switch RGB565 byte order at runtime (panel revision / debugging).
 *
 * Repaints the whole screen. Wrong setting shows up as vertical stripes.
 */
void ring_display_set_swap_bytes(bool enable);
bool ring_display_get_swap_bytes(void);

#ifdef __cplusplus
}
#endif
