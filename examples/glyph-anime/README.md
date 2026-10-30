# glyph-anime — glyph-grid layered-reveal design & build spec

Design document for `examples/glyph-anime/`, the **default** application on the
Smart Ring Plus. It replicates the interaction seen in
`@KINDOFDEVICE_`'s Smart Ring Plus demo: a **full-screen grid of glyphs** that
is **built up layer by layer from the center outward** (concentric rings of
glyphs added until the edge), **color-cycles its accent icons**, and
**collapses back** to a small lit cluster at the center.

> **Correction:** the interface does **not** scale. There is no zoom/scale
> transform anywhere. The screen is a fixed grid of glyph labels; the open/close
> effect is a **layered reveal** — the grid is split into concentric layers by
> distance from the center, and layers light up one at a time. Glyphs in a layer
> that has not been reached are invisible (painted the background color), so
> rings of glyphs appear to be added around the center outward (and are removed
> outside-in on close).

Target: VIEWE Smart Ring Plus, ESP32-S3, 1.8" 360×360 ST77916 round panel,
LVGL **9.4.0**, BSP `viewesmart/smartring_plus`. Implementation:
`examples/glyph-anime/glyph_anime.c`.

---

## 1. Observed behavior (from video)

| Phase | Visual |
|-------|--------|
| Idle | Screen dark; a small cluster of glyphs lit near the center (innermost 2 layers, ~9 cells) |
| Open | **Layers of glyphs are added ring by ring from the center outward** (~0.4–0.5 s, ease-out) until the panel is full — a layered reveal, not a zoom; each concentric ring fades in as one unit |
| Open (held) | Dense grid fills the panel using only **~6 distinct glyph shapes** (`#`, `+`, `×`, filled square, dot, open-circle). **Green vs accent is scattered at random** (not in rings); green stays constant while accents cycle hue: orange → pink → blue → cream |
| Close | Rings **disappear outside-in** (~0.3–0.4 s) until only the tiny center cluster remains |

> The demo's timings were ~450 ms open / ~350 ms close. This app deliberately
> runs them **3× slower** via `RING_MS_REVEAL_OPEN` (1350 ms) and
> `RING_MS_REVEAL_CLOSE` (1050 ms) so the layering reads clearly — see §6.

---

## 2. Architecture

```
examples/glyph-anime/
├── CMakeLists.txt              # fragment: RING_EXAMPLE_SRCS etc. (see examples/)
├── include/ring_example.h      # esp_err_t ring_example_start(void);
├── glyph_anime.c               # state machine + glyph grid + reveal anim + input
└── README.md                   # this design & build spec
components/ringkit/             # skeleton: board, LVGL lock, theme, RNG
```

- No new BSP code. Everything uses `lv_*` under `RING_UI_LOCKED(-1)`.
- `glyph_anime.c` owns the whole UI; `main.c` only calls `ring_example_start()`.
- One `lv_obj` (screen). All glyphs are `lv_label` children of the screen placed
  on a **fixed grid**.
- There is **no scale transform**. The open/close effect is a **layered
  reveal**: the grid is split into concentric layers by distance from center,
  and a progress value advances one layer at a time. A glyph is lit (target
  color) once its layer's window has been passed, otherwise it is painted the
  **background color** (black), i.e. invisible. Each layer lights up as a whole
  ring, so the lit set grows layer by layer outward from the center.

### State machine

```
IDLE ──tap──▶ OPENING ──(reveal anim done)──▶ OPEN
  ▲                                            │
  │                                            │ tap
  └───────CLOSING ◀──(reveal anim done)─────────┘
```

States: `L_IDLE`, `L_OPENING`, `L_OPEN`, `L_CLOSING` (`state_t`).

---

## 3. Layout (fixed grid, 360×360)

Panel center = `(RING_CX, RING_CY)` = (180, 180). Glyphs sit on a square grid;
nothing moves.

| Param | Value | Note |
|-------|-------|------|
| `GRID_N` | 13 | 13×13 = **169** glyphs |
| `GRID_STEP` | 27 px | extent = 13·27 = 351 px → ~4.5 px margin each side |
| `LAYER_UNIT` | 256 | fixed-point progress units per layer |
| `LAYER_FADE` | 128 | fade window per layer (½ a slot → visible step between rings) |
| `MAX_LAYERS` | 10 | safety cap on the layer index |
| `n_layers` | 9 | derived: `max(round(dist/GRID_STEP)) + 1` |
| `IDLE_PROGRESS` | 384 | `1·LAYER_UNIT + LAYER_FADE` → lights layers 0–1 (the idle cluster) |

