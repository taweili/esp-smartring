# glyph-anime — ring-rotation-driven glyph reveal

Design document for `examples/glyph-anime/`, the **default** application on the
Smart Ring Plus. It implements the **rotation-driven glyph reveal**: a **fixed
grid of glyphs** whose lit set grows and shrinks as the **ring is turned
clockwise / counter-clockwise**.

> **The grid does NOT rotate, scale or move.** Cells are fixed. The interaction
> is a **reveal**: turning the ring clockwise lights up more cells (center-out)
> until the panel is full; turning it counter-clockwise un-lights them
> (outside-in). The reveal is driven by the on-board **IMU gyro Z**, integrated
> into a turn angle and mapped to a reveal radius. A tap resets to the idle core.

Target: VIEWE Smart Ring Plus, ESP32-S3, 1.8" 360×360 ST77916 round panel,
LVGL **9.4.0**, BSP `viewesmart/smartring_plus`. Implementation:
`examples/glyph-anime/glyph_anime.c`.

---

## 1. Observed behavior (from the reference stills)

The pattern is defined by three stills — three samples of **one continuous
growth sequence**, not three discrete states:

| Still | File | Coverage | Description |
|-------|------|----------|-------------|
| 1 | `~/Pictures/screenshot-2026-10-30_20-37-11.png` | ~3.9 % of panel disc | small lit cluster near centre |
| 2 | `~/Pictures/screenshot-2026-10-30_20-37-23.png` | ~5.7 % of panel disc | mid-size lit region |
| 3 | `~/Pictures/screenshot-2026-10-30_20-37-34.png` | ~17.5 % of panel disc | most of the panel lit |

Turning the ring clockwise grows the pattern; counter-clockwise shrinks it. The
growth runs all the way until the grid fills the whole round screen.

### 1.1 The grid — axis-aligned square, fixed cells

The pattern is a **regular square grid** of glyph cells:

```c
int x = RING_CX + (i - half) * GRID_STEP;   /* i over grid rows */
int y = RING_CY + (j - half) * GRID_STEP;   /* j over grid cols */
```

There is **no 45° lattice, no checkerboard parity** — every `(i, j)` cell is
used. The cells are a **fixed size** and their **positions never change** — only
whether each cell is lit changes.

> **Why the stills look 45°-rotated.** Because the glyphs are fixed to the
> panel, physically rotating the ring rotates the *whole device* in the camera
> frame — the pattern is tilted in the photo, but it is **not** rotated relative
> to the panel. The on-screen grid is a plain square grid; the 45° appearance is
> the ring's physical orientation at the moment the photo was taken. (The same
> reason the circular bezel reads as an ellipse in the stills.)

### 1.2 Growth sequence — a centre-out reveal

Frame 1 → 2 → 3 shows **more and more cells lighting up** from the centre
outward. The lit region is a growing blob that starts as a tiny central cluster
and expands until the cells near the bezel light up and the grid fills the round
screen. Shrinking is the exact reverse: cells un-light **outside-in**.

Because the glyphs are fixed to the panel, the reveal grows **from the centre
outward** — the centre stays lit and lit cells appear at the growing edge.

- **Centre lights first, stays lit.** A compact core of glyphs near the centre
  is the "seed". In frame 1 it is nearly all you see.
- **Lit set grows outward.** Single glyphs light up progressively toward the
  bezel; the region between the lit edge and the bezel is dark.

### 1.3 Palette — green + warm yellow, multiple brightness levels

The palette is **green and warm yellow** (amber/gold), each at **several degrees
of brightness** — the same hue at different luminance, not a single flat colour.

| Class | Intent |
|-------|--------|
| warm yellow | amber / gold accent, hue ~35–45°, several brightness tiers |
| green | the fixed `RING_COLOR_GREEN`-family green, several brightness tiers |
| background | black (`RING_COLOR_BG`) |

Warm yellow dominates the stills (~84–95 % of glyph pixels); green is the
minority colour. The **brightness variation** is the key visual texture: a cell
is not simply on/off, it is one of N brightness levels (dim / mid / full), so
the lit region has a soft, uneven luminance rather than a hard edge.

### 1.4 Glyph shapes

Visible shapes: **filled squares, rotated squares (diamonds), 4/5-point
stars/asterisks, filled dots, plus/cross, short bars**. This app uses ~6
distinct shapes (`+`, `×`, filled square, dot, triangle, bars) from the
Montserrat `LV_SYMBOL_*` set — a compatible repertoire.

