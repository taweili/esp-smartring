/*
 * hello — bring-up smoke test.
 *
 * Deliberately the smallest possible UI: a white full-screen background and one
 * large black "Hello" glyph. If this renders square, correctly oriented and
 * without stripes, then the panel, its colour order, the QSPI link, the LVGL
 * adapter task and the backlight are all sane — so when a richer example looks
 * wrong the fault is in that example, not the board.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#define TAG "hello"

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        /* White background is the point: it exercises every pixel. */
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *label = ring_ui_add_label(scr, "Hello", &lv_font_montserrat_48,
                                            lv_color_black());
        lv_obj_center(label);
    }

    ESP_LOGI(TAG, "Hello World on screen");
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "hello";
}
