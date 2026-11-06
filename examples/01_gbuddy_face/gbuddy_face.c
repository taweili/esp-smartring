/*
 * 01_gbuddy_face — the GEODIS buddy face, alive on the round panel.
 *
 * The reference stills (medias/gbuddy-001..004.png) are four *expressions* of
 * the same robot, not an animation. Instead of showing a photo, this app cuts
 * the eyes and mouth out of those stills as alpha sprites (tools/ +
 * assets/rebuild.sh) and animates them as small objects on a generated
 * flat "screen" band. See examples/01_gbuddy_face/README.md for the measurements
 * behind every number and §4 for what changed once it ran on the panel.
 *
 *   happy      open eyes + smile       (001)
 *   surprised  open eyes + "o" mouth   (002)
 *   sad        droopy eyes + frown     (003)
 *   joy        closed "^" eyes + grin  (004)
 *
 * Life on top of the moods:
 *   - blink every 2.5..6 s (swap to the closed-eye sprite for ~110 ms; a
 *     double-blink happens 15 % of the time)
 *   - eyes follow gravity: ring_imu_tilt() moves them up to +/-8 px
 *   - a quick twist of the ring (gyro Z) throws the gaze sideways and it
 *     decays back to centre
 *   - long-press: sleep (closed eyes, backlight 20 %); tap wakes it
 *   - tap: next mood
 *   - battery < 15 %: forced sad face (once per crossing)
 *
 * Only small objects are invalidated while animating — never the whole face —
 * so the 40-line QSPI flush stays cheap. The background is static (and flat:
 * an RGB565 gradient bands badly over 222 px).
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define TAG "01_gbuddy_face"

/* ------------------------------------------------------------- layout */
/* Sprite positions are relative to the 360x222 face container (panel y=69).
 * Measured in examples/01_gbuddy_face/README.md §1; the sprites were cut with
 * the exact crop windows that made these numbers true. */

#define FACE_Y   69
#define FACE_W   360
#define FACE_H   222

/* Sprite top-lefts. The images size themselves from their descriptors
 * (eye 118x150, mouth 110x80), so no explicit sizes are needed here. */
#define EYE_LX   12
#define EYE_RX   224
#define EYE_Y    24

#define MOUTH_X  124
#define MOUTH_Y  146

/* --------------------------------------------------------------- feel */

#define GAZE_PERIOD_MS     40     /* IMU poll / gaze smoothing cadence      */
#define GAZE_DEADBAND       0.7f  /* m/s^2: ignore small tilts              */
#define GAZE_GAIN           2.0f  /* px per m/s^2                           */
#define GAZE_MAX_X          8     /* px; keeps the eyes on their glow       */
#define GAZE_MAX_Y          3
#define GAZE_SMOOTH         0.35f /* low-pass factor per tick (0..1)        */

#define TWIST_DEADBAND_DPS  3.0f
#define TWIST_GAIN          0.25f /* px per integrated dps*s                */
#define TWIST_MAX           12.0f
#define TWIST_DECAY         0.92f /* per tick when the ring is still        */

#define BLINK_MS            110
#define BLINK_MIN_MS       2500
#define BLINK_SPAN_MS      3500
#define BLINK_DOUBLE_PCT     15

#define BATTERY_POLL_MS    5000
#define BATTERY_LOW_PCT      15

#define BOOT_FADE_MS        700

/* ------------------------------------------------------------- assets */

LV_IMAGE_DECLARE(eye_open_l);
LV_IMAGE_DECLARE(eye_open_r);
LV_IMAGE_DECLARE(eye_joy_l);
LV_IMAGE_DECLARE(eye_joy_r);
LV_IMAGE_DECLARE(eye_sad_l);
LV_IMAGE_DECLARE(eye_sad_r);
LV_IMAGE_DECLARE(mouth_smile);
LV_IMAGE_DECLARE(mouth_o);
LV_IMAGE_DECLARE(mouth_frown);
LV_IMAGE_DECLARE(mouth_grin);

typedef struct {
    const lv_image_dsc_t *eye_l;
    const lv_image_dsc_t *eye_r;
    const lv_image_dsc_t *mouth;
    const char           *name;
} mood_def_t;

static const mood_def_t s_moods[] = {
    { &eye_open_l, &eye_open_r, &mouth_smile, "happy"     },
    { &eye_open_l, &eye_open_r, &mouth_o,     "surprised" },
    { &eye_sad_l,  &eye_sad_r,  &mouth_frown, "sad"       },
    { &eye_joy_l,  &eye_joy_r,  &mouth_grin,  "joy"       },
};
#define N_MOODS ((int)(sizeof(s_moods) / sizeof(s_moods[0])))
#define MOOD_JOY 3

