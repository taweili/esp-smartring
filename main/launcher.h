/*
 * Smart Ring Plus (VIEWE) — Circular app-grid launcher
 *
 * Replicates the @KINDOFDEVICE_ demo: a full-screen grid of glyphs that
 * lights up from the center outward (a reveal wave, no scaling), holds a
 * fixed green ring while accent glyphs cycle hue (orange -> pink -> blue ->
 * cream), and collapses back to a tiny central cluster on tap. See
 * notes/launcher-spec.md for the full design.
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
