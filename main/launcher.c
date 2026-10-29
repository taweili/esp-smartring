/*
 * Smart Ring Plus (VIEWE) — Circular app-grid launcher
 *
 * A radial icon grid (4 concentric rings, 37 apps) rendered on the 360x360
 * round panel. Tap to open: the whole grid zooms out from the screen center
 * (container transform scale 0.12 -> 1.0); accent-colored icons then cycle
 * hue (orange -> pink -> blue -> cream) via an HSV anim while a fixed ring of
 * green icons stays constant. Tap again (or swipe to advance the palette)
 * collapses it back to the tiny idle cluster.
 *
 * State machine: IDLE -> OPENING -> OPEN -> CLOSING -> IDLE
 *
 * See notes/launcher-spec.md for the full design document.
 */
#include "launcher.h"

#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <math.h>

#define TAG "launcher"

#define CX       180          /* panel center x (360px display)            */
#define CY       180          /* panel center y                             */
#define SCALE_N  256          /* LVGL transform scale units: 256 == 1.0     */
#define SCALE_IDLE 0.12f      /* collapsed scale (the idle "dot" cluster)   */
#define ICON_FONT (&lv_font_montserrat_20)
#define COLOR_GREEN lv_color_hex(0x00E676)  /* fixed ring color (green)     */
#define HUE_SAT   90          /* HSV saturation for accent icons            */
#define HUE_VAL   95          /* HSV value (brightness) for accent icons    */

/* Set to 1 to log per-render timing (render loop cost while tuning). */
#define LAUNCHER_DEBUG_RENDER 0

/* Palette stops from the footage: orange, pink, blue, cream. */
static const uint16_t s_palette[4] = { 30, 320, 230, 45 };
#define N_PALETTE (sizeof(s_palette) / sizeof(s_palette[0]))

/* ------------------------------------------------------------------------- */
/* Icon table                                                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    ICON_STATIC_GREEN,   /* fixed green — the "app" ring    */
    ICON_ACCENT,         /* hue-cycling                     */
} icon_class_t;

typedef struct {
    const char *symbol;
    icon_class_t cls;
} launcher_icon_t;

/* Ring 0: 1 icon, ring 1: 6, ring 2: 12, ring 3: 18  =>  37 apps. */
static const launcher_icon_t s_icons[] = {
    /* ring 0 (r = 0) */
    { LV_SYMBOL_HOME,        ICON_STATIC_GREEN },
    /* ring 1 (r = 48) */
    { LV_SYMBOL_SETTINGS,    ICON_STATIC_GREEN },
    { LV_SYMBOL_AUDIO,       ICON_ACCENT },
    { LV_SYMBOL_WIFI,        ICON_STATIC_GREEN },
    { LV_SYMBOL_IMAGE,       ICON_ACCENT },
    { LV_SYMBOL_BLUETOOTH,   ICON_STATIC_GREEN },
    { LV_SYMBOL_PLAY,        ICON_ACCENT },
    /* ring 2 (r = 96) */
    { LV_SYMBOL_NEXT,        ICON_STATIC_GREEN },
    { LV_SYMBOL_PREV,        ICON_ACCENT },
    { LV_SYMBOL_PAUSE,       ICON_STATIC_GREEN },
    { LV_SYMBOL_STOP,        ICON_ACCENT },
    { LV_SYMBOL_BELL,        ICON_STATIC_GREEN },
    { LV_SYMBOL_GPS,         ICON_ACCENT },
    { LV_SYMBOL_CHARGE,      ICON_STATIC_GREEN },
    { LV_SYMBOL_BATTERY_FULL,ICON_ACCENT },
    { LV_SYMBOL_WARNING,     ICON_STATIC_GREEN },
    { LV_SYMBOL_CALL,        ICON_ACCENT },
    { LV_SYMBOL_DIRECTORY,   ICON_STATIC_GREEN },
    { LV_SYMBOL_VOLUME_MAX,  ICON_ACCENT },
    /* ring 3 (r = 140) */
    { LV_SYMBOL_DOWNLOAD,    ICON_STATIC_GREEN },
    { LV_SYMBOL_UPLOAD,      ICON_ACCENT },
    { LV_SYMBOL_REFRESH,     ICON_STATIC_GREEN },
    { LV_SYMBOL_MUTE,        ICON_ACCENT },
    { LV_SYMBOL_VOLUME_MID,  ICON_STATIC_GREEN },
    { LV_SYMBOL_EYE_OPEN,    ICON_ACCENT },
    { LV_SYMBOL_EYE_CLOSE,   ICON_STATIC_GREEN },
    { LV_SYMBOL_SHUFFLE,     ICON_ACCENT },
    { LV_SYMBOL_LOOP,        ICON_STATIC_GREEN },
    { LV_SYMBOL_EDIT,        ICON_ACCENT },
    { LV_SYMBOL_TRASH,       ICON_STATIC_GREEN },
    { LV_SYMBOL_SAVE,        ICON_ACCENT },
    { LV_SYMBOL_COPY,        ICON_STATIC_GREEN },
    { LV_SYMBOL_PASTE,       ICON_ACCENT },
    { LV_SYMBOL_KEYBOARD,    ICON_STATIC_GREEN },
    { LV_SYMBOL_USB,         ICON_ACCENT },
    { LV_SYMBOL_BARS,        ICON_STATIC_GREEN },
    { LV_SYMBOL_FILE,        ICON_ACCENT },
};
#define N_ICONS (sizeof(s_icons) / sizeof(s_icons[0]))

