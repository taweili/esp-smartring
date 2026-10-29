/*
 * Smart Ring Plus (VIEWE) — Full-screen glyph-grid app launcher
 *
 * A 13x13 grid of glyph labels covering the 360x360 round panel. There is NO
 * scale/zoom transform: the open/close effect is a REVEAL RADIUS. A glyph is
 * "lit" (its target color) when its distance from the screen center is <= the
 * current radius, otherwise it is painted the background color (black), i.e.
 * invisible. Growing the radius from idle -> max lights the glyphs from the
 * center outward (appears to grow to the edge); shrinking it collapses the lit
 * region back to the tiny idle cluster at the center.
 *
 * While OPEN, accent glyphs cycle hue (orange -> pink -> blue -> cream) via an
 * HSV anim; a concentric-ring pattern of fixed green glyphs stays constant.
 *
 * State machine: IDLE -> OPENING -> OPEN -> CLOSING -> IDLE
 *
 * See notes/launcher-spec.md for the full design document.
 */
#include "launcher.h"

#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "lvgl.h"

#include <math.h>

#define TAG "launcher"

#define CX       180          /* panel center x (360px display)              */
#define CY       180          /* panel center y                              */
#define GRID_N   13           /* 13x13 = 169 glyphs                          */
#define GRID_STEP 27          /* px between grid cells (extent = 13*27=351)  */
#define IDLE_R   30           /* lit radius at idle -> tiny center cluster   */
#define MAX_R    240          /* reveal radius that reaches the corners      */
#define ICON_FONT (&lv_font_montserrat_20)
#define COLOR_GREEN lv_color_hex(0x00E676)  /* fixed ring color (green)      */
#define COLOR_BG    lv_color_black()        /* "hidden" glyphs = background  */
#define HUE_SAT   90          /* HSV saturation for accent glyphs            */
#define HUE_VAL   95          /* HSV value (brightness) for accent glyphs    */

/* Palette stops from the footage: orange, pink, blue, cream. */
static const uint16_t s_palette[4] = { 30, 320, 230, 45 };
#define N_PALETTE (sizeof(s_palette) / sizeof(s_palette[0]))

/* ------------------------------------------------------------------------- */
/* Symbols & classes                                                         */
/* ------------------------------------------------------------------------- */

typedef enum {
    ICON_STATIC_GREEN,   /* fixed green — the "app" ring    */
    ICON_ACCENT,         /* hue-cycling                     */
} icon_class_t;

typedef struct {
    const char *symbol;
    icon_class_t cls;
    int16_t x, y;        /* grid cell position (px)         */
    uint16_t dist;       /* distance from center (reveal key) */
    lv_obj_t *obj;       /* the lv_label                    */
} launcher_glyph_t;

/* Glyph symbols are cycled from this table (LV_SYMBOL_* are baked into the
 * built-in Montserrat fonts; no extra font component needed). */
static const char *const s_syms[] = {
    LV_SYMBOL_HOME,        LV_SYMBOL_SETTINGS,    LV_SYMBOL_AUDIO,
    LV_SYMBOL_WIFI,        LV_SYMBOL_IMAGE,       LV_SYMBOL_BLUETOOTH,
    LV_SYMBOL_PLAY,        LV_SYMBOL_NEXT,        LV_SYMBOL_PREV,
    LV_SYMBOL_PAUSE,       LV_SYMBOL_STOP,        LV_SYMBOL_BELL,
    LV_SYMBOL_GPS,         LV_SYMBOL_CHARGE,      LV_SYMBOL_BATTERY_FULL,
    LV_SYMBOL_WARNING,     LV_SYMBOL_CALL,        LV_SYMBOL_DIRECTORY,
    LV_SYMBOL_VOLUME_MAX,  LV_SYMBOL_DOWNLOAD,    LV_SYMBOL_UPLOAD,
    LV_SYMBOL_REFRESH,     LV_SYMBOL_MUTE,        LV_SYMBOL_VOLUME_MID,
    LV_SYMBOL_EYE_OPEN,    LV_SYMBOL_EYE_CLOSE,   LV_SYMBOL_SHUFFLE,
    LV_SYMBOL_LOOP,        LV_SYMBOL_EDIT,        LV_SYMBOL_TRASH,
    LV_SYMBOL_SAVE,        LV_SYMBOL_COPY,        LV_SYMBOL_PASTE,
    LV_SYMBOL_KEYBOARD,    LV_SYMBOL_USB,         LV_SYMBOL_BARS,
    LV_SYMBOL_FILE,        LV_SYMBOL_SD_CARD,     LV_SYMBOL_ENVELOPE,
    LV_SYMBOL_VIDEO,       LV_SYMBOL_POWER,       LV_SYMBOL_OK,
};
#define N_SYMS (sizeof(s_syms) / sizeof(s_syms[0]))

