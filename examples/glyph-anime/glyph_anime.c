/*
 * glyph-anime — ring-rotation-driven glyph reveal.
 *
 * A fixed 13x13 axis-aligned square grid of glyph labels on the 360x360 round
 * panel. Cells NEVER move, scale or rotate. The grid is built once and never
 * re-laid-out.
 *
 * Interaction (see examples/glyph-anime/README.md):
 *   - Turning the ring CLOCKWISE reveals MORE cells, center-out, until the
 *     panel is full. Turning it COUNTER-CLOCKWISE un-reveals them outside-in.
 *   - The reveal is a single radius, driven by the on-board IMU: gyro Z (the
 *     turn axis through the finger) is integrated into a turn angle and mapped
 *     to a reveal radius in px. No touch is needed; the grid is a position
 *     control (it holds where you leave it).
 *   - A tap resets the reveal to the small idle core.
 *
 * Palette: green + warm yellow, each cell at one of N_BRIGHTNESS brightness
 * tiers (pseudo-random per cell), giving the soft, uneven luminance seen in
 * the stills. The reveal edge is blended across one cell step rather than a
 * hard threshold.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>

#define TAG "glyph-anime"

#define GRID_N    13   /* 13x13 = 169 glyphs                        */
#define GRID_STEP 27   /* px between cells (extent = 13*27 = 351 px) */

#define ICON_FONT (&lv_font_montserrat_20)

/* ------------------------------------------------------------------------- */
/* Turn -> reveal mapping                                                    */
/* ------------------------------------------------------------------------- */

/* Degrees of ring turn per 100 px of reveal (feel knob). */
#define DEG_PER_REVEAL 90.0f

/* Idle core radius: the small lit cluster shown when the reveal is at minimum.
 * ~1.5 cells lights the innermost few glyphs. */
#define REVEAL_IDLE (1.5f * GRID_STEP)

/* Full-panel radius. 180 px is the panel radius, which lights every cell that
 * is actually on the round panel. */
#define REVEAL_MAX ((float)RING_RADIUS)

/* Gyro deadband (deg/s): ignore rotation below this so offset drift does not
 * creep the reveal while the ring is still. */
#define GYRO_DEADBAND_DPS 1.5f

/* Turn direction sign. +1.0 assumes a clockwise turn gives +gz. If the ring
 * grows on counter-clockwise instead, flip this to -1.0. Verify on device:
 * rotate ~90 deg clockwise and confirm the reveal expands. */
#define TURN_SIGN 1.0f

/* Soft reveal edge: cells within this distance of the reveal radius blend from
 * black to full color instead of snapping. One cell step reads as a soft edge. */
#define EDGE_FADE ((float)GRID_STEP)

/* IMU poll cadence (ms). 50 Hz is plenty for a position-controlled reveal. */
#define POLL_MS 20

/* ------------------------------------------------------------------------- */
/* Palette                                                                   */
/* ------------------------------------------------------------------------- */

/* Warm amber/yellow accent. Green uses RING_COLOR_GREEN (0x00E676). */
#define RING_COLOR_YELLOW lv_color_hex(0xFFB74D)

/* Brightness tiers per cell: dim / mid / full. Each cell picks one at build
 * time (pseudo-random), so the lit field has uneven luminance like the stills. */
#define N_BRIGHTNESS 3
static const float s_bright[N_BRIGHTNESS] = { 0.35f, 0.65f, 1.0f };

/* ------------------------------------------------------------------------- */
/* Glyph model                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    GLYPH_GREEN,   /* fixed green        */
    GLYPH_YELLOW,  /* warm yellow accent */
} glyph_class_t;

