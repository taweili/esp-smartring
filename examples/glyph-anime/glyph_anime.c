/*
 * glyph-anime — full-screen glyph grid with a layered reveal.
 *
 * A 13x13 grid of glyph labels covering the 360x360 round panel. There is NO
 * scale/zoom transform: the open/close effect is a LAYERED REVEAL. The grid is
 * split into concentric layers by distance from the screen center; progress
 * advances one layer at a time, so each layer of glyphs fades in as a complete
 * ring and the lit set grows outward -- layers and layers of glyphs added
 * around the center until the panel is full. Closing removes them outside-in.
 * Glyphs not yet reached are painted the background color, i.e. invisible.
 *
 * While OPEN, accent glyphs cycle hue (orange -> pink -> blue -> cream); fixed
 * green glyphs are scattered pseudo-randomly and stay constant. Only ~6 glyph
 * shapes are used, matching the source footage.
 *
 * State machine: IDLE -> OPENING -> OPEN -> CLOSING -> IDLE
 *
 * See notes/launcher-spec.md for the full design document.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <math.h>

#define TAG "glyph-anime"

#define GRID_N    13   /* 13x13 = 169 glyphs                        */
#define GRID_STEP 27   /* px between cells (extent = 13*27 = 351 px) */

#define LAYER_UNIT 256 /* progress units per layer (fixed point)     */
#define LAYER_FADE 128 /* fade window per layer (half a slot)        */
#define MAX_LAYERS 10  /* safety cap on the layer index              */

#define ICON_FONT (&lv_font_montserrat_20)

/* Palette stops from the footage: orange, pink, blue, cream. */
static const uint16_t s_palette[4] = { 30, 320, 230, 45 };
#define N_PALETTE (sizeof(s_palette) / sizeof(s_palette[0]))

/* ------------------------------------------------------------------------- */
/* Glyph model                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    ICON_STATIC_GREEN,   /* fixed green — the "app" pattern */
    ICON_ACCENT,         /* hue-cycling                     */
} glyph_class_t;

typedef struct {
    const char *symbol;
    glyph_class_t cls;
    int16_t x, y;      /* cell position (px)                        */
    uint16_t dist;     /* distance from center (px)                 */
    uint8_t layer;     /* concentric layer = round(dist / GRID_STEP) */
    lv_obj_t *obj;
} glyph_t;

/* The footage uses only ~6 distinct pixel glyph shapes; approximate them with
 * a small LV_SYMBOL_* set (baked into the built-in Montserrat fonts). Shapes
 * are picked pseudo-randomly per cell rather than cycled, so the field reads
 * as organic. */
static const char *const s_syms[6] = {
    LV_SYMBOL_PLUS,    /* +        */
    LV_SYMBOL_CLOSE,   /* x        */
    LV_SYMBOL_STOP,    /* square   */
    LV_SYMBOL_BULLET,  /* dot      */
    LV_SYMBOL_PLAY,    /* triangle */
    LV_SYMBOL_PAUSE,   /* bars     */
};
#define N_SYMS (sizeof(s_syms) / sizeof(s_syms[0]))

#define N_GLYPHS (GRID_N * GRID_N)

static glyph_t s_glyphs[N_GLYPHS];

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    L_IDLE, L_OPENING, L_OPEN, L_CLOSING,
} state_t;

static state_t s_state = L_IDLE;
static int32_t s_hue = 0;         /* animated hue (var for lv_anim)        */
static int32_t s_progress = 0;    /* layered reveal progress               */
static int     s_n_layers = 1;    /* highest layer index + 1               */
static int     s_palette_idx = 0; /* current palette stop                  */

/* Idle shows the innermost two layers only (the tiny center cluster). A glyph
 * in layer L is fully lit once progress >= L*LAYER_UNIT + LAYER_FADE. */
#define IDLE_PROGRESS (1 * LAYER_UNIT + LAYER_FADE)

/* ------------------------------------------------------------------------- */
/* Repaint                                                                   */
/* ------------------------------------------------------------------------- */

static lv_color_t target_color(const glyph_t *g)
{
    if (g->cls == ICON_STATIC_GREEN) {
        return RING_COLOR_GREEN;
    }
    return lv_color_hsv_to_rgb((uint16_t)s_hue, RING_HUE_SAT, RING_HUE_VAL);
}

