/*
 * Smart Ring Plus (VIEWE) — Full-screen glyph-grid app launcher
 *
 * A 13x13 grid of glyph labels covering the 360x360 round panel. There is NO
 * scale/zoom transform: the open/close effect is a LAYERED REVEAL. The grid is
 * split into concentric layers by distance from the screen center; progress
 * advances one layer at a time, so each layer of glyphs fades in as a complete
 * ring and the lit set grows outward -- layers and layers of glyphs added
 * around the center until the panel is full. Closing removes them outside-in.
 * Glyphs not yet reached are painted the background color (black), i.e.
 * invisible.
 *
 * While OPEN, accent glyphs cycle hue (orange -> pink -> blue -> cream) via an
 * HSV anim; randomly scattered fixed-green glyphs stay constant. Only a
 * handful of distinct glyph shapes are used (~6), matching the footage.
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
#define LAYER_UNIT 256        /* progress units per layer (fixed point)      */
#define LAYER_FADE 128        /* per-layer fade window, in LAYER_UNIT/256    */
#define MAX_LAYERS 10         /* safety cap for the layer index              */
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
    int16_t x, y;        /* grid cell position (px)                     */
    uint16_t dist;       /* distance from center (px)                   */
    uint8_t layer;       /* concentric layer index = round(dist/STEP)   */
    lv_obj_t *obj;       /* the lv_label                                */
} launcher_glyph_t;

/* The footage uses only ~6 distinct pixel glyph shapes; approximate them with
 * a small LV_SYMBOL_* set (baked into the built-in Montserrat fonts, so no
 * extra font component is needed). Glyphs are picked pseudo-randomly per cell
 * rather than cycled, so the field reads as organic like the footage. */
static const char *const s_syms[6] = {
    LV_SYMBOL_PLUS,    /* +        */
    LV_SYMBOL_CLOSE,   /* x        */
    LV_SYMBOL_STOP,    /* square   */
    LV_SYMBOL_BULLET,  /* dot      */
    LV_SYMBOL_PLAY,    /* triangle */
    LV_SYMBOL_PAUSE,   /* bars     */
};
#define N_SYMS (sizeof(s_syms) / sizeof(s_syms[0]))

/* Tiny deterministic xorshift32 RNG: the random glyph/color field stays
 * reproducible across boots (useful when tuning the look).
 *
 * NOTE: do NOT use a plain LCG here — its low bits are degenerate, so
 * `rnd() % 6` and `rnd() & 1` collapse to even values only (the whole field
 * came out all-accent with 3 glyph shapes). xorshift32 mixes all bits. */
static uint32_t s_rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

#define N_GLYPHS (GRID_N * GRID_N)

static launcher_glyph_t s_glyphs[N_GLYPHS];

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    L_IDLE, L_OPENING, L_OPEN, L_CLOSING,
} launcher_state_t;

static launcher_state_t s_state = L_IDLE;
static int32_t    s_hue = 0;            /* animated hue (var for lv_anim)   */
static int32_t    s_progress = 0;       /* layered reveal progress          */
static int        s_n_layers = 1;       /* highest layer index + 1          */
static int        s_palette_idx = 0;    /* current palette stop             */

/* Idle shows the innermost two layers only (the tiny center cluster).
 * A glyph in layer L is fully lit once progress >= L*LAYER_UNIT + LAYER_FADE. */
#define IDLE_PROGRESS (1 * LAYER_UNIT + LAYER_FADE)

/* ------------------------------------------------------------------------- */
/* Repaint                                                                    */
/* ------------------------------------------------------------------------- */

static lv_color_t target_color(const launcher_glyph_t *g)
{
    if (g->cls == ICON_STATIC_GREEN)
        return COLOR_GREEN;
    return lv_color_hsv_to_rgb((uint16_t)s_hue, HUE_SAT, HUE_VAL);
}

/* Paint every glyph from the layered reveal progress.
 *
 * The reveal is NOT one continuous radius: it advances one concentric layer
 * at a time. Each glyph belongs to a layer (round(dist / GRID_STEP)); a layer
 * starts fading in when progress reaches its slot and is fully lit LAYER_FADE
 * units later, then stays lit. So the lit set grows layer by layer outward --
 * layers and layers of glyphs added around the center -- and shrinking
 * progress removes them outside-in. LAYER_FADE < LAYER_UNIT leaves a short
 * gap between layers so each ring reads as a distinct step. */
static void repaint(int32_t progress)
{
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        int32_t local = progress - (int32_t)s_glyphs[i].layer * LAYER_UNIT;
        lv_color_t c;

        if (local <= 0) {
            c = COLOR_BG;                       /* layer not reached yet    */
        } else if (local >= LAYER_FADE) {
            c = target_color(&s_glyphs[i]);     /* layer fully lit          */
        } else {
            /* Fading this layer in: blend from background to target. */
            c = lv_color_mix(target_color(&s_glyphs[i]), COLOR_BG,
                             (uint8_t)(local * 255 / LAYER_FADE));
        }
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------------- */
/* Animations                                                                 */
/* ------------------------------------------------------------------------- */

static void reveal_cb(void *var, int32_t v)
{
    (void)var;
    s_progress = v;
    repaint(v);
}

static void color_cb(void *var, int32_t v)
{
    (void)var;
    s_hue = v;
    repaint(s_progress);
}

static void reveal(int32_t from, int32_t to, uint32_t dur,
                   lv_anim_path_cb_t path, lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_progress);
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
    repaint(s_progress);
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
    repaint(IDLE_PROGRESS);   /* ensure the idle cluster is painted */
    ESP_LOGI(TAG, "closed -> idle");
}

static void launcher_open(void)
{
    if (s_state != L_IDLE) return;
    s_state = L_OPENING;
    ESP_LOGI(TAG, "opening: %d layers", s_n_layers);
    reveal(IDLE_PROGRESS, s_n_layers * LAYER_UNIT, 450,
           lv_anim_path_ease_out, open_done);
}

static void launcher_close(void)
{
    if (s_state != L_OPEN) return;
    s_state = L_CLOSING;
    ESP_LOGI(TAG, "closing...");
    reveal(s_n_layers * LAYER_UNIT, IDLE_PROGRESS, 350,
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
            s_glyphs[idx].symbol = s_syms[rnd() % N_SYMS];

            /* Concentric layer = grid distance from the center. This is the
             * reveal key: layers light up one after another outward. */
            int layer = (int)roundf((float)s_glyphs[idx].dist / GRID_STEP);
            if (layer >= MAX_LAYERS) layer = MAX_LAYERS - 1;
            s_glyphs[idx].layer = (uint8_t)layer;
            if (layer + 1 > s_n_layers) s_n_layers = layer + 1;

            /* Random green/accent split (no concentric bands): half the cells
             * are the fixed green pattern, the rest cycle hue. */
            s_glyphs[idx].cls = (rnd() & 1) ? ICON_ACCENT : ICON_STATIC_GREEN;

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
    s_progress = IDLE_PROGRESS;
    repaint(s_progress);      /* start collapsed to the tiny idle cluster */

    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, input_cb, LV_EVENT_GESTURE, NULL);

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "ready (idle): %d glyphs, %d cells, %d layers",
             (int)N_GLYPHS, GRID_N, s_n_layers);
    return ESP_OK;
}
