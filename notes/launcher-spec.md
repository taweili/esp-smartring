# Circular App-Grid Launcher — Design & Build Spec

Replicates the interaction seen in `@KINDOFDEVICE_`'s Smart Ring Plus demo:
a **full-screen grid of glyphs** that **lights up from the center outward**
(a reveal wave that grows to the edge), **color-cycles its accent icons**,
and **collapses back** to a small lit cluster at the center.

> **Correction:** the interface does **not** scale. There is no zoom/scale
> transform anywhere. The screen is a fixed grid of glyph labels; the open/close
> effect is a **reveal radius** — glyphs are invisible (background color) until
> the radius reaches them, so the lit region appears to grow from the center to
> the edge, and shrink back on close.

Target: VIEWE Smart Ring Plus, ESP32-S3, 1.8" 360×360 ST77916 round panel,
LVGL **9.4.0**, BSP `viewesmart/smartring_plus`.

---

## 1. Observed behavior (from video)

| Phase | Visual |
|-------|--------|
| Idle | Screen dark; a small cluster of glyphs lit near the center (a few center cells) |
| Open | Glyphs **light up starting at the center** and the lit region **grows outward to the edge** (~0.4–0.5 s, ease-out) — a reveal wave, not a zoom |
| Open (held) | Dense grid fills the panel using only **~6 distinct glyph shapes** (`#`, `+`, `×`, filled square, dot, open-circle). **Green vs accent is scattered at random** (not in rings); green stays constant while accents cycle hue: orange → pink → blue → cream |
| Close | Lit region **collapses back toward the center** (~0.3–0.4 s) until only the tiny center cluster remains |

---

## 2. Architecture

```
main/
├── main.c          # BSP init sequence, then launcher_start()
├── launcher.h      # public API: esp_err_t launcher_start(void);
└── launcher.c      # state machine + glyph grid + reveal anim + input
```

- No new BSP code. Everything uses `lv_*` under `esp_lv_adapter_lock()`.
- `launcher.c` owns the whole UI; `main.c` only calls `launcher_start()`.
- One `lv_obj` (screen). All glyphs are `lv_label` children of the screen (or a
  single non-scaled container) placed on a **fixed grid**.
- There is **no scale transform**. The open/close effect is driven by a
  **reveal radius** `R`: a glyph is "lit" (target color) when its distance from
  the center is `≤ R`, otherwise it is painted the **background color** (black),
  i.e. invisible. Growing `R` from idle → max reveals the glyphs center-to-edge;
  shrinking it reverses the effect.

### State machine

```
IDLE ──tap──▶ OPENING ──(reveal anim done)──▶ OPEN
  ▲                                            │
  │                                            │ tap
  └───────CLOSING ◀──(reveal anim done)─────────┘
```

States: `LAUNCHER_IDLE`, `LAUNCHER_OPENING`, `LAUNCHER_OPEN`, `LAUNCHER_CLOSING`.

---

## 3. Layout (fixed grid, 360×360)

Panel center = (180, 180). Glyphs sit on a square grid; nothing moves.

| Param | Value | Note |
|-------|-------|------|
| `GRID_N` | 13 | 13×13 = **169** glyphs |
| `GRID_STEP` | 27 px | extent = 13·27 = 351 px → ~4.5 px margin each side |
| `IDLE_R` | 30 px | lit radius at idle → the ~5 center glyphs form the tiny cluster |
| `MAX_R` | 240 px | reaches the corners (√(168²+168²) ≈ 238) |

Glyph position for cell (i, j), i,j ∈ [0, 12]:

```c
int x = CX + (i - (GRID_N - 1) / 2) * GRID_STEP;
int y = CY + (j - (GRID_N - 1) / 2) * GRID_STEP;
```

Each glyph's reveal key is its distance from the center:

```c
uint16_t dist = (uint16_t)roundf(hypotf(x - CX, y - CY));
```

Glyphs with `dist <= IDLE_R` are lit at idle (center + 4 edge-adjacent cells);
the rest are background-colored until the radius reaches them.

---

## 4. Icon (glyph) model