---

## 2. Interaction contract

| Input | Effect |
|-------|--------|
| Ring **clockwise** | Reveal radius grows → more cells light up, center-out, until the panel is full |
| Ring **counter-clockwise** | Reveal radius shrinks → cells un-light outside-in, back to the idle core |
| Ring **held still** | Reveal holds (position control — it stays where you leave it) |
| **Tap** | Reset the reveal to the small idle core (`s_turn_deg = 0`) |

- **Fixed grid:** 169 glyphs on a 13×13 square grid; cells never move, never
  scale, never rotate with the ring.
- **Reveal, not scale:** only each cell's lit/unlit state changes.
- **IMU-driven:** the turn is detected from `ring_imu_gyro()` gyro **Z** — the
  axis through the finger, perpendicular to the screen. No touch is needed to
  grow/shrink.

---

## 3. Architecture

```
examples/glyph-anime/
├── CMakeLists.txt              # fragment: RING_EXAMPLE_SRCS etc. (see examples/)
├── include/ring_example.h      # esp_err_t ring_example_start(void);
├── glyph_anime.c               # fixed grid + IMU turn -> reveal repaint
└── README.md                   # this design & build spec
components/ringkit/             # skeleton: board, LVGL lock, theme, RNG, IMU
```

- No new BSP code. Everything uses `lv_*` under `RING_UI_LOCKED(-1)` and
  `ring_imu_gyro()`.
- `glyph_anime.c` owns the whole UI; `main.c` only calls `ring_example_start()`.
- One `lv_obj` (screen). All glyphs are `lv_label` children of the screen placed
  on a **fixed grid**, built once and never re-laid-out.

### Data flow

```
lv_timer (20 ms) ── ring_imu_gyro(&gx,&gy,&gz)
                     │  gz = turn rate about the finger axis (deg/s)
                     ▼
               s_turn_deg += TURN_SIGN * gz * dt     (deadbanded)
                     ▼
               reveal = REVEAL_IDLE + (s_turn_deg / DEG_PER_REVEAL) * 100
                     ▼   (clamped to [REVEAL_IDLE, REVEAL_MAX])
               repaint(reveal)  → per-cell lit/unlit colour
```

No `lv_anim` for the reveal — rotation is a **position control**, so the radius
is computed directly from the IMU integral and repainted when it moves by
>0.5 px.

---

## 4. Layout (fixed grid, 360×360)

Panel center = `(RING_CX, RING_CY)` = (180, 180). Glyphs sit on a square grid;
nothing moves.

| Param | Value | Note |
|-------|-------|------|
| `GRID_N` | 13 | 13×13 = **169** glyphs |
| `GRID_STEP` | 27 px | extent = 13·27 = 351 px → ~4.5 px margin each side |
| `REVEAL_IDLE` | 40.5 px | `1.5 · GRID_STEP` — the small idle core |
| `REVEAL_MAX` | 180 px | `RING_RADIUS` — lights every cell on the round panel |
| `DEG_PER_REVEAL` | 90 | degrees of ring turn per 100 px of reveal |
| `GYRO_DEADBAND_DPS` | 1.5 | deg/s; ignore below this (kills offset drift) |
| `EDGE_FADE` | 27 px | `GRID_STEP` — soft reveal edge blend |
| `POLL_MS` | 20 | IMU poll cadence (50 Hz) |

Glyph position for cell (i, j), i,j ∈ [0, 12] — **fixed at build time**:

```c
int half = (GRID_N - 1) / 2;                    /* 6 */
int x = RING_CX + (i - half) * GRID_STEP;
int y = RING_CY + (j - half) * GRID_STEP;
```

Distance from center (the reveal key) is computed once:

```c
uint16_t dist = (uint16_t)roundf(hypotf(x - RING_CX, y - RING_CY));
```

### Turn → reveal mapping

```c
#define DEG_PER_REVEAL 90.0f     /* deg per 100 px */
#define REVEAL_IDLE   (1.5f * GRID_STEP)   /* 40.5 px */
#define REVEAL_MAX    ((float)RING_RADIUS) /* 180 px  */
#define GYRO_DEADBAND_DPS 1.5f
#define TURN_SIGN 1.0f           /* +1: clockwise = +gz grows the reveal */

/* in the 20 ms poll */
float gx, gy, gz;
ring_imu_gyro(&gx, &gy, &gz);
if (fabsf(gz) > GYRO_DEADBAND_DPS)
    s_turn_deg += TURN_SIGN * gz * dt;

float reveal = REVEAL_IDLE + (s_turn_deg / DEG_PER_REVEAL) * 100.0f;
if (reveal < REVEAL_IDLE) reveal = REVEAL_IDLE;
if (reveal > REVEAL_MAX)  reveal = REVEAL_MAX;
```