#define N_GLYPHS (GRID_N * GRID_N)

static launcher_glyph_t s_glyphs[N_GLYPHS];

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    L_IDLE, L_OPENING, L_OPEN, L_CLOSING,
} launcher_state_t;

static launcher_state_t s_state = L_IDLE;
static int32_t    s_hue = 0;          /* animated hue (var for lv_anim)     */
static int32_t    s_radius = IDLE_R;  /* current reveal radius (px)         */
static int        s_palette_idx = 0;  /* current palette stop               */

/* ------------------------------------------------------------------------- */
/* Repaint                                                                    */
/* ------------------------------------------------------------------------- */

static lv_color_t target_color(const launcher_glyph_t *g)
{
    if (g->cls == ICON_STATIC_GREEN)
        return COLOR_GREEN;
    return lv_color_hsv_to_rgb((uint16_t)s_hue, HUE_SAT, HUE_VAL);
}

/* Paint every glyph: lit if within the reveal radius, else background. */
static void repaint(int r)
{
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        lv_color_t c = (s_glyphs[i].dist <= (uint16_t)r)
                           ? target_color(&s_glyphs[i])
                           : COLOR_BG;
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------------- */
/* Animations                                                                 */
/* ------------------------------------------------------------------------- */

static void reveal_cb(void *var, int32_t v)
{
    (void)var;
    s_radius = v;
    repaint((int)v);
}

static void color_cb(void *var, int32_t v)
{
    (void)var;
    s_hue = v;
    repaint((int)s_radius);
}

static void reveal(int32_t from, int32_t to, uint32_t dur,
                   lv_anim_path_cb_t path, lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_radius);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_exec_cb(&a, reveal_cb);
    lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
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
    s_hue = start;
    repaint((int)s_radius);
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
    repaint(IDLE_R);          /* ensure the idle cluster is painted */
    ESP_LOGI(TAG, "closed -> idle");
}

static void launcher_open(void)
{
    if (s_state != L_IDLE) return;
    s_state = L_OPENING;
    ESP_LOGI(TAG, "opening...");
    reveal(IDLE_R, MAX_R, 450, lv_anim_path_ease_out, open_done);
}

static void launcher_close(void)
{
    if (s_state != L_OPEN) return;
    s_state = L_CLOSING;
    ESP_LOGI(TAG, "closing...");
    reveal(MAX_R, IDLE_R, 350, lv_anim_path_ease_in, close_done);
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

static void build_grid(void)
{
    lv_obj_t *scr = lv_screen_active();
    int half = (GRID_N - 1) / 2;

    /* Pass 1: create the labels at their cell centers (top-left == center for
     * now) and record their reveal distance. */
    for (int i = 0; i < GRID_N; i++) {
        for (int j = 0; j < GRID_N; j++) {
            int idx = i * GRID_N + j;
            int x = CX + (i - half) * GRID_STEP;
            int y = CY + (j - half) * GRID_STEP;

            s_glyphs[idx].x = (int16_t)x;
            s_glyphs[idx].y = (int16_t)y;
            s_glyphs[idx].dist = (uint16_t)roundf(hypotf(x - CX, y - CY));
            s_glyphs[idx].symbol = s_syms[idx % N_SYMS];

            /* Concentric distance bands: odd bands are accent, even are the
             * fixed green. The center (dist 0, band 0) is green. */
            int band = s_glyphs[idx].dist / GRID_STEP;
            s_glyphs[idx].cls = (band & 1) ? ICON_ACCENT : ICON_STATIC_GREEN;

            lv_obj_t *lab = lv_label_create(scr);
            lv_label_set_text(lab, s_glyphs[idx].symbol);
            lv_obj_set_style_text_font(lab, ICON_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(lab, COLOR_BG, LV_PART_MAIN);
            lv_obj_set_pos(lab, x, y);
            s_glyphs[idx].obj = lab;
        }
    }

    /* Pass 2: force LVGL to compute label sizes, then centre each glyph on its
     * cell point. Without this lv_obj_get_width()/height() return 0 and every
     * glyph's top-left corner sits on the cell point (off-centre). */
    lv_obj_update_layout(scr);
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        lv_obj_t *lab = s_glyphs[i].obj;
        /* (size-1)/2 so the glyph's true centre lands exactly on the point. */
        lv_obj_set_pos(lab,
                       s_glyphs[i].x - (lv_obj_get_width(lab) - 1) / 2,
                       s_glyphs[i].y - (lv_obj_get_height(lab) - 1) / 2);
    }
}

esp_err_t launcher_start(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    build_grid();
    repaint(IDLE_R);      /* start collapsed to the tiny idle cluster */

    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_GESTURE, NULL);

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "ready (idle): %d glyphs, %d cells", (int)N_GLYPHS, GRID_N);
    return ESP_OK;
}
