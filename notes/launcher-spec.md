# Circular App-Grid Launcher — Design & Build Spec

Replicates the interaction seen in `@KINDOFDEVICE_`'s Smart Ring Plus demo:
a **circular (radial) app-grid launcher** that **zooms open from the screen
center**, **color-cycles its accent icons**, and **collapses back** to a tiny
idle cluster.

Target: VIEWE Smart Ring Plus, ESP32-S3, 1.8" 360×360 ST77916 round panel,
LVGL **9.4.0**, BSP `viewesmart/smartring_plus`.

---

## 1. Observed behavior (from video)

| Phase | Visual |
|-------|--------|
| Idle | Screen dark; a tiny cluster of ~a dozen glyphs at center (~12 % scale) |
| Open | Grid **zooms out from center** (scale 0.12 → 1.0), ~0.4–0.5 s, ease-out |
| Open (held) | Dense circular grid of glyph icons fills the panel. A fixed ring of **green** icons stays constant; **accent icons cycle hue**: orange → pink → blue → cream |
| Close | Grid **zooms back to center** (scale 1.0 → 0.12), ~0.3–0.4 s |

---

## 2. Architecture

```
main/
├── main.c          # BSP init sequence, then launcher_start()
├── launcher.h      # public API: esp_err_t launcher_start(void);
└── launcher.c      # state machine + icon table + layout + anims + input
```

- No new BSP code. Everything uses `lv_*` under `esp_lv_adapter_lock()`.
- `launcher.c` owns the whole UI; `main.c` only calls `launcher_start()`.
- One `lv_obj` (screen) + one transparent container `grid` that holds all
  icons as children. Scaling the **container** (not each icon) gives the
  zoom open/close for free.

### State machine

```
IDLE ──tap──▶ OPENING ──(scale anim done)──▶ OPEN
  ▲                                            │
  │                                            │ tap
  └───────CLOSING ◀──(scale anim done)─────────┘
```

States: `LAUNCHER_IDLE`, `LAUNCHER_OPENING`, `LAUNCHER_OPEN`, `LAUNCHER_CLOSING`.

---

## 3. Layout (radial, 360×360)

Panel radius = 180. Use `MAX_R = 158` (leave a breathing margin).

| Ring | Radius (px) | Icon count | Angle step |
|------|-------------|-----------|------------|
| 0 (center) | 0 | 1 | — |
| 1 | 48 | 6 | 60° |
| 2 | 96 | 12 | 30° |
| 3 | 140 | 18 | 20° |
| **Total** | | **37** | |

Icon position:

```c
float a = 2.0f * M_PI * i / n - M_PI_2;      // start at 12 o'clock
int x = (int)roundf(cx + r * cosf(a));
int y = (int)roundf(cy + r * sinf(a));
```

Spacing check: ring 3 circumference = 2π·140 ≈ 880 px / 18 ≈ 49 px per icon
(no overlap at 14–18 px glyphs). Ring 1: 2π·48 ≈ 302 / 6 ≈ 50 px.

---

## 4. Icon model

```c
typedef enum { ICON_STATIC_GREEN, ICON_ACCENT } icon_class_t;

typedef struct {
    const char *symbol;     // LV_SYMBOL_*
    icon_class_t cls;
} launcher_icon_t;

static const launcher_icon_t s_icons[37] = {
    /* ring 0 */
    { LV_SYMBOL_HOME,     ICON_STATIC_GREEN },
    /* ring 1 */
    { LV_SYMBOL_SETTINGS, ICON_STATIC_GREEN },
    { LV_SYMBOL_AUDIO,    ICON_ACCENT },
    { LV_SYMBOL_WIFI,     ICON_STATIC_GREEN },
    { LV_SYMBOL_IMAGE,    ICON_ACCENT },
    { LV_SYMBOL_BLUETOOTH,ICON_STATIC_GREEN },
    { LV_SYMBOL_PLAY,     ICON_ACCENT },
    /* ... fill rings 2 and 3 from the LV_SYMBOL_ table ... */
};
```

- `ICON_STATIC_GREEN` → fixed `lv_color_hex(0x00E676)` (green).
- `ICON_ACCENT` → colored each frame from the cycling hue (see §5).
- Icons are `lv_label` children of `grid` (symbols render with the built-in
  Montserrat fonts — symbol glyphs are baked in, no extra font needed).

---

## 5. Accent color cycle