/* Paint every glyph from the layered reveal progress.
 *
 * The reveal is NOT one continuous radius: it advances one concentric layer at
 * a time. A layer starts fading in when progress reaches its slot and is fully
 * lit LAYER_FADE units later, then stays lit. So the lit set grows layer by
 * layer outward, and shrinking progress removes them outside-in. Because
 * LAYER_FADE < LAYER_UNIT there is a short gap between layers, which is what
 * makes each ring read as a distinct step rather than a smooth bloom. */
static void repaint(int32_t progress)
{
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        int32_t local = progress - (int32_t)s_glyphs[i].layer * LAYER_UNIT;
        lv_color_t c;

        if (local <= 0) {
            c = RING_COLOR_BG;                    /* layer not reached yet */
        } else if (local >= LAYER_FADE) {
            c = target_color(&s_glyphs[i]);       /* layer fully lit       */
        } else {
            /* Fading this layer in: blend from background to target. */
            c = lv_color_mix(target_color(&s_glyphs[i]), RING_COLOR_BG,
                             (uint8_t)(local * 255 / LAYER_FADE));
        }
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------------- */
/* Animations                                                                */
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
    lv_anim_set_values(&a, start, start + 360);   /* one full hue sweep */
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
    if (s_state != L_IDLE) {
        return;
    }
    s_state = L_OPENING;
    ESP_LOGI(TAG, "opening: %d layers", s_n_layers);
    reveal(IDLE_PROGRESS, s_n_layers * LAYER_UNIT, RING_MS_REVEAL_OPEN,
           lv_anim_path_ease_out, open_done);
}

static void launcher_close(void)
{
    if (s_state != L_OPEN) {
        return;
    }
    s_state = L_CLOSING;
    ESP_LOGI(TAG, "closing...");
    reveal(s_n_layers * LAYER_UNIT, IDLE_PROGRESS, RING_MS_REVEAL_CLOSE,
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
        if (s_state == L_IDLE) {
            launcher_open();
        } else if (s_state == L_OPEN) {
            launcher_close();
        }
    } else if (code == LV_EVENT_GESTURE) {
        if (s_state == L_OPEN) {
            palette_next();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

static void build_grid(lv_obj_t *scr)
{
    const int half = (GRID_N - 1) / 2;

    ring_rand_seed(0x9E3779B9u);

    for (int i = 0; i < GRID_N; i++) {
        for (int j = 0; j < GRID_N; j++) {
            int idx = i * GRID_N + j;
            int x = RING_CX + (i - half) * GRID_STEP;
            int y = RING_CY + (j - half) * GRID_STEP;

            s_glyphs[idx].x      = (int16_t)x;
            s_glyphs[idx].y      = (int16_t)y;
            s_glyphs[idx].dist   = (uint16_t)roundf(hypotf(x - RING_CX, y - RING_CY));
            s_glyphs[idx].symbol = s_syms[ring_rand_below(N_SYMS)];

            /* Random green/accent split (no concentric bands). */
            s_glyphs[idx].cls = ring_rand_bool() ? ICON_ACCENT : ICON_STATIC_GREEN;

            /* Concentric layer = grid distance from center (reveal key). */
            int layer = (int)roundf((float)s_glyphs[idx].dist / GRID_STEP);
            if (layer >= MAX_LAYERS) {
                layer = MAX_LAYERS - 1;
            }
            s_glyphs[idx].layer = (uint8_t)layer;
            if (layer + 1 > s_n_layers) {
                s_n_layers = layer + 1;
            }

            s_glyphs[idx].obj = ring_ui_add_label(scr, s_glyphs[idx].symbol,
                                                  ICON_FONT, RING_COLOR_BG);
        }
    }

    /* Centre every glyph on its cell point. */
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        ring_ui_place_centered(s_glyphs[i].obj, s_glyphs[i].x, s_glyphs[i].y);
    }
}

/* ------------------------------------------------------------------------- */
/* Example entry points                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        build_grid(scr);

        s_progress = IDLE_PROGRESS;
        repaint(s_progress);   /* start collapsed to the tiny idle cluster */

        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_GESTURE, NULL);
    }

    ESP_LOGI(TAG, "ready (idle): %d glyphs, %d cells, %d layers",
             (int)N_GLYPHS, GRID_N, s_n_layers);
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "glyph-anime";
}