Glyph position for cell (i, j), i,j ∈ [0, 12]:

```c
int half = (GRID_N - 1) / 2;                    /* 6 */
int x = RING_CX + (i - half) * GRID_STEP;
int y = RING_CY + (j - half) * GRID_STEP;
```

Each glyph is assigned to a **concentric layer** by its distance from center.
The layer — not a raw radius — is the reveal key:

```c
uint16_t dist  = (uint16_t)roundf(hypotf(x - RING_CX, y - RING_CY));
int      layer = (int)roundf((float)dist / GRID_STEP);   // clamp to MAX_LAYERS-1
```

On this grid that yields **9 layers**, with these glyph counts (a square grid
means layers are not perfectly even rings — that is expected):

| Layer | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|-------|---|---|---|---|---|---|---|---|---|
| Glyphs | 1 | 8 | 12 | 16 | 32 | 28 | 40 | 20 | 12 |

Layers 0–1 (9 glyphs) form the idle cluster; the rest fade in one layer at a
time as the reveal advances.

---

## 4. Icon (glyph) model

```c
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

static glyph_t s_glyphs[GRID_N * GRID_N];   /* 169 entries */
```

- **Only ~6 distinct glyph shapes**, approximating the footage's pixel marks
  (`#`, `+`, `×`, filled square, dot, open-circle). The video uses custom pixel
  glyphs; we approximate with a small `LV_SYMBOL_*` set:

  ```c
  static const char *const s_syms[6] = {
      LV_SYMBOL_PLUS,    /* +        */
      LV_SYMBOL_CLOSE,   /* x        */
      LV_SYMBOL_STOP,    /* square   */
      LV_SYMBOL_BULLET,  /* dot      */
      LV_SYMBOL_PLAY,    /* triangle */
      LV_SYMBOL_PAUSE,   /* bars     */
  };
  ```

  Assign each cell a glyph with a deterministic pseudo-random pick
  (`s_syms[ring_rand_below(N_SYMS)]`), **not** a cyclic walk, so the field looks
  organic.
- **Class assignment is random, not concentric bands.** Each cell is
  `ICON_ACCENT` with ~50 % probability (`ring_rand_bool()`) so green glyphs are
  scattered randomly across the field like the footage, with no ring structure.
- `ICON_STATIC_GREEN` → `RING_COLOR_GREEN` (theme).
- `ICON_ACCENT` → colored from the cycling hue (see §5).
- Glyphs are `lv_label`s (symbols render with the built-in Montserrat fonts —
  symbol glyphs are baked in, no extra font needed).
- **Randomness is `ringkit`'s `ring_rand`**, seeded once with
  `ring_rand_seed(0x9E3779B9u)` at start. The whole field is reproducible
  across boots. (The LCG low-bit trap that made a local RNG unsuitable is
  documented in `ringkit/ring_rand`.)

---

## 5. Accent color cycle

A single `lv_anim` on a `float hue` (0–360) runs while the launcher is OPEN.
Each frame the exec callback repaints every `ICON_ACCENT` label (this shares the
reveal repaint — see §6):

```c
/* palette stops from the footage: orange → pink → blue → cream */
static const uint16_t s_palette[4] = { 30, 320, 230, 45 };

static void color_cb(void *var, int32_t v)
{
    s_hue = v;
    repaint(s_progress);
}
/* lv_anim: start_hue -> start_hue + 360 over 6000 ms, linear, repeat infinite */
```

- The sweep starts at the current palette stop `s_palette[s_palette_idx]` and
  runs one full 360° pass, so orange→pink→blue→cream reads like the footage.
- The fixed green pattern provides contrast exactly like the footage.

---

## 6. Open / close reveal — layers of glyphs (no scaling)

Progress is a fixed-point value in `LAYER_UNIT` (256) units per layer, animated
from `IDLE_PROGRESS` to `n_layers · LAYER_UNIT`. Each glyph maps its own layer
onto that progress:

```c
static void repaint(int32_t progress)
{
    for (int i = 0; i < N_GLYPHS; i++) {
        int32_t local = progress - (int32_t)s_glyphs[i].layer * LAYER_UNIT;
        lv_color_t c;

        if (local <= 0) {
            c = RING_COLOR_BG;                    /* layer not reached yet */
        } else if (local >= LAYER_FADE) {
            c = target_color(&s_glyphs[i]);       /* layer fully lit       */
        } else {
            c = lv_color_mix(target_color(&s_glyphs[i]), RING_COLOR_BG,
                             (uint8_t)(local * 255 / LAYER_FADE));
        }
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}
```

Because a whole layer shares the same `layer` value, **all glyphs in a layer
light up together as one ring**; because the windows are sequential, rings are
added one after another outward — *layers and layers of glyphs growing from the
center*. `LAYER_FADE` (128) is half a slot, so each ring finishes fading in
before the next starts, leaving a short gap that makes the steps readable.
Shrinking progress removes rings outside-in.

| Transition | Progress | Duration | Ease |
|-----------|----------|----------|------|
| Open | 384 → `9·256` (2304) | `RING_MS_REVEAL_OPEN` (1350 ms, 3× slower) | `lv_anim_path_ease_out` |
| Close | 2304 → 384 | `RING_MS_REVEAL_CLOSE` (1050 ms, 3× slower) | `lv_anim_path_ease_in` |

With 9 layers over 1350 ms each layer gets a ~150 ms slot (≈75 ms fade, ≈75 ms
gap). Tune `RING_MS_REVEAL_OPEN` / `LAYER_FADE` to make the stepping more or
less obvious; lowering `LAYER_FADE` makes each ring snap in harder.

On open-complete → state `OPEN`, start color anim (`open_done`).
On close-complete → state `IDLE`, stop/delete color anim, re-paint at
`IDLE_PROGRESS` (`close_done`).

Use `lv_anim_set_ready_cb` to transition the state machine at the end of each
reveal. The idle cluster IS the same grid with progress `IDLE_PROGRESS` — no
separate widget, no scale.

> `lv_color_mix(c1, c2, mix)` takes `mix = 255 → c1`, `0 → c2`, so
> `lv_color_mix(target, RING_COLOR_BG, t)` fades a glyph from the black
> background up to its target color.

---

## 7. Input

`esp_lv_adapter` already registers the CST816 touch indev, so use standard
LVGL events on the screen / grid:

- **Tap (`LV_EVENT_CLICKED`)** while `IDLE` → `launcher_open()`.
- **Tap (`LV_EVENT_CLICKED`)** while `OPEN` → `launcher_close()`.
- **Swipe (`LV_EVENT_GESTURE`)** while `OPEN` → advance `s_palette_idx` to the
  next stop (orange→pink→blue→cream) and restart the hue sweep from there.