```c
typedef enum { ICON_STATIC_GREEN, ICON_ACCENT } icon_class_t;

typedef struct {
    const char *symbol;     // LV_SYMBOL_*
    icon_class_t cls;
    int16_t x, y;           // grid position (px)
    uint16_t dist;          // distance from center (reveal key)
    lv_obj_t *obj;          // the lv_label (filled at build time)
} launcher_glyph_t;

static launcher_glyph_t s_glyphs[GRID_N * GRID_N];   // 169 entries
```

- **Only ~6 distinct glyph shapes**, approximating the footage's pixel marks
  (`#`, `+`, `×`, filled square, dot, open-circle). The video uses custom pixel
  glyphs; we approximate with a small `LV_SYMBOL_*` set:

  ```c
  static const char *const s_syms[6] = {
      LV_SYMBOL_PLUS,    /* +        */
      LV_SYMBOL_CLOSE,   /* ×        */
      LV_SYMBOL_STOP,    /* ■ square */
      LV_SYMBOL_BULLET,  /* • dot    */
      LV_SYMBOL_PLAY,    /* ▶        */
      LV_SYMBOL_PAUSE,   /* ‖        */
  };
  ```

  Assign each cell a glyph with a deterministic pseudo-random pick
  (`s_syms[rnd() % 6]`), **not** a cyclic walk, so the field looks organic.
- **Class assignment is random, not concentric bands.** Each cell is
  `ICON_ACCENT` with ~50 % probability from a deterministic RNG (reproducible)
  so green glyphs are scattered randomly across the field like the footage,
  with no ring structure.
- `ICON_STATIC_GREEN` → fixed `lv_color_hex(0x00E676)` (green).
- `ICON_ACCENT` → colored from the cycling hue (see §5).
- Glyphs are `lv_label`s (symbols render with the built-in Montserrat fonts —
  symbol glyphs are baked in, no extra font needed).

---

## 5. Accent color cycle

A single `lv_anim` on a `float hue` (0–360) runs while the launcher is OPEN.
Each frame the exec callback repaints every `ICON_ACCENT` label (this shares the
reveal repaint — see §6):

```c
static void color_cb(void *var, int32_t v)
{
    s_hue = (float)v;
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)s_hue, 90, 95);
    /* apply c to each ICON_ACCENT label's text color */
}
/* lv_anim: 0 -> 360 over ~5000 ms, LV_ANIM_REPEAT_INFINITE, linear */
```

The video's palette (orange→pink→blue→cream) is approximately HSV hue
**30 → 320 → 230 → 45**, so a full 360° sweep reads as the same effect. The
fixed green pattern provides contrast exactly like the footage.

---

## 6. Open / close reveal (no scaling)

Animate the **reveal radius** `R` and repaint glyphs each frame:

```c
static void reveal_cb(void *var, int32_t v)
{
    int r = (int)v;
    for (int i = 0; i < N_GLYPHS; i++) {
        lv_color_t c = (s_glyphs[i].dist <= r) ? target_color(&s_glyphs[i])
                                               : COLOR_BG;
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}
```

| Transition | Radius | Duration | Ease |
|-----------|--------|----------|------|
| Open | `IDLE_R` (30) → `MAX_R` (240) | 450 ms | `LV_EASE_OUT_QUART` |
| Close | `MAX_R` → `IDLE_R` | 350 ms | `LV_EASE_IN_QUART` |

On open-complete → state `OPEN`, start color anim.
On close-complete → state `IDLE`, stop/delete color anim, re-paint at `IDLE_R`.

Use `lv_anim_set_ready_cb` to transition the state machine at the end of each
reveal. The idle cluster IS the same grid with `R = IDLE_R` — no separate
widget, no scale.

> Hard-edge wave (glyph fully lit or fully background) matches the footage. If a
> softer edge is wanted later, interpolate each glyph's color by how far `dist`
> is inside `R`, or stagger a per-glyph color `lv_anim` by `dist`. Keep v1
> hard-edge for simplicity.

---

## 7. Input

`esp_lv_adapter` already registers the CST816 touch indev, so use standard
LVGL events on the screen / grid:

- **Tap (LV_EVENT_CLICKED)** while `IDLE` → `launcher_open()`.
- **Tap (LV_EVENT_CLICKED)** while `OPEN` → `launcher_close()`.
- **Swipe (LV_EVENT_GESTURE)** while `OPEN` → jump the accent hue to the next
  palette stop (orange→pink→blue→cream), i.e. page-forward the color cycle.
  *(Nice-to-have; optional in v1.)*

Register on the **screen** so taps anywhere toggle, not just on glyphs.
Guard state transitions with `lv_anim` / `lv_timer` so a tap mid-animation
either queues the reverse or is ignored.

---

## 8. Files to change

1. **`main/CMakeLists.txt`** — add `launcher.c` to `SRCS`.
2. **`main/main.c`** — replace the Hello-World UI block with
   `ESP_ERROR_CHECK(launcher_start());` (keep BSP init order).
3. **`main/launcher.h`** — new: `esp_err_t launcher_start(void);`
4. **`main/launcher.c`** — new: full implementation (skeleton below).
5. **`sdkconfig.defaults`** — add fonts:
   ```
   CONFIG_LV_FONT_MONTSERRAT_20=y
   CONFIG_LV_FONT_MONTSERRAT_24=y
   ```
   (symbol glyphs are included in the built-in Montserrat fonts.)
6. **`notes/launcher-spec.md`** — this document.

> Remember: `sdkconfig.defaults` is only applied when `sdkconfig` is
> (re)created → run `rm -f sdkconfig && idf.py reconfigure` after editing it.

---

## 9. Build & verify

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # watch TAG "launcher"
```

Expected log: `launcher: idle`, `launcher: opening -> open`, `launcher: closing -> idle`.

Visual check: tap → glyphs light up from the center and the lit region grows to
the edge (no scaling), green pattern fixed, accents cycle
orange→pink→blue→cream; tap again → lit region collapses back to the tiny
center cluster.

---

## 10. Risks / notes

- **No transform scaling** — the reveal repaint approach avoids LVGL transform
  pivot/scale quirks entirely. Glyphs never move.
- **PSRAM flush limit:** do NOT enlarge the BSP's 40-line draw buffer
  (full-panel buffering exhausts internal RAM → `ESP_ERR_NO_MEM`). The reveal
  only changes text colors; LVGL clips/redraws in software on the same buffer.
- **Reveal cost:** up to 169 `lv_obj_set_style_text_color` calls per anim frame
  (plus the color cycle while OPEN). This is cheap (style property writes) and
  fine at 60 fps; avoid anything heavier per frame.
- **All LVGL calls inside `esp_lv_adapter_lock(-1)` / `unlock()`.** The
  `lv_anim` exec/ready callbacks run on the LVGL task, so they're already
  locked; user-input callbacks are dispatched by the adapter's task too.
- **`lv_color_hsv_to_rgb`** is available in LVGL 9.4; no extra component.
- **Symbol fonts:** the `LV_SYMBOL_*` glyphs (0xF000+) ship inside the
  built-in Montserrat fonts — `CONFIG_LV_FONT_MONTSERRAT_14/16` is already on.

---

## 11. Reference implementation skeleton (`main/launcher.c`)

```c
#include "launcher.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>

#define TAG "launcher"

#define CX        180
#define CY        180
#define GRID_N    13
#define GRID_STEP 27
#define IDLE_R    30      /* lit radius at idle -> tiny center cluster */
#define MAX_R     240     /* reaches panel corners (~238) */

#define ICON_FONT  (&lv_font_montserrat_20)
#define COLOR_GREEN lv_color_hex(0x00E676)
#define COLOR_BG    lv_color_black()

/* The footage uses ~6 custom pixel glyphs; approximate with LV_SYMBOL. */
static const char *const s_syms[6] = {
    LV_SYMBOL_PLUS,   /* +        */
    LV_SYMBOL_CLOSE,  /* x        */
    LV_SYMBOL_STOP,   /* square   */
    LV_SYMBOL_BULLET, /* dot      */
    LV_SYMBOL_PLAY,   /* triangle */
    LV_SYMBOL_PAUSE,  /* bars     */
};