/* -------------------------------------------------------------- state */

static lv_obj_t *s_face;
static lv_obj_t *s_eye[2];
static lv_obj_t *s_mouth;

static int      s_mood;
static bool     s_asleep;
static bool     s_battery_warned;

static float    s_gaze_x, s_gaze_y;      /* smoothed, px            */
static float    s_twist;                 /* integrated gyro, px     */
static int32_t  s_gaze_px, s_gaze_py;    /* last applied offset     */
static int64_t  s_last_us;

/* ------------------------------------------------------------- helpers */

static void set_eye_srcs(const lv_image_dsc_t *l, const lv_image_dsc_t *r)
{
    lv_image_set_src(s_eye[0], l);
    lv_image_set_src(s_eye[1], r);
}

static void apply_mood(int m)
{
    s_mood = m % N_MOODS;
    set_eye_srcs(s_moods[s_mood].eye_l, s_moods[s_mood].eye_r);
    lv_image_set_src(s_mouth, s_moods[s_mood].mouth);
    ESP_LOGI(TAG, "mood %s", s_moods[s_mood].name);
}

static void set_backlight_for_state(void)
{
    ring_display_backlight(s_asleep ? 20 : 100);
}


/* ------------------------------------------------------------ blink */

/* lv_anim_path_step returns the values themselves, so (1,0) means: closed at
 * the start, open at the end. 110 ms is long enough to read as a blink. */
static void blink_exec(void *var, int32_t v)
{
    (void)var;
    if (s_asleep) {
        return;
    }
    if (v) {
        set_eye_srcs(&eye_joy_l, &eye_joy_r);
    } else {
        set_eye_srcs(s_moods[s_mood].eye_l, s_moods[s_mood].eye_r);
    }
}

static uint32_t next_blink_period(void)
{
    return BLINK_MIN_MS + ring_rand_below(BLINK_SPAN_MS);
}

static void blink_cb(lv_timer_t *timer)
{
    uint32_t next = next_blink_period();

    if (!s_asleep && s_mood != MOOD_JOY) {
        RING_UI_LOCKED(-1) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, &s_gaze_x);   /* any stable address */
            lv_anim_set_exec_cb(&a, blink_exec);
            lv_anim_set_values(&a, 1, 0);
            lv_anim_set_duration(&a, BLINK_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_step);
            lv_anim_start(&a);
        }
        if ((int)ring_rand_below(100) < BLINK_DOUBLE_PCT) {
            next = BLINK_MS + 90;             /* immediate second blink */
        }
    }

    lv_timer_set_period(timer, next);
}

/* ------------------------------------------------------------- gaze */

static void apply_gaze(int32_t px, int32_t py)
{
    if (px == s_gaze_px && py == s_gaze_py) {
        return;
    }
    s_gaze_px = px;
    s_gaze_py = py;

    RING_UI_LOCKED(-1) {
        for (int i = 0; i < 2; i++) {
            lv_obj_set_style_translate_x(s_eye[i], px, 0);
            lv_obj_set_style_translate_y(s_eye[i], py, 0);
        }
    }
}

static void gaze_cb(lv_timer_t *timer)
{
    (void)timer;

    int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_us) / 1e6f;
    s_last_us = now;
    if (dt <= 0.0f || dt > 0.5f) {
        dt = GAZE_PERIOD_MS / 1000.0f;
    }

    float gx, gy, gxm, gym, gz;
    ring_imu_tilt(&gx, &gy);              /* screen-plane gravity, m/s^2 */
    ring_imu_gyro(&gxm, &gym, &gz);       /* gz = twist around the screen */

    /* Tilt -> gaze, with a deadband so a flat, still ring looks straight. */
    float tx = 0.0f, ty = 0.0f;
    if (fabsf(gx) > GAZE_DEADBAND) {
        tx = (gx - copysignf(GAZE_DEADBAND, gx)) * GAZE_GAIN;
    }
    if (fabsf(gy) > GAZE_DEADBAND) {
        ty = (gy - copysignf(GAZE_DEADBAND, gy)) * GAZE_GAIN;
    }
    tx = LV_CLAMP(-(float)GAZE_MAX_X, tx, (float)GAZE_MAX_X);
    ty = LV_CLAMP(-(float)GAZE_MAX_Y, ty, (float)GAZE_MAX_Y);

    /* Twist (gyro Z) throws the gaze sideways, then it decays home. */
    if (fabsf(gz) > TWIST_DEADBAND_DPS) {
        s_twist += gz * dt * TWIST_GAIN;
        s_twist = LV_CLAMP(-TWIST_MAX, s_twist, TWIST_MAX);
    } else {
        s_twist *= TWIST_DECAY;
        if (fabsf(s_twist) < 0.2f) {
            s_twist = 0.0f;
        }
    }

    float want_x = LV_CLAMP(-TWIST_MAX, tx + s_twist, TWIST_MAX);
    s_gaze_x += (want_x - s_gaze_x) * GAZE_SMOOTH;
    s_gaze_y += (ty - s_gaze_y) * GAZE_SMOOTH;

    apply_gaze((int32_t)lroundf(s_gaze_x), (int32_t)lroundf(s_gaze_y));
}

