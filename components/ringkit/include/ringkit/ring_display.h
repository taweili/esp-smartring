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

/**
 * @brief Set the backlight brightness.
 *
 * @param percent  0..100 (LEDC PWM duty).
 */
void ring_display_backlight(uint8_t percent);

/**
 * @brief LVGL display handle (for lv_display_set_color_format etc.).
 *
 * @return the display handle, valid after ring_board_init().
 */
lv_display_t *ring_display_handle(void);

/**
 * @brief Switch RGB565 byte order at runtime (panel revision / debugging).
 *
 * Repaints the whole screen. Wrong setting shows up as vertical stripes.
 *
 * @param enable  true to swap the bytes.
 */
void ring_display_set_swap_bytes(bool enable);

/**
 * @brief Current RGB565 byte-swap setting.
 *
 * @return true when byte swapping is enabled.
 */
bool ring_display_get_swap_bytes(void);

#ifdef __cplusplus
}
#endif