/* Tiny deterministic xorshift32 RNG so the random glyph/color field is
 * reproducible. (A plain LCG has degenerate low bits: `% 6` / `& 1` would
 * collapse to even values only, giving an all-accent field.) */
static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

typedef enum { ICON_STATIC_GREEN, ICON_ACCENT } icon_class_t;

typedef struct {
    const char *symbol;
    icon_class_t cls;
    int16_t x, y;
    uint16_t dist;
    lv_obj_t *obj;
} launcher_glyph_t;

static launcher_glyph_t s_glyphs[GRID_N * GRID_N];
#define N_GLYPHS (sizeof(s_glyphs) / sizeof(s_glyphs[0]))

static lv_obj_t *s_accent[64];   /* ICON_ACCENT labels, for the color anim */
static int s_n_accent;
static float s_hue = 0.0f;

typedef enum { L_IDLE, L_OPENING, L_OPEN, L_CLOSING } launcher_state_t;
static launcher_state_t s_state = L_IDLE;

static lv_color_t target_color(const launcher_glyph_t *g)
{
    if (g->cls == ICON_STATIC_GREEN)
        return COLOR_GREEN;
    return lv_color_hsv_to_rgb((uint16_t)s_hue, 90, 95);
}

/* Reveal: glyphs within `r` px of center are lit, rest are background. */
static void reveal_cb(void *var, int32_t v)
{
    int r = (int)v;
    for (int i = 0; i < N_GLYPHS; i++) {
        lv_color_t c = (s_glyphs[i].dist <= r) ? target_color(&s_glyphs[i])
                                               : COLOR_BG;
        lv_obj_set_style_text_color(s_glyphs[i].obj, c, LV_PART_MAIN);
    }
}

/* Accent hue cycle (only runs while OPEN). */
static void color_cb(void *var, int32_t v)
{
    s_hue = (float)v;
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)s_hue, 90, 95);
    for (int i = 0; i < s_n_accent; i++)
        lv_obj_set_style_text_color(s_accent[i], c, LV_PART_MAIN);
}

static void reveal_done(lv_anim_t *a) { /* state transitions */ }

static void reveal(int from, int to, uint32_t dur,
                   lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out_quart);
    lv_anim_set_exec_cb(&a, reveal_cb);
    lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

/* ... launcher_open(), launcher_close(), input event handler ... */

esp_err_t launcher_start(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Build the 13x13 grid of glyph labels, classify accents. */
    for (int i = 0; i < GRID_N; i++) {
        for (int j = 0; j < GRID_N; j++) {
            int idx = i * GRID_N + j;
            int x = CX + (i - (GRID_N - 1) / 2) * GRID_STEP;
            int y = CY + (j - (GRID_N - 1) / 2) * GRID_STEP;

            s_glyphs[idx].x = x;
            s_glyphs[idx].y = y;
            s_glyphs[idx].dist = (uint16_t)roundf(hypotf(x - CX, y - CY));
            s_glyphs[idx].symbol = s_syms[rnd() % 6];
            s_glyphs[idx].cls = (rnd() & 1) ? ICON_ACCENT : ICON_STATIC_GREEN;

            lv_obj_t *l = lv_label_create(scr);
            lv_label_set_text(l, s_glyphs[idx].symbol);
            lv_obj_set_style_text_font(l, ICON_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, COLOR_BG, LV_PART_MAIN);
            lv_obj_set_pos(l, x, y);
            s_glyphs[idx].obj = l;

            if (s_glyphs[idx].cls == ICON_ACCENT && s_n_accent < 64)
                s_accent[s_n_accent++] = l;
        }
    }

    reveal_cb(NULL, IDLE_R);        /* paint the idle cluster */
    lv_obj_add_event_cb(scr, launcher_input_cb, LV_EVENT_CLICKED, NULL);

    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "launcher ready (idle)");
    return ESP_OK;
}
```

> The skeleton is illustrative — the full version fills the symbol/class
> assignment, the accent array, and the state-machine transitions. Build first,
> then flesh out `launcher.c`.