/* ---------------------------------------------------------- battery */

static void battery_cb(lv_timer_t *timer)
{
    (void)timer;

    ring_battery_data_t b;
    ring_battery_get(&b);

    if (b.percent < BATTERY_LOW_PCT && !b.charging) {
        if (!s_battery_warned) {
            s_battery_warned = true;
            ESP_LOGW(TAG, "battery %d%% (%0.2f V)", b.percent, b.voltage_v);
            if (!s_asleep && s_mood != 2) {
                RING_UI_LOCKED(-1) { apply_mood(2); }   /* sad */
            }
        }
    } else if (b.percent >= BATTERY_LOW_PCT + 5) {
        s_battery_warned = false;
    }
}

/* ------------------------------------------------------------ input */

static void sleep_enter(void)
{
    if (s_asleep) {
        return;
    }
    s_asleep = true;
    RING_UI_LOCKED(-1) {
        set_eye_srcs(&eye_joy_l, &eye_joy_r);
        lv_image_set_src(s_mouth, &mouth_smile);
        set_backlight_for_state();
    }
    ESP_LOGI(TAG, "sleep");
}

static void sleep_exit(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    RING_UI_LOCKED(-1) {
        apply_mood(s_mood);
        set_backlight_for_state();
    }
    ESP_LOGI(TAG, "wake");
}

static void input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_asleep) {
            sleep_exit();
        } else {
            sleep_enter();
        }
        return;
    }

    if (code != LV_EVENT_SHORT_CLICKED) {
        return;
    }

    if (s_asleep) {
        sleep_exit();
        return;
    }

    RING_UI_LOCKED(-1) { apply_mood(s_mood + 1); }
}

/* ------------------------------------------------------------ build */

static void fade_exec(void *var, int32_t v)
{
    (void)var;
    lv_obj_set_style_opa(s_eye[0], (lv_opa_t)v, 0);
    lv_obj_set_style_opa(s_eye[1], (lv_opa_t)v, 0);
    lv_obj_set_style_opa(s_mouth, (lv_opa_t)v, 0);
}

esp_err_t ring_example_start(void)
{
    ring_rand_seed(0xB6D1u);

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        /* SHORT_CLICKED (not CLICKED): a tap and a hold must not both fire. */
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_LONG_PRESSED, NULL);

        /* The face band: one object, painted once, never invalidated. */
        s_face = lv_obj_create(scr);
        lv_obj_remove_style_all(s_face);
        lv_obj_remove_flag(s_face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_face, FACE_W, FACE_H);
        lv_obj_set_pos(s_face, 0, FACE_Y);
        /* Flat fill, not a gradient: RGB565 gives a 222-px vertical gradient
         * only 3 quantisation steps, which bands visibly on a dark screen.
         * The depth comes from the sprites' own glow instead. */
        lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_face, lv_color_hex(0x091B36), 0);

        s_eye[0] = lv_image_create(s_face);
        s_eye[1] = lv_image_create(s_face);
        lv_obj_set_pos(s_eye[0], EYE_LX, EYE_Y);
        lv_obj_set_pos(s_eye[1], EYE_RX, EYE_Y);

        s_mouth = lv_image_create(s_face);
        lv_obj_set_pos(s_mouth, MOUTH_X, MOUTH_Y);

        apply_mood(0);

        /* Boot: fade the face in instead of snapping. One-shot, layer-buffered. */
        lv_obj_set_style_opa(s_eye[0], LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(s_eye[1], LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(s_mouth, LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &s_mood);
        lv_anim_set_exec_cb(&a, fade_exec);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, BOOT_FADE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);

        s_last_us = esp_timer_get_time();
        lv_timer_create(gaze_cb, GAZE_PERIOD_MS, NULL);
        lv_timer_create(blink_cb, BLINK_MIN_MS + ring_rand_below(BLINK_SPAN_MS), NULL);
        lv_timer_create(battery_cb, BATTERY_POLL_MS, NULL);
    }

    ESP_LOGI(TAG, "ready: %d moods, %d eye + %d mouth sprites in flash, "
                  "gaze +/-%d px, blink %d ms",
             N_MOODS, 6, 4, GAZE_MAX_X, BLINK_MS);
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "01_gbuddy_face";
}
