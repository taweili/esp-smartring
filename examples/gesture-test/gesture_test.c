/*
 * gesture-test — report every touch gesture the ring can produce.
 *
 * The CST816S touch controller reports a SINGLE contact point
 * (POINT_NUM_MAX == 1), so LVGL's multi-touch recognizers (pinch, rotate,
 * two-finger swipe) can never trigger on this hardware. This example
 * therefore exercises the complete set of single-finger gestures that LVGL
 * can report, and shows each one's name on screen the moment it happens:
 *
 *   press        LV_EVENT_PRESSED            finger went down
 *   release      LV_EVENT_RELEASED           finger went up
 *   tap          LV_EVENT_SHORT_CLICKED      short press + release, no movement
 *   hold         LV_EVENT_LONG_PRESSED       pressed for > long_press_time (400 ms)
 *   hold ...     LV_EVENT_LONG_PRESSED_REPEAT  repeats every repeat_time while held
 *   swipe <dir>  LV_EVENT_GESTURE            moved > gesture_limit (50 px)
 *
 * The screen is made non-scrollable so a swipe is never consumed by a scroll
 * handler. A small green dot follows the finger while it is down. Every event
 * is also logged to the serial console so it can be captured from a script.
 *
 * See examples/README.md and notes/esp-smartring.md for the event details.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <stdio.h>

#define TAG "gesture-test"

/* ------------------------------------------------------------- geometry */

#define TITLE_Y   40   /* top title baseline                          */
#define NAME_Y    158  /* big gesture name (slightly above centre)    */
#define DETAIL_Y  224  /* small detail line                           */
#define HINT_Y    296  /* bottom hint                                 */

#define DOT_SIZE  22   /* finger-follow dot diameter (px)             */
#define DOT_R     (DOT_SIZE / 2)

/* ------------------------------------------------------------- UI state */

static lv_obj_t *s_gesture_label;  /* big centre name                   */
static lv_obj_t *s_detail_label;   /* small detail line                 */
static lv_obj_t *s_touch_dot;      /* follows the finger while pressed  */

static uint32_t s_hold_repeat = 0; /* repeats counted for the current hold */

/* ------------------------------------------------------------- helpers */

/* Colour the name per gesture family so tap/hold/swipe read at a glance. */
static lv_color_t color_for(lv_event_code_t code)
{
    switch (code) {
    case LV_EVENT_SHORT_CLICKED:       return RING_COLOR_GREEN;      /* tap     */
    case LV_EVENT_LONG_PRESSED:
    case LV_EVENT_LONG_PRESSED_REPEAT: return lv_color_hex(0xFF9800);/* hold    */
    case LV_EVENT_GESTURE:             return lv_color_hex(0x00BCD4);/* swipe   */
    default:                           return lv_color_hex(0xB0BEC5);/* press/rel */
    }
}

static const char *dir_name(lv_dir_t dir)
{
    switch (dir) {
    case LV_DIR_LEFT:   return "left";
    case LV_DIR_RIGHT:  return "right";
    case LV_DIR_TOP:    return "up";
    case LV_DIR_BOTTOM: return "down";
    default:            return "?";
    }
}

/* Put a gesture on screen and in the log. */
static void show_gesture(lv_event_code_t code, const char *name, const char *detail)
{
    lv_label_set_text(s_gesture_label, name);
    lv_obj_set_style_text_color(s_gesture_label, color_for(code), LV_PART_MAIN);
    lv_label_set_text(s_detail_label, detail ? detail : "");

    if (detail != NULL && detail[0] != '\0') {
        ESP_LOGI(TAG, "gesture: %-14s (%s)", name, detail);
    } else {
        ESP_LOGI(TAG, "gesture: %s", name);
    }
}

/* ------------------------------------------------------------- input */

static void input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    char detail[40];

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(indev, &p);
        snprintf(detail, sizeof(detail), "x=%d y=%d", (int)p.x, (int)p.y);
        show_gesture(code, "press", detail);
        s_hold_repeat = 0;
        /* Drop the dot under the finger. */
        lv_obj_remove_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_touch_dot, p.x - DOT_R, p.y - DOT_R);
        break;

    case LV_EVENT_PRESSING:
        /* Finger is moving: follow it. No label update (too chatty). */
        lv_indev_get_point(indev, &p);
        lv_obj_set_pos(s_touch_dot, p.x - DOT_R, p.y - DOT_R);
        break;

    case LV_EVENT_RELEASED:
        lv_indev_get_point(indev, &p);
        snprintf(detail, sizeof(detail), "x=%d y=%d", (int)p.x, (int)p.y);
        show_gesture(code, "release", detail);
        lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
        break;

    case LV_EVENT_SHORT_CLICKED:
        show_gesture(code, "tap", "short press + release");
        break;

    case LV_EVENT_LONG_PRESSED:
        s_hold_repeat = 0;
        show_gesture(code, "hold", "held 400 ms");
        break;

    case LV_EVENT_LONG_PRESSED_REPEAT:
        s_hold_repeat++;
        snprintf(detail, sizeof(detail), "repeat #%lu", (unsigned long)s_hold_repeat);
        show_gesture(code, "hold", detail);
        break;

    case LV_EVENT_GESTURE: {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        snprintf(detail, sizeof(detail), "direction: %s", dir_name(dir));
        char name[24];
        snprintf(name, sizeof(name), "swipe %s", dir_name(dir));
        show_gesture(code, name, detail);
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        /* A swipe must reach the gesture handler, never a scroll handler. */
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        /* Title. */
        lv_obj_t *title = ring_ui_add_label(scr, "GESTURE TEST",
                                            &lv_font_montserrat_20, RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        /* Big gesture name. */
        s_gesture_label = ring_ui_add_label(scr, "--",
                                            &lv_font_montserrat_48,
                                            lv_color_hex(0xB0BEC5));
        ring_ui_place_centered(s_gesture_label, RING_CX, NAME_Y);

        /* Detail line. */
        s_detail_label = ring_ui_add_label(scr, "touch & swipe",
                                           &lv_font_montserrat_20,
                                           lv_color_hex(0x607D8B));
        ring_ui_place_centered(s_detail_label, RING_CX, DETAIL_Y);

        /* Bottom hint. */
        lv_obj_t *hint = ring_ui_add_label(scr, "tap  ·  hold  ·  swipe",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x455A64));
        ring_ui_place_centered(hint, RING_CX, HINT_Y);

        /* Finger-follow dot: a plain rounded rectangle, inert to input. */
        s_touch_dot = lv_obj_create(scr);
        lv_obj_set_size(s_touch_dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_touch_dot, RING_COLOR_GREEN, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_touch_dot, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_touch_dot, 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_touch_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);

        /* Register every single-finger gesture event on the screen. */
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_GESTURE, NULL);
    }

    ESP_LOGI(TAG, "ready: tap / hold / swipe (and press / release) are reported");
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "gesture-test";
}
