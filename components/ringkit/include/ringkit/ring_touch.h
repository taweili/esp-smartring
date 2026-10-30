/*
 * ringkit — capacitive touch (CST816), already registered as an LVGL indev.
 */
#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Touch handle, or NULL before ring_board_init().
 *
 * Normal UI code does not need this: the adapter already wired the touch
 * panel to LVGL, so use LV_EVENT_CLICKED / LV_EVENT_GESTURE on objects.
 * This is for reading raw coordinates.
 */
esp_lcd_touch_handle_t ring_touch_handle(void);

#ifdef __cplusplus
}
#endif