---

## 5. Icon (glyph) model

```c
typedef enum { GLYPH_GREEN, GLYPH_YELLOW } glyph_class_t;

typedef struct {
    const char *symbol;
    glyph_class_t cls;
    uint8_t bright;    /* brightness tier 0..N_BRIGHTNESS-1 */
    int16_t x, y;      /* cell position (px) — FIXED        */
    uint16_t dist;     /* distance from center (px)         */
    lv_obj_t *obj;
} glyph_t;

static glyph_t s_glyphs[GRID_N * GRID_N];   /* 169 entries */
```

- **~6 distinct glyph shapes** from a small `LV_SYMBOL_*` set (`+`, `×`,
  square, dot, triangle, bars), picked pseudo-randomly per cell
  (`s_syms[ring_rand_below(N_SYMS)]`), not cycled — organic field.
- **Green / warm-yellow split is random** (`ring_rand_bool()`), no ring bands.
- **Each cell gets a random brightness tier** (`ring_rand_below(3)`):
  dim / mid / full (`s_bright = {0.35, 0.65, 1.0}`). This gives the uneven
  luminance seen in the stills.
- Full-lit color = class color **mixed toward black by `(1 - frac)`**, so only
  luminance changes, hue is preserved:

  ```c
  lv_color_t base = (g->cls == GLYPH_GREEN) ? RING_COLOR_GREEN
                                            : RING_COLOR_YELLOW; /* 0xFFB74D */
  return lv_color_mix(base, RING_COLOR_BG, (uint8_t)((1.0f - frac) * 255.0f));
  ```

---

## 6. Reveal repaint (cells fixed, only colour changes)

```c
static void repaint(float reveal)
{
    for (int i = 0; i < N_GLYPHS; i++) {
        const glyph_t *g = &s_glyphs[i];
        lv_color_t c;

        if ((float)g->dist > reveal) {
            c = RING_COLOR_BG;                        /* outside the reveal */
        } else {
            lv_color_t base = base_color(g);
            float edge = (float)g->dist - (reveal - EDGE_FADE);
            if (edge > 0.0f) {
                float f = 1.0f - edge / EDGE_FADE;    /* 0..1 blend */
                c = lv_color_mix(base, RING_COLOR_BG,
                                 (uint8_t)((1.0f - f) * 255.0f));
            } else {
                c = base;                             /* fully lit */
            }
        }
        lv_obj_set_style_text_color(g->obj, c, LV_PART_MAIN);
    }
}
```

- A cell is lit once `dist <= reveal`; near the edge it blends from black over
  `EDGE_FADE` (one cell step), so the lit region has a **soft, uneven boundary**
  rather than a hard circle.
- Cells never move — only `lv_obj_set_style_text_color` is called. No scale, no
  position, no transform.

---

## 7. IMU turn detection

The ring is worn on a finger; turning it clockwise/counter-clockwise is a
rotation about the axis through the finger — the board **Z** axis, perpendicular
to the screen. `ring_imu_gyro()` returns the BSP's 250 Hz EMA-filtered gyro
(deg/s), so `gz` is already smooth.

```c
static void poll_cb(lv_timer_t *timer)
{
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_us) / 1e6f;
    s_last_us = now;

    float gx, gy, gz;
    ring_imu_gyro(&gx, &gy, &gz);
    if (fabsf(gz) > GYRO_DEADBAND_DPS)
        s_turn_deg += TURN_SIGN * gz * dt;

    float reveal = REVEAL_IDLE + (s_turn_deg / DEG_PER_REVEAL) * 100.0f;
    if (reveal < REVEAL_IDLE) reveal = REVEAL_IDLE;
    if (reveal > REVEAL_MAX)  reveal = REVEAL_MAX;

    if (fabsf(reveal - s_reveal) > 0.5f) {
        s_reveal = reveal;
        RING_UI_LOCKED(-1) { repaint(reveal); }
    }
}
```

> **`TURN_SIGN` must match the board.** It is `+1.0` here (a clockwise turn
> gives `+gz` and grows the reveal on this ring). If a unit grows on
> counter-clockwise instead, flip `TURN_SIGN` to `-1.0`. Verify by rotating
> ~90° clockwise and watching the telemetry `turn` value increase and the panel
> fill.