A single `lv_anim` on a `float hue` (0–360) runs while the launcher is OPEN.
Each frame the exec callback paints every `ICON_ACCENT` label:

```c
static void color_cb(void *var, int32_t v)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)v, 90, 95);
    /* apply c to each ICON_ACCENT label's text color */
}
/* lv_anim: 0 -> 360 over ~5000 ms, LV_ANIM_REPEAT_INFINITE, linear */
```

The video's palette (orange→pink→blue→cream) is approximately HSV hue
**30 → 320 → 230 → 45**, so a full 360° sweep reads as the same effect. The
fixed green ring provides contrast exactly like the footage.

---

## 6. Open / close zoom

Animate the `grid` container's transform scale around its center:

```c
lv_obj_set_style_transform_pivot(grid, 50, 50, 0);   // percent, center pivot
lv_obj_set_style_transform_scale_x(grid, scale, 0);
lv_obj_set_style_transform_scale_y(grid, scale, 0);
```

| Transition | Scale | Duration | Ease |
|-----------|-------|----------|------|
| Open | 0.12 → 1.00 | 450 ms | `LV_EASE_OUT_QUART` |
| Close | 1.00 → 0.12 | 350 ms | `LV_EASE_IN_QUART` |

On open-complete → state `OPEN`, start color anim.
On close-complete → state `IDLE`, stop/delete color anim, reset scale.

Use `lv_anim_set_ready_cb` to transition the state machine at the end of each
zoom. The idle cluster IS the same container at scale 0.12 — no separate
widget needed.

---

## 7. Input

`esp_lv_adapter` already registers the CST816 touch indev, so use standard
LVGL events on the screen / grid:

- **Tap (LV_EVENT_CLICKED)** while `IDLE` → `launcher_open()`.
- **Tap (LV_EVENT_CLICKED)** while `OPEN` → `launcher_close()`.
- **Swipe (LV_EVENT_GESTURE)** while `OPEN` → jump the accent hue to the next
  palette stop (orange→pink→blue→cream), i.e. page-forward the color cycle.
  *(Nice-to-have; optional in v1.)*

Register on the **screen** so taps anywhere toggle, not just on icons.
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

Visual check: tap → grid zooms out, green ring fixed, accents cycle
orange→pink→blue→cream; tap again → zooms back to the tiny cluster.

---

## 10. Risks / notes

- **Console back-pressure (the big one).** The board's console is the USB-Serial-JTAG
  port, and `ESP_LOG*` **blocks** when the host is not draining it. During bring-up
  this made the firmware look like it hung in `bsp_sd_init()`, `bsp_power_init()` and
  the first LVGL render. It never did — the capture script slept before reading.
  Always drain `/dev/ttyACM0` continuously (background thread or `idf.py monitor`).
- **Transform scaling** on the container: LVGL scales around the pivot; `transform_pivot`
  is set to `50,50` (percent) so it zooms about the screen center. With
  `CONFIG_LV_DRAW_LAYER_MAX_MEMORY=0` a transformed object gets a full-size layer buffer,
  but measured cost is fine (first full render ≈158 ms).
- **PSRAM flush limit:** do NOT enlarge the BSP's 40-line draw buffer
  (full-panel buffering exhausts internal RAM → `ESP_ERR_NO_MEM`). Scaling a
  360×360 container is fine — LVGL clips/scales in software on the same
  buffer.
- **All LVGL calls inside `esp_lv_adapter_lock(-1)` / `unlock()`.** The
  `lv_anim` exec/ready callbacks run on the LVGL task, so they're already
  locked; user-input callbacks are dispatched by the adapter's task too.
- **`lv_color_hsv_to_rgb`** is available in LVGL 9.4; no extra component.
- **Symbol fonts:** the `LV_SYMBOL_*` glyphs (0xF000+) ship inside the
  built-in Montserrat fonts — `CONFIG_LV_FONT_MONTSERRAT_20=y` is what the
  icons use. Note the built-in fonts do **not** carry every `LV_SYMBOL_*`
  glyph, but LVGL simply renders a placeholder box for a missing one; it does
  not hang.
- **Transient `axp2101: status1 failed: ESP_ERR_TIMEOUT`** can appear while the
  launcher animates (I2C contention with the battery poll task). Harmless and
  self-recovering.

---

## 10b. Built and verified

Implemented as specified and confirmed on hardware (VIEWE Smart Ring Plus):

```
I (2179) launcher: ready (idle): 37 icons, 18 accent
I (2180) smartring: Launcher is on screen.
```

