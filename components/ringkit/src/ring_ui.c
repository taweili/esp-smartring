/*
 * ringkit — LVGL helpers. See ring_ui.h.
 */
#include "ringkit/ring_ui.h"

#include "esp_lv_adapter.h"
#include "ringkit/ring_theme.h"

esp_err_t ring_ui_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void ring_ui_unlock(void)
{
    esp_lv_adapter_unlock();
}

lv_obj_t *ring_ui_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, RING_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    return scr;
}

lv_obj_t *ring_ui_add_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

void ring_ui_place_centered(lv_obj_t *obj, int x, int y)
{
    if (obj == NULL) {
        return;
    }
    /* Layout must be up to date or a fresh label reports size 0. */
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj,
                   x - (lv_obj_get_width(obj) - 1) / 2,
                   y - (lv_obj_get_height(obj) - 1) / 2);
}