/* Ring geometry: radius and icon count per concentric ring. */
static const int s_ring_r[] = { 0, 48, 96, 140 };
static const int s_ring_n[] = { 1, 6, 12, 18 };
#define N_RINGS (sizeof(s_ring_r) / sizeof(s_ring_r[0]))

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    L_IDLE, L_OPENING, L_OPEN, L_CLOSING,
} launcher_state_t;

static lv_obj_t *s_grid;              /* the whole launcher (scaled)        */
static lv_obj_t *s_accent[N_ICONS];   /* ICON_ACCENT labels for color anim  */
static int        s_n_accent = 0;
static launcher_state_t s_state = L_IDLE;
static int32_t    s_hue = 0;          /* animated hue (var for lv_anim)     */
static int        s_palette_idx = 0;  /* current palette stop               */

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void apply_scale(float s)
{
    int32_t v = (int32_t)(s * SCALE_N);
    lv_obj_set_style_transform_scale_x(s_grid, v, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(s_grid, v, LV_PART_MAIN);
}

static void paint_accent(uint16_t hue)
{
    lv_color_t c = lv_color_hsv_to_rgb(hue, HUE_SAT, HUE_VAL);
    for (int i = 0; i < s_n_accent; i++) {
        lv_obj_set_style_text_color(s_accent[i], c, LV_PART_MAIN);
    }
}

#if LAUNCHER_DEBUG_RENDER
/* --- Per-render timing probe (debug only) -------------------------------- */
static uint32_t s_render_n;
static int64_t  s_render_t0;

static void render_ready_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    s_render_n++;
    if (s_render_n <= 30 || (s_render_n % 100) == 0) {
        ESP_LOGI(TAG, "render #%u dt=%lldms", (unsigned)s_render_n,
                 (long long)((now - s_render_t0) / 1000));
    }
    s_render_t0 = now;
}
#endif

/* ------------------------------------------------------------------------- */
/* Zoom animation                                                            */
/* ------------------------------------------------------------------------- */

static void scale_cb(void *var, int32_t v)
{
    (void)var;
    lv_obj_set_style_transform_scale_x(s_grid, v, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(s_grid, v, LV_PART_MAIN);
}

static void zoom(int32_t from, int32_t to, uint32_t dur,
                 lv_anim_path_cb_t path, lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_grid);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_exec_cb(&a, scale_cb);
    lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------------- */
/* Color-cycle animation                                                     */
/* ------------------------------------------------------------------------- */

static void color_cb(void *var, int32_t v)
{
    (void)var;
    paint_accent((uint16_t)v);
}

static void start_color_anim(void)
{
    uint16_t start = s_palette[s_palette_idx];
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_hue);
    lv_anim_set_values(&a, start, start + 360);       /* one full hue sweep  */
    lv_anim_set_duration(&a, 6000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, color_cb);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    paint_accent(start);
}

static void stop_color_anim(void)
{
    lv_anim_del(&s_hue, NULL);
}

/* ------------------------------------------------------------------------- */
/* State transitions                                                         */
/* ------------------------------------------------------------------------- */

static void open_done(lv_anim_t *a)
{
    (void)a;
    s_state = L_OPEN;
    start_color_anim();
    ESP_LOGI(TAG, "opened");
}

static void close_done(lv_anim_t *a)
{
    (void)a;
    s_state = L_IDLE;
    stop_color_anim();
    apply_scale(SCALE_IDLE);
    ESP_LOGI(TAG, "closed -> idle");
}

