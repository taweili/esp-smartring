/*
 * ringkit — LVGL helpers: lock discipline + object creation.
 *
 * Every LVGL call must run on the adapter's task under its lock. The scoped
 * RING_UI_LOCKED macro is the safe default: it unlocks on every exit path,
 * including `return` and `break`, which a hand-written lock/unlock pair will
 * happily leak.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Take the LVGL lock (-1 == wait forever). */
esp_err_t ring_ui_lock(int32_t timeout_ms);

/** Release the LVGL lock. */
void ring_ui_unlock(void);

/**
 * @brief Run the following block (or single statement) holding the LVGL lock.
 *
 * @code
 * RING_UI_LOCKED(-1) {
 *     lv_obj_t *scr = ring_ui_screen();
 * }
 * @endcode
 *
 * Runs exactly once and always unlocks, even on early `return` / `break`.
 */
#define RING_UI_LOCKED(timeout_ms_)                              \
    for (int _ring_locked = (ring_ui_lock(timeout_ms_) == ESP_OK); \
         _ring_locked;                                            \
         _ring_locked = (ring_ui_unlock(), 0))

/**
 * @brief Active screen, cleared to the theme background.
 *
 * Caller must hold the lock.
 */
lv_obj_t *ring_ui_screen(void);

/**
 * @brief Create a label on @p parent. Caller must hold the lock.
 *
 * @param color  use lv_color_white() etc; pass RING_COLOR_BG to start hidden
 */
lv_obj_t *ring_ui_add_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color);

/**
 * @brief Centre @p obj exactly on the point (x, y). Caller must hold the lock.
 *
 * LVGL's area centre is x + (w - 1) / 2, so naive positioning leaves every
 * glyph half off-centre. This also forces a layout pass first, otherwise
 * lv_obj_get_width()/height() still return 0 for freshly created labels.
 */
void ring_ui_place_centered(lv_obj_t *obj, int x, int y);

#ifdef __cplusplus
}
#endif