typedef struct {
    const char *symbol;
    glyph_class_t cls;
    uint8_t bright;    /* brightness tier index 0..N_BRIGHTNESS-1 */
    int16_t x, y;      /* cell position (px) — FIXED, never changes */
    uint16_t dist;     /* distance from center (px)                */
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

static float    s_turn_deg = 0.0f;  /* accumulated ring turn, degrees      */
static float    s_reveal   = REVEAL_IDLE;
static int64_t  s_last_us  = 0;
static uint32_t s_poll_n   = 0;     /* for periodic telemetry              */

/* ------------------------------------------------------------------------- */
/* Colour helpers                                                            */
/* ------------------------------------------------------------------------- */

/* A glyph's full-lit colour: its class colour scaled down to its brightness
 * tier by mixing toward black (keeps the hue; only luminance changes). */
static lv_color_t base_color(const glyph_t *g)
{
    lv_color_t base = (g->cls == GLYPH_GREEN) ? RING_COLOR_GREEN
                                              : RING_COLOR_YELLOW;
    float frac = s_bright[g->bright];
    return lv_color_mix(base, RING_COLOR_BG, (uint8_t)((1.0f - frac) * 255.0f));
}

/* ------------------------------------------------------------------------- */
/* Repaint                                                                   */
/* ------------------------------------------------------------------------- */

/* Paint every glyph from the reveal radius. Cells never move; only their
 * colour changes. A cell is lit (at its brightness-tier colour) once the
 * reveal radius passes it; near the edge it blends from black over EDGE_FADE,
 * so the lit region has a soft, uneven boundary instead of a hard circle. */
static void repaint(float reveal)
{
    for (int i = 0; i < (int)N_GLYPHS; i++) {
        const glyph_t *g = &s_glyphs[i];
        lv_color_t c;

        if ((float)g->dist > reveal) {
            c = RING_COLOR_BG;                    /* outside the reveal    */
        } else {
            lv_color_t base = base_color(g);
            float edge = (float)g->dist - (reveal - EDGE_FADE);
            if (edge > 0.0f) {
                /* In the fade window: blend black -> base. */
                float f = 1.0f - edge / EDGE_FADE;      /* 0..1 */
                c = lv_color_mix(base, RING_COLOR_BG, (uint8_t)((1.0f - f) * 255.0f));
            } else {
                c = base;                             /* fully lit           */
            }
        }
        lv_obj_set_style_text_color(g->obj, c, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------------- */
/* IMU poll                                                                  */
/* ------------------------------------------------------------------------- */

static void poll_cb(lv_timer_t *timer)
{
    (void)timer;

    int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_us) / 1e6f;
    s_last_us = now;

    /* Gyro Z is the turn axis through the finger (perpendicular to the
     * screen). Deadband kills offset drift; TURN_SIGN sets the direction. */
    float gx, gy, gz;
    ring_imu_gyro(&gx, &gy, &gz);
    if (fabsf(gz) > GYRO_DEADBAND_DPS) {
        s_turn_deg += TURN_SIGN * gz * dt;
    }

    float reveal = REVEAL_IDLE + (s_turn_deg / DEG_PER_REVEAL) * 100.0f;
    if (reveal < REVEAL_IDLE) reveal = REVEAL_IDLE;
    if (reveal > REVEAL_MAX)  reveal = REVEAL_MAX;

    if (fabsf(reveal - s_reveal) > 0.5f) {
        s_reveal = reveal;
        RING_UI_LOCKED(-1) { repaint(reveal); }
    }

    /* Periodic telemetry so the mapping can be calibrated on-device. */
    if ((s_poll_n++ % 50) == 0) {   /* ~1 s */
        ESP_LOGI(TAG, "turn %.0f deg, reveal %.0f px (%.0f..%.0f)",
                 s_turn_deg, s_reveal, REVEAL_IDLE, REVEAL_MAX);
    }
}

/* ------------------------------------------------------------------------- */
/* Input — tap resets to the idle core                                       */
/* ------------------------------------------------------------------------- */

static void input_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_turn_deg = 0.0f;
    s_reveal = REVEAL_IDLE;
    RING_UI_LOCKED(-1) { repaint(s_reveal); }
    ESP_LOGI(TAG, "reset -> idle core");
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

            /* Random green/warm-yellow split + random brightness tier. */
            s_glyphs[idx].cls    = ring_rand_bool() ? GLYPH_YELLOW : GLYPH_GREEN;
            s_glyphs[idx].bright = (uint8_t)ring_rand_below(N_BRIGHTNESS);

            s_glyphs[idx].obj = ring_ui_add_label(scr, s_glyphs[idx].symbol,
                                                  ICON_FONT, RING_COLOR_BG);
        }
    }

    /* Centre every glyph on its cell point. This is the ONLY layout pass —
     * positions are never touched again (cells do not move). */
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

        s_last_us = esp_timer_get_time();
        repaint(s_reveal);   /* start at the idle core */

        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);
        lv_timer_create(poll_cb, POLL_MS, NULL);
    }

    ESP_LOGI(TAG, "ready: %d glyphs, %d cells, reveal %.0f..%.0f px, "
                  "%d deg/100px, turn sign %+.1f",
             (int)N_GLYPHS, GRID_N, REVEAL_IDLE, REVEAL_MAX,
             (int)DEG_PER_REVEAL, TURN_SIGN);
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "glyph-anime";
}
