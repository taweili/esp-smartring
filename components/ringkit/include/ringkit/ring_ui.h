/*
 * ringkit — LVGL helpers: lock discipline + object creation.
 *
 * Every LVGL call must run on the adapter's task under its lock. The scoped
 * RING_UI_LOCKED macro is the safe default: it unlocks on every exit path,
 * including `return` and `break`, which a hand-written lock/unlock pair will
 * happily leak.
 */
#pragma once

#include <stdbool.h>
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

/* Lock handle with a cleanup handler. The cleanup runs on every exit path
 * from the following block, including `return`, `break` and `goto`, which a
 * plain lock/unlock pair (or a for-loop increment) cannot do. */
typedef struct {
    bool ok;
} ring_ui_lock_t;

static inline ring_ui_lock_t ring_ui_lock_make(int32_t timeout_ms)
{
    ring_ui_lock_t l = { .ok = (ring_ui_lock(timeout_ms) == ESP_OK) };
    return l;
}

static inline void ring_ui_unlock_cleanup(ring_ui_lock_t *l)
{
    if (l->ok) {
        ring_ui_unlock();
        l->ok = false;
    }
}

/**
 * @brief Run the following block (or single statement) holding the LVGL lock.
 *
 * @code
 * RING_UI_LOCKED(-1) {
 *     lv_obj_t *scr = ring_ui_screen();
 * }
 * @endcode
 *
 * Runs exactly once and always unlocks, including on early `return` / `break`
 * / `goto`. If the lock cannot be taken the block is skipped.
 */
#define RING_UI_LOCKED(timeout_ms_)                                    \
    for (int _ring_once = 0; _ring_once < 1; _ring_once++)             \
        for (ring_ui_lock_t _ring_lock                                 \
                 __attribute__((cleanup(ring_ui_unlock_cleanup))) =    \
                 ring_ui_lock_make(timeout_ms_);                      \
             _ring_lock.ok;                                            \
             _ring_lock.ok = false)

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
