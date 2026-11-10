/*
 * 13_button_probe — show every BOOT-button event on screen as it happens.
 *
 * The Smart Ring Plus has one button the ESP32 can read: the BOOT button on
 * GPIO0 (active-low, 10 kΩ pull-up). ring_button decodes it into:
 *
 *   press         debounced press-down
 *   long press    held for 800 ms (once per press)
 *   click         short press + release (fires immediately on release)
 *   double click  a second press + release within 300 ms of the last release
 *
 * The screen shows the latest event big in the centre, a live "held N ms"
 * line while the button is down, and a running count of each event. Every
 * event is also logged to the serial console.
 *
 * NOTE: GPIO0 is a strapping pin. Holding BOOT while the ring powers up or
 * resets enters the ROM bootloader — do not hold it across a reset.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <stdio.h>

#define TAG "13_button_probe"

/* ------------------------------------------------------------- geometry */

#define TITLE_Y    44    /* title baseline                              */
#define DOT_CY     92    /* live pressed-indicator centre               */
#define DOT_R      12    /* indicator radius                            */
#define NAME_Y     150   /* big event name                              */
#define DETAIL_Y   215   /* live state / held time                      */
#define COUNTER_Y  265   /* per-event counters                          */
#define HINT_Y     320   /* bottom hint                                 */

#define UI_PERIOD_MS 50  /* live-state refresh (held ms, dot)           */

/* Counter labels are spread across the safe circle at COUNTER_Y. */
static const int k_counter_x[RING_BUTTON_EVT_COUNT] = { 55, 135, 215, 295 };

/* ------------------------------------------------------------- UI state */

static lv_obj_t *s_name_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_dot;
static lv_obj_t *s_counter_labels[RING_BUTTON_EVT_COUNT];

static const char *const s_names[RING_BUTTON_EVT_COUNT] = {
    "press", "long press", "click", "dbl click",
};

static const char *const s_counter_titles[RING_BUTTON_EVT_COUNT] = {
    "press", "long", "click", "dbl",
};

static uint32_t s_counts[RING_BUTTON_EVT_COUNT];

/* ------------------------------------------------------------- helpers */

static lv_color_t color_for(ring_button_evt_t evt)
{
    switch (evt) {
    case RING_BUTTON_EVT_PRESS:        return lv_color_hex(0x42A5F5); /* blue   */
    case RING_BUTTON_EVT_LONG_PRESS:   return lv_color_hex(0xFF9800); /* orange */
    case RING_BUTTON_EVT_CLICK:        return RING_COLOR_GREEN;       /* green  */
    case RING_BUTTON_EVT_DOUBLE_CLICK: return lv_color_hex(0xE040FB); /* purple */
    default:                           return lv_color_white();
    }
}

static void update_counters(void)
{
    for (int i = 0; i < RING_BUTTON_EVT_COUNT; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %lu", s_counter_titles[i],
                 (unsigned long)s_counts[i]);
        lv_label_set_text(s_counter_labels[i], buf);
    }
}

/* ------------------------------------------------------------- button */

static void on_button(ring_button_evt_t evt, void *user)
{
    (void)user;

    if (evt < 0 || evt >= RING_BUTTON_EVT_COUNT) {
        return;
    }

    s_counts[evt]++;
    ESP_LOGI(TAG, "button: %s", s_names[evt]);

    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_name_label, s_names[evt]);
        lv_obj_set_style_text_color(s_name_label, color_for(evt), LV_PART_MAIN);
        update_counters();
    }
}

/* ------------------------------------------------------------- UI timer */

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    bool pressed = ring_button_is_pressed();
    uint32_t held = ring_button_held_ms();

    RING_UI_LOCKED(-1) {
        lv_obj_set_style_bg_color(s_dot,
                                 pressed ? RING_COLOR_GREEN
                                         : lv_color_hex(0x37474F),
                                 LV_PART_MAIN);
        if (pressed) {
            lv_label_set_text_fmt(s_detail_label, "held %lu ms",
                                  (unsigned long)held);
            lv_obj_set_style_text_color(s_detail_label, RING_COLOR_GREEN,
                                        LV_PART_MAIN);
        } else {
            lv_label_set_text(s_detail_label, "idle");
            lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x90A4AE),
                                        LV_PART_MAIN);
        }
    }
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        /* Title. */
        lv_obj_t *title = ring_ui_add_label(scr, "BUTTON",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        /* Live pressed indicator. */
        s_dot = lv_obj_create(scr);
        lv_obj_set_size(s_dot, DOT_R * 2, DOT_R * 2);
        lv_obj_set_pos(s_dot, RING_CX - DOT_R, DOT_CY - DOT_R);
        lv_obj_set_style_radius(s_dot, DOT_R, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x37474F), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_dot, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_dot, 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Big event name. */
        s_name_label = ring_ui_add_label(scr, "--", &lv_font_montserrat_48,
                                         lv_color_hex(0xB0BEC5));
        ring_ui_place_centered(s_name_label, RING_CX, NAME_Y);

        /* Detail line (live state). */
        s_detail_label = ring_ui_add_label(scr, "idle", &lv_font_montserrat_16,
                                           lv_color_hex(0x90A4AE));
        ring_ui_place_centered(s_detail_label, RING_CX, DETAIL_Y);

        /* Per-event counters. */
        for (int i = 0; i < RING_BUTTON_EVT_COUNT; i++) {
            s_counter_labels[i] = ring_ui_add_label(scr, "",
                                                    &lv_font_montserrat_14,
                                                    RING_COLOR_FG);
            ring_ui_place_centered(s_counter_labels[i], k_counter_x[i],
                                   COUNTER_Y);
        }
        update_counters();

        /* Bottom hint. */
        lv_obj_t *hint = ring_ui_add_label(scr, "press the BOOT button",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x546E7A));
        ring_ui_place_centered(hint, RING_CX, HINT_Y);

        lv_timer_create(ui_tick, UI_PERIOD_MS, NULL);
    }

    /* Start the decoder only after the UI exists, so the callback can never
     * touch a NULL label. */
    ring_button_config_t cfg = RING_BUTTON_CONFIG_DEFAULT(on_button, NULL);
    esp_err_t err = ring_button_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_button_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ready: press / hold / click / double-click the BOOT button");
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "13_button_probe";
}