---

## 8. Input — tap resets

`esp_lv_adapter` registers the CST816 touch indev. Only one gesture is used:

- **Tap (`LV_EVENT_CLICKED`)** → `s_turn_deg = 0`, `s_reveal = REVEAL_IDLE`,
  repaint. An explicit way back to the idle core.

Growth/shrink needs no touch — it is IMU-driven.

---

## 9. Files

1. **`examples/glyph-anime/glyph_anime.c`** — the whole implementation.
2. **`examples/glyph-anime/include/ring_example.h`** — `ring_example_start()` /
   `ring_example_name()`.
3. **`examples/glyph-anime/CMakeLists.txt`** — fragment listing `RING_EXAMPLE_SRCS`.
4. **`main/main.c`** — app-agnostic; calls `ring_board_init()` then
   `ring_example_start()`.
5. **`sdkconfig.defaults`** — fonts (already set): `CONFIG_LV_FONT_MONTSERRAT_20=y`.
6. **`components/ringkit/`** — `ring_imu_gyro()`, `ring_ui_add_label()`,
   `ring_ui_place_centered()`, `ring_rand_*`, `RING_UI_LOCKED`.

> Remember: `sdkconfig.defaults` is only applied when `sdkconfig` is
> (re)created → run `rm -f sdkconfig && idf.py reconfigure` after editing it.

---

## 10. Build & verify

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # watch TAG "glyph-anime"
```

Expected log:

```
glyph-anime: ready: 169 glyphs, 13 cells, reveal 40..180 px, 90 deg/100px, turn sign +1.0
glyph-anime: turn 0 deg, reveal 40 px (40..180)       # at rest (idle core)
glyph-anime: turn 13 deg, reveal 55 px (40..180)      # after a clockwise turn
glyph-anime: turn -95 deg, reveal 40 px (40..180)     # counter-clockwise → idle
```

Visual check: rotate the ring clockwise → more glyphs light up from the center
outward until the panel is full (cells never move or scale, green + warm yellow
at varied brightness); counter-clockwise → they un-light outside-in; hold → it
stays; tap → reset to the small idle core.

---

## 11. Open questions / to verify on device

1. **Sign of `gz` for clockwise.** Currently `TURN_SIGN = +1.0` (verified on the
   dev unit: clockwise grows the reveal). Confirm on each new unit.
2. **Brightness-tier model** (§1.3): is a cell's brightness distance-based
   (brighter core / dimmer rim), random per cell (current implementation), or
   cycling? All three read as "varying brightness" in a still — pick on
   hardware.
3. **Gyro offset drift** over minutes — the deadband mitigates it; add a slow
   leak of `s_turn_deg` toward 0 when `|gz|` is below the deadband if it creeps.
4. **Reveal edge softness.** `EDGE_FADE = GRID_STEP` blends the edge; tune it if
   the boundary should be harder/softer.

---

## 12. Risks / notes

- **No transform scaling** — cells never move; only text colours change, so
  LVGL pivot/scale quirks are avoided entirely.
- **`TURN_SIGN` is board-specific.** Confirm clockwise grows the reveal on the
  actual unit; flip if not.
- **Gyro drift:** the `GYRO_DEADBAND_DPS` deadband stops offset drift while the
  ring is still. If the reveal still creeps over minutes, add a slow leak of
  `s_turn_deg` toward 0 when `|gz|` is below the deadband.
- **Reveal cost:** up to 169 `lv_obj_set_style_text_color` calls per change.
  Repaint only fires when the reveal moves >0.5 px, so at rest it's zero work.
- **All LVGL calls inside `RING_UI_LOCKED(-1) { ... }`.** The `lv_timer`
  callback runs on the LVGL task (already inside the adapter lock), so the
  scoped lock is re-entrant-safe; `ring_imu_gyro()` takes the BSP mutex
  independently.
- **Square-grid layers, not circles** — the lit set is quantized to the grid, so
  the edge is stepped, not a smooth circle. That is the intended look; the
  `EDGE_FADE` blend softens it.
- **`lv_color_mix(c1, c2, mix)`** (LVGL 9.4): `mix=255 → c1`, `0 → c2`; no extra
  component.
- **Symbol fonts:** the `LV_SYMBOL_*` glyphs (0xF000+) ship inside the built-in
  Montserrat fonts — `CONFIG_LV_FONT_MONTSERRAT_20` is already on.