static void launcher_open(void)
{
    if (s_state != L_IDLE) return;
    s_state = L_OPENING;
    ESP_LOGI(TAG, "opening...");
    zoom((int32_t)(SCALE_IDLE * SCALE_N), SCALE_N, 450,
         lv_anim_path_ease_out, open_done);
}

static void launcher_close(void)
{
    if (s_state != L_OPEN) return;
    s_state = L_CLOSING;
    ESP_LOGI(TAG, "closing...");
    zoom(SCALE_N, (int32_t)(SCALE_IDLE * SCALE_N), 350,
         lv_anim_path_ease_in, close_done);
}

static void palette_next(void)
{
    s_palette_idx = (s_palette_idx + 1) % N_PALETTE;
    if (s_state == L_OPEN) {
        stop_color_anim();
        start_color_anim();
        ESP_LOGI(TAG, "palette -> %u", s_palette[s_palette_idx]);
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static void input_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        if (s_state == L_IDLE)       launcher_open();
        else if (s_state == L_OPEN)  launcher_close();
    } else if (code == LV_EVENT_GESTURE) {
        if (s_state == L_OPEN)       palette_next();
    }
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

static lv_obj_t *s_icon_obj[N_ICONS];   /* all icon labels                      */
static int16_t   s_icon_x[N_ICONS];     /* ring point for each icon            */
static int16_t   s_icon_y[N_ICONS];

static void build_rings(void)
{
    const float TWO_PI = 6.283185307f;
    int idx = 0;

    /* Pass 1: create the labels. Their size is NOT known yet, so just record
     * the ring point each one must be centred on. */
    for (int ring = 0; ring < (int)N_RINGS; ring++) {
        int r = s_ring_r[ring];
        int n = s_ring_n[ring];
        for (int i = 0; i < n; i++) {
            /* Start at 12 o'clock, distribute evenly around the ring. */
            float ang = TWO_PI * (float)i / (float)n - 1.5707963f;
            s_icon_x[idx] = (int16_t)(CX + (int)roundf(r * cosf(ang)));
            s_icon_y[idx] = (int16_t)(CY + (int)roundf(r * sinf(ang)));

            lv_obj_t *lab = lv_label_create(s_grid);
            lv_label_set_text(lab, s_icons[idx].symbol);
            lv_obj_set_style_text_font(lab, ICON_FONT, LV_PART_MAIN);

            if (s_icons[idx].cls == ICON_ACCENT) {
                lv_obj_set_style_text_color(lab, lv_color_hex(0xFF7043),
                                            LV_PART_MAIN);
                s_accent[s_n_accent++] = lab;
            } else {
                lv_obj_set_style_text_color(lab, COLOR_GREEN, LV_PART_MAIN);
            }

            s_icon_obj[idx] = lab;
            idx++;
        }
    }

    /* Pass 2: force LVGL to compute the label sizes before reading them.
     * Without this, lv_obj_get_width()/height() return 0 and every glyph is
     * placed with its top-left corner on the ring point (off-centre). */
    lv_obj_update_layout(s_grid);

    for (int i = 0; i < (int)N_ICONS; i++) {
        lv_obj_t *lab = s_icon_obj[i];
        /* Use (size-1)/2 so the glyph's true centre lands exactly on the ring
         * point (LVGL centre of an area is x1 + (w-1)/2). */
        lv_obj_set_pos(lab, s_icon_x[i] - (lv_obj_get_width(lab) - 1) / 2,
                            s_icon_y[i] - (lv_obj_get_height(lab) - 1) / 2);
    }
}

esp_err_t launcher_start(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Transparent full-screen container that we scale to zoom. */
    s_grid = lv_obj_create(scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_grid, 360, 360);
    lv_obj_center(s_grid);
    lv_obj_set_style_transform_pivot_x(s_grid, 50, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_grid, 50, LV_PART_MAIN);

    build_rings();
    apply_scale(SCALE_IDLE);

    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_GESTURE, NULL);

#if LAUNCHER_DEBUG_RENDER
    s_render_t0 = esp_timer_get_time();
    lv_display_add_event_cb(lv_display_get_default(), render_ready_cb,
                            LV_EVENT_RENDER_READY, NULL);
#endif

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "ready (idle): %d icons, %d accent",
             (int)N_ICONS, s_n_accent);
    return ESP_OK;
}