Interactive path verified with synthesized taps:

```
synthetic tap 1 -> launcher: opening... -> (490 ms) -> launcher: opened
synthetic tap 2 -> launcher: closing... -> (431 ms) -> launcher: closed -> idle
```

Full BSP init sequence runs cleanly alongside it (SD 476 MB APPSD mounted,
QMI8658A present, ES8311 audio up, AXP2101 at 4.15 V / 100 % / charging).

---

## 11. Reference implementation skeleton (`main/launcher.c`)

```c
#include "launcher.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>

#define TAG "launcher"

#define CX   180
#define CY   180
#define MAX_R 158

#define ICON_FONT  (&lv_font_montserrat_20)
#define COLOR_GREEN lv_color_hex(0x00E676)
#define SCALE_IDLE  0.12f

typedef enum { ICON_STATIC_GREEN, ICON_ACCENT } icon_class_t;

typedef struct {
    const char *symbol;
    icon_class_t cls;
} launcher_icon_t;

static const launcher_icon_t s_icons[] = {
    { LV_SYMBOL_HOME,       ICON_STATIC_GREEN },
    { LV_SYMBOL_SETTINGS,   ICON_STATIC_GREEN },
    { LV_SYMBOL_AUDIO,      ICON_ACCENT },
    { LV_SYMBOL_WIFI,       ICON_STATIC_GREEN },
    { LV_SYMBOL_IMAGE,      ICON_ACCENT },
    { LV_SYMBOL_BLUETOOTH,  ICON_STATIC_GREEN },
    { LV_SYMBOL_PLAY,       ICON_ACCENT },
    { LV_SYMBOL_NEXT,       ICON_STATIC_GREEN },
    { LV_SYMBOL_PREV,       ICON_ACCENT },
    { LV_SYMBOL_PAUSE,      ICON_STATIC_GREEN },
    { LV_SYMBOL_STOP,       ICON_ACCENT },
    { LV_SYMBOL_BELL,       ICON_STATIC_GREEN },
    { LV_SYMBOL_GPS,        ICON_ACCENT },
    { LV_SYMBOL_CHARGE,     ICON_STATIC_GREEN },
    { LV_SYMBOL_BATTERY_FULL, ICON_ACCENT },
    { LV_SYMBOL_WARNING,    ICON_STATIC_GREEN },
    { LV_SYMBOL_CALL,       ICON_ACCENT },
    /* ... up to 37 entries (rings 0..3) ... */
};
#define N_ICONS (sizeof(s_icons)/sizeof(s_icons[0]))

typedef enum { L_IDLE, L_OPENING, L_OPEN, L_CLOSING } launcher_state_t;

static lv_obj_t *s_grid;
static lv_obj_t *s_accent[16];   /* ICON_ACCENT labels, for the color anim */
static int s_n_accent;
static launcher_state_t s_state = L_IDLE;

static void set_scale(float s)
{
    lv_obj_set_style_transform_scale_x(s_grid, (lv_style_value_t){ .num = (int32_t)(s * 256) }, 0);
    lv_obj_set_style_transform_scale_y(s_grid, (lv_style_value_t){ .num = (int32_t)(s * 256) }, 0);
}

static void color_cb(void *var, int32_t hue)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue, 90, 95);
    for (int i = 0; i < s_n_accent; i++)
        lv_obj_set_style_text_color(s_accent[i], c, LV_PART_MAIN);
}

static void zoom_done(lv_anim_t *a) { /* state transitions */ }

static void zoom(float from, float to, uint32_t dur, lv_anim_exec_xcb_t cb, lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_grid);
    lv_anim_set_values(&a, (int32_t)(from * 256), (int32_t)(to * 256));
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

/* ... launcher_open(), launcher_close(), input event handler ... */

esp_err_t launcher_start(void)
{
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_grid = lv_obj_create(scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, 360, 360);
    lv_obj_center(s_grid);
    lv_obj_set_style_transform_pivot(s_grid, 50, 50, 0);

    /* place rings 0..3, create labels, classify accents */
    /* ... */

    set_scale(SCALE_IDLE);
    lv_obj_add_event_cb(scr, launcher_input_cb, LV_EVENT_CLICKED, NULL);

    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "launcher ready (idle)");
    return ESP_OK;
}
```

> The skeleton is illustrative — the full version wires the ring layout,
> the accent array, and the state machine transitions. Build first, then
> flesh out `launcher.c`.
