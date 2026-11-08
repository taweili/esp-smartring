/*
 * 09_hello_smoke_test — bring-up smoke test.
 *
 * Deliberately the smallest possible UI: a white full-screen background and one
 * large black "Hello" glyph. If this renders square, correctly oriented and
 * without stripes, then the panel, its colour order, the QSPI link, the LVGL
 * adapter task and the backlight are all sane — so when a richer example looks
 * wrong, the fault is in that example, not the board.
 *
 * Everything goes through ringkit: ring_ui_screen() / ring_ui_add_label() /
 * ring_ui_place_centered() under RING_UI_LOCKED, and ring_theme.h for the
 * geometry and colours. The only departure from the theme is the inverted
 * palette (RING_COLOR_FG as the background): the point of this app is to drive
 * every pixel white, which also makes a stuck pixel, a wrong colour order or a
 * partial QSPI flush immediately visible.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#define TAG "09_hello_smoke_test"

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        /* White full-screen background — exercises every pixel. */
        lv_obj_t *scr = ring_ui_screen();
        lv_obj_set_style_bg_color(scr, RING_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *label = ring_ui_add_label(scr, "Hello",
                                            &lv_font_montserrat_48,
                                            RING_COLOR_BG);
        ring_ui_place_centered(label, RING_CX, RING_CY);
    }

    lv_display_t *disp = ring_display_handle();
    ESP_LOGI(TAG, "display %dx%d, color format %d, swap_bytes %d — Hello on screen",
             disp ? (int)lv_display_get_horizontal_resolution(disp) : -1,
             disp ? (int)lv_display_get_vertical_resolution(disp) : -1,
             disp ? (int)lv_display_get_color_format(disp) : -1,
             (int)ring_display_get_swap_bytes());
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "09_hello_smoke_test";
}
