/*
 * Smart Ring Plus (VIEWE) — Circular app-grid launcher
 *
 * Replicates the @KINDOFDEVICE_ demo: a radial icon grid that zooms open
 * from the screen center, holds a fixed green ring while accent icons cycle
 * hue (orange -> pink -> blue -> cream), and collapses back to a tiny
 * central cluster on tap. See notes/launcher-spec.md for the full design.
 */
#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the circular app-grid launcher UI on the active LVGL screen.
 *
 * Must be called after bsp_display_init() (LVGL task running). All LVGL work
 * happens under the esp_lv_adapter lock. Returns ESP_OK on success.
 */
esp_err_t launcher_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_H */