Register on the **screen** so taps anywhere toggle, not just on glyphs.
Guard state transitions with `lv_anim` so a tap mid-animation either queues the
reverse or is ignored (the anim's `ready_cb` owns the transition).

---

## 8. Files

1. **`examples/glyph-anime/glyph_anime.c`** — the whole implementation.
2. **`examples/glyph-anime/include/ring_example.h`** — `ring_example_start()` /
   `ring_example_name()`.
3. **`examples/glyph-anime/CMakeLists.txt`** — fragment listing `RING_EXAMPLE_SRCS`.
4. **`main/main.c`** — app-agnostic; calls `ring_board_init()` then
   `ring_example_start()`.
5. **`sdkconfig.defaults`** — fonts (already set):
   ```
   CONFIG_LV_FONT_MONTSERRAT_20=y
   CONFIG_LV_FONT_MONTSERRAT_24=y
   ```
   (symbol glyphs are included in the built-in Montserrat fonts.)
6. **`examples/glyph-anime/README.md`** — this document.

> Remember: `sdkconfig.defaults` is only applied when `sdkconfig` is
> (re)created → run `rm -f sdkconfig && idf.py reconfigure` after editing it.

---

## 9. Build & verify

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # watch TAG "glyph-anime"
```

Expected log:

```
glyph-anime: ready (idle): 169 glyphs, 13 cells, 9 layers
glyph-anime: opening: 9 layers
glyph-anime: opened
glyph-anime: palette -> 320
glyph-anime: closing...
glyph-anime: closed -> idle
```

Visual check: tap → rings of glyphs are added one after another from the center
outward until the panel is full (no scaling), green pattern fixed, accents cycle
orange→pink→blue→cream; swipe advances the palette; tap again → rings disappear
outside-in back to the tiny center cluster.

---

## 10. Risks / notes

- **No transform scaling** — the layered-reveal repaint approach avoids LVGL
  transform pivot/scale quirks entirely. Glyphs never move.
- **PSRAM flush limit:** do NOT enlarge the BSP's 40-line draw buffer
  (full-panel buffering exhausts internal RAM → `ESP_ERR_NO_MEM`). The reveal
  only changes text colors; LVGL clips/redraws in software on the same buffer.
- **Reveal cost:** up to 169 `lv_obj_set_style_text_color` calls per anim frame
  (plus the color cycle while OPEN). This is cheap (style property writes) and
  fine at 60 fps; avoid anything heavier per frame.
- **All LVGL calls inside `RING_UI_LOCKED(-1) { ... }`.** The `lv_anim`
  exec/ready callbacks run on the LVGL task, so they're already locked;
  user-input callbacks are dispatched by the adapter's task too.
- **These are square-grid layers, not perfect circles** — layer counts are
  uneven (1/8/12/16/32/28/40/20/12). That is the intended look: rings
  quantized to the grid, not a smooth circular wavefront.
- **`lv_color_hsv_to_rgb`** and **`lv_color_mix`** are available in LVGL 9.4
  (`lv_color_mix(c1, c2, mix)`: `mix=255 → c1`, `0 → c2`); no extra component.
- **Symbol fonts:** the `LV_SYMBOL_*` glyphs (0xF000+) ship inside the
  built-in Montserrat fonts — `CONFIG_LV_FONT_MONTSERRAT_20` is already on.
- **Timings come from the theme** (`RING_MS_REVEAL_OPEN` / `_CLOSE` in
  `ring_theme.h`), not from the app, so all apps share one motion language.

---

## 11. Reference implementation skeleton (`examples/glyph-anime/glyph_anime.c`)

```c
#include "ring_example.h"
#include "ringkit/ringkit.h"
#include "esp_log.h"
#include <math.h>

#define TAG "glyph-anime"

#define GRID_N    13   /* 13x13 = 169 glyphs                         */
#define GRID_STEP 27   /* px between cells (extent = 13*27 = 351 px) */
#define LAYER_UNIT 256 /* progress units per layer (fixed point)     */
#define LAYER_FADE 128 /* fade window per layer (half a slot)        */
#define MAX_LAYERS 10  /* safety cap on the layer index              */
#define IDLE_PROGRESS (1 * LAYER_UNIT + LAYER_FADE)  /* lights layers 0-1 */

#define ICON_FONT (&lv_font_montserrat_20)

/* Palette stops from the footage: orange → pink → blue → cream. */
static const uint16_t s_palette[4] = { 30, 320, 230, 45 };

/* ~6 distinct glyph shapes, approximated with LV_SYMBOL. */
static const char *const s_syms[6] = {
    LV_SYMBOL_PLUS,   /* +        */
    LV_SYMBOL_CLOSE,  /* x        */
    LV_SYMBOL_STOP,   /* square   */
    LV_SYMBOL_BULLET, /* dot      */
    LV_SYMBOL_PLAY,   /* triangle */
    LV_SYMBOL_PAUSE,  /* bars     */
};

typedef enum { ICON_STATIC_GREEN, ICON_ACCENT } glyph_class_t;

typedef struct {
    const char *symbol;
    glyph_class_t cls;
    int16_t x, y;
    uint16_t dist;
    uint8_t layer;    /* concentric layer = round(dist / GRID_STEP) */
    lv_obj_t *obj;
} glyph_t;

static glyph_t s_glyphs[GRID_N * GRID_N];
#define N_GLYPHS (GRID_N * GRID_N)

static int32_t s_hue = 0;          /* animated hue (var for lv_anim) */
static int32_t s_progress = 0;     /* layered reveal progress        */
static int     s_n_layers = 1;     /* highest layer index + 1        */
static int     s_palette_idx = 0;  /* current palette stop           */

typedef enum { L_IDLE, L_OPENING, L_OPEN, L_CLOSING } state_t;
static state_t s_state = L_IDLE;

static lv_color_t target_color(const glyph_t *g)
{
    if (g->cls == ICON_STATIC_GREEN)
        return RING_COLOR_GREEN;
    return lv_color_hsv_to_rgb((uint16_t)s_hue, RING_HUE_SAT, RING_HUE_VAL);
}

/* Layered reveal: each glyph's layer lights as a whole ring, in order. */
static void repaint(int32_t progress)
{
    for (int i = 0; i < N_GLYPHS; i++) {
        int32_t local = progress - (int32_t)s_glyphs[i].layer * LAYER_UNIT;
        lv_color_t c;

        if (local <= 0)
            c = RING_COLOR_BG;                    /* layer not reached yet */
        else if (local >= LAYER_FADE)
            c = target_color(&s_glyphs[i]);       /* layer fully lit       */
        else
            c = lv_color_mix(target_color(&s_glyphs[i]), RING_COLOR_BG,
                             (uint8_t)(local * 255 / LAYER_FADE));

        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}

static void reveal_cb(void *var, int32_t v)
{
    s_progress = v;
    repaint(v);
}

static void color_cb(void *var, int32_t v)
{
    s_hue = v;
    repaint(s_progress);
}

static void open_done(lv_anim_t *a);  /* -> L_OPEN, start_color_anim() */
static void close_done(lv_anim_t *a); /* -> L_IDLE, stop_color_anim()  */

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

/* launcher_open():  reveal(IDLE_PROGRESS, s_n_layers * LAYER_UNIT,
 *                          RING_MS_REVEAL_OPEN, lv_anim_path_ease_out, open_done);
 * launcher_close(): reveal(s_n_layers * LAYER_UNIT, IDLE_PROGRESS,
 *                          RING_MS_REVEAL_CLOSE, lv_anim_path_ease_in, close_done); */

esp_err_t ring_example_start(void)
{
    ring_rand_seed(0x9E3779B9u);   /* reproducible field */

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();
        const int half = (GRID_N - 1) / 2;

        /* Build the 13x13 grid of glyph labels, classify accents. */
        for (int i = 0; i < GRID_N; i++) {
            for (int j = 0; j < GRID_N; j++) {
                int idx = i * GRID_N + j;
                int x = RING_CX + (i - half) * GRID_STEP;
                int y = RING_CY + (j - half) * GRID_STEP;

                s_glyphs[idx].x = x;
                s_glyphs[idx].y = y;
                s_glyphs[idx].dist =
                    (uint16_t)roundf(hypotf(x - RING_CX, y - RING_CY));
                s_glyphs[idx].symbol = s_syms[ring_rand_below(N_SYMS)];
                s_glyphs[idx].cls =
                    ring_rand_bool() ? ICON_ACCENT : ICON_STATIC_GREEN;

                /* Concentric layer = grid distance from center (reveal key). */
                int layer = (int)roundf((float)s_glyphs[idx].dist / GRID_STEP);
                if (layer >= MAX_LAYERS) layer = MAX_LAYERS - 1;
                s_glyphs[idx].layer = (uint8_t)layer;
                if (layer + 1 > s_n_layers) s_n_layers = layer + 1;

                lv_obj_t *l = lv_label_create(scr);
                lv_label_set_text(l, s_glyphs[idx].symbol);
                lv_obj_set_style_text_font(l, ICON_FONT, LV_PART_MAIN);
                lv_obj_set_style_text_color(l, RING_COLOR_BG, LV_PART_MAIN);
                lv_obj_set_pos(l, x, y);
                s_glyphs[idx].obj = l;
            }
        }

        s_progress = IDLE_PROGRESS;
        repaint(s_progress);            /* paint the idle cluster */
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
```

> The skeleton is illustrative — the full version fills the symbol/class
> assignment, the palette swipe and the state-machine transitions.
> `ring_ui_screen()` already clears the screen to the theme background, and
> `RING_UI_LOCKED` releases the lock on every exit path. See
> `examples/glyph-anime/glyph_anime.c` for the implementation that actually
> ships.
