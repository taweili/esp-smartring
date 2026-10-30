# Restructure proposal — generic skeleton + `examples/`

Status: **IMPLEMENTED** (see §10 for the two deviations from this plan, and why).

Goal: split the repo into

1. a **generic skeleton** (`components/ringkit/`) that knows how to bring up the
   board and host an app on the 360×360 panel, and
2. a set of **interchangeable application examples** (`examples/<name>/`), of
   which exactly one is compiled into a given firmware image.

Today everything is one app: `main/main.c` does BSP bring-up *and* app selection,
and `main/launcher.c` is the only UI. Nothing is reusable, and the launcher's
RNG / lock handling / theme constants are trapped inside it.

---

## 1. Why this is worth doing

Concrete duplication and coupling in the current tree:

| Problem today | Where |
|---|---|
| BSP bring-up order (display → battery → power → sd → imu → audio) is inline in `app_main`, untested and un-reusable | `main/main.c` |
| Every new UI must re-derive the lock discipline (`esp_lv_adapter_lock(-1)` … `unlock()`), and early `return` leaks the lock | `main/launcher.c` |
| Theme values (green `0x00E676`, black bg, HSV 90/95, panel center, `GRID_STEP`) are private to the launcher | `main/launcher.c` |
| The xorshift32 RNG — including the documented LCG low-bit trap — lives in the launcher | `main/launcher.c` |
| Adding a second UI means editing `main.c`, `main/CMakeLists.txt`, and fighting `launcher.h` | — |

After the split, adding a new app is **one directory** and picks up board
bring-up, locking, theme and RNG for free.

---

## 2. Target tree

```
.
├── CMakeLists.txt                  # selects exactly one example, then project()
├── sdkconfig.defaults              # unchanged (fonts, FREERTOS_HZ, PSRAM, codec)
├── components/
│   ├── espressif__esp_codec_dev/   # vendored override — UNCHANGED, must stay here
│   └── ringkit/                    # ← the generic skeleton
│       ├── CMakeLists.txt
│       ├── include/ringkit/
│       │   ├── ring_board.h        # board bring-up + peripherals summary
│       │   ├── ring_ui.h           # LVGL lock + screen helpers
│       │   ├── ring_theme.h        # shared colours / geometry / timing
│       │   ├── ring_rand.h         # deterministic xorshift32
│       │   └── ring_app.h          # app descriptor + registry (tier 2)
│       └── src/
│           ├── ring_board.c
│           ├── ring_ui.c
│           ├── ring_rand.c
│           └── ring_app.c
├── main/                           # thin, app-agnostic entry point
│   ├── CMakeLists.txt              # REQUIRES ringkit ring_example
│   ├── idf_component.yml           # single dependency manifest (unchanged)
│   └── main.c                      # board init → ring_example_start()
├── examples/
│   ├── README.md                   # how to add / build an example
│   ├── launcher/                   # ← the current glyph-grid launcher (moved)
│   │   ├── CMakeLists.txt          # registers component `ring_example`
│   │   ├── include/ring_example.h
│   │   └── launcher.c
│   ├── hello/                      # original Montserrat 48 label (bring-up smoke test)
│   ├── clock/                      # round watch face (panel geometry demo)
│   └── dashboard/                  # battery + IMU readout (peripheral demo)
└── notes/
    ├── esp-smartring.md            # unchanged
    ├── launcher-spec.md            # unchanged (launcher-specific)
    └── restructure-proposal.md     # this file
```

---

## 3. How exactly one example is selected

> **Superseded by §10.1** — the `EXTRA_COMPONENT_DIRS` mechanism described below
> is silently broken in ESP-IDF. Read §10.1 for what was actually implemented.

**Constraint (verified):** the project `CMakeLists.txt` is evaluated before
`sdkconfig` is included — `project.cmake:447` does `include(${sdkconfig_cmake})`
*inside* `idf_build_process`, which is called by `project()`. So `CONFIG_*`
variables do **not** exist yet when `EXTRA_COMPONENT_DIRS` must be set.
A **CMake cache variable** is therefore the only viable selector at that point.

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

set(RING_APP "launcher" CACHE STRING "Example app under examples/ to build")

set(_app_dir "${CMAKE_CURRENT_LIST_DIR}/examples/${RING_APP}")
if(NOT EXISTS "${_app_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "RING_APP='${RING_APP}' has no examples/${RING_APP}/CMakeLists.txt. "
        "Available: ...")
endif()

# `components/` (vendored codec override, ringkit) is still discovered
# automatically; this only ADDS the selected example.
set(EXTRA_COMPONENT_DIRS "${_app_dir}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartring)
```

Build a specific app:

```bash
idf.py -DRING_APP=clock      -B build/clock      build
idf.py -DRING_APP=clock      -B build/clock -p /dev/ttyACM0 flash
```

`idf.py build` with no flags keeps building `launcher`, so the commands
documented in `AGENTS.md` stay valid.

> **Always use a per-app build directory (`-B build/<name>`).** Switching
> `RING_APP` in place leaves stale objects/`sdkconfig` state and produces
> confusing link errors. Document this in `examples/README.md`.

### Why every example registers as the *same* component name

Each example's `CMakeLists.txt` declares component **`ring_example`**:

```cmake
idf_component_register(
    SRCS "launcher.c"
    INCLUDE_DIRS "include"
    REQUIRES ringkit
)
```

Because only `examples/${RING_APP}` is on the component search path, the name
can be identical everywhere. That makes `main/` **completely app-agnostic** —
it always requires `ring_example` and always calls the same two functions, so
adding an example never touches `main/`.

*(Alternative, rejected: Kconfig `choice` + all example dirs in
`EXTRA_COMPONENT_DIRS` with conditionally-empty `idf_component_register()`
calls. It works — `CONFIG_*` **is** available in component CMakeLists — and
lets `menuconfig` pick the app, but it compiles the selection into `sdkconfig`,
which is harder to script than a cache variable and invites the "I flashed the
wrong app" mistake.)*

---

## 4. The skeleton API

### 4.1 `ring_board.h` — board bring-up

Extracted verbatim from today's `app_main`, so the *order* becomes documented
code instead of a comment:

```c
typedef struct {
    bool sd_ok;        /* optional — no card-detect pin, may legitimately fail */
    bool imu_ok;       /* optional                                            */
    bool audio_ok;
    bool battery_v2;   /* AXP2101 vs GPIO1 ADC                                 */
} ring_board_t;

/* Full init sequence in the required order (display first: it creates the
 * shared I2C bus and the LVGL task). Mandatory steps are ESP_ERROR_CHECK'd;
 * SD/IMU/audio fail soft and are reported via the summary. */
esp_err_t ring_board_init(ring_board_t *out);
void      ring_board_log_summary(const ring_board_t *b);

/* Convenience passthroughs so examples don't include the BSP header. */
esp_err_t ring_board_backlight(uint8_t pct);
esp_err_t ring_board_shutdown(void);   /* soft power-off, normally doesn't return */
```

Anything a *particular* app needs beyond this (IMU samples, audio streaming)
still goes straight to the BSP — `ringkit` deliberately does not wrap the whole
BSP API.

### 4.2 `ring_ui.h` — locking done safely

The lock discipline is the single easiest thing to get wrong. Replace manual
lock/unlock pairs with something that cannot leak on early return:

```c
esp_err_t ring_ui_lock(void);      /* esp_lv_adapter_lock(-1)  */
void      ring_ui_unlock(void);

/* Scoped lock: unlocks on every exit path, including `return` and `break`. */
#define RING_UI_LOCKED(for_) \
    for (int _ring_lock = ring_ui_lock() == ESP_OK ? 1 : 0; \
         _ring_lock; _ring_lock = (ring_ui_unlock(), 0))

lv_obj_t *ring_ui_screen(void);                 /* active screen, filled black */
lv_obj_t *ring_ui_add_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, lv_color_t color);
/* Centres a label on (x, y) using LVGL's (w-1)/2 area centre.
 * Two-pass layout is handled internally — see the gotcha in
 * notes/launcher-spec.md §"Build". */
void      ring_ui_place_centered(lv_obj_t *obj, int x, int y);
```

Usage in an example:

```c
esp_err_t my_app_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();
        ring_ui_add_label(scr, "hello", &lv_font_montserrat_48, lv_color_white());
    }
    return ESP_OK;
}
```

### 4.3 `ring_theme.h` — shared look

Everything colour/geometry that is currently hard-coded in `launcher.c`:

```c
#define RING_PANEL_W     360
#define RING_PANEL_H     360
#define RING_CX          180
#define RING_CY          180

#define RING_COLOR_BG    lv_color_black()
#define RING_COLOR_GREEN lv_color_hex(0x00E676)   /* fixed accent green  */

#define RING_HUE_SAT     90
#define RING_HUE_VAL     95

/* Motion tuning, shared so apps feel consistent. */
#define RING_MS_REVEAL_OPEN   450
#define RING_MS_REVEAL_CLOSE  350
```

### 4.4 `ring_rand.h` — deterministic RNG

Promotes the current xorshift32 (and its hard-won comment about LCG low-bit
degeneracy) out of the launcher:

```c
void     ring_rand_seed(uint32_t seed);
uint32_t ring_rand_u32(void);
uint32_t ring_rand_below(uint32_t n);   /* unbiased-enough modulo */
bool     ring_rand_bool(void);
```

### 4.5 `ring_app.h` — registry (tier 2, additive)

Design the descriptor now so the next step doesn't require another restructure,
but **do not wire it up in v1**:

```c
typedef struct ring_app {
    const char        *name;
    const char        *symbol;                  /* LV_SYMBOL_* grid icon  */
    esp_err_t        (*start)(void);            /* build UI (locks itself) */
    void             (*stop)(void);             /* tear down UI            */
    void             (*tick)(uint32_t dt_ms);   /* optional periodic work  */
} ring_app_t;

/* Filled once at boot; the enabled app registers itself. */
void                 ring_app_register(const ring_app_t *app);
const ring_app_t    *ring_app_current(void);
esp_err_t            ring_app_launch(const ring_app_t *app);
```

Tier 2 then becomes: the launcher enumerates `ring_app_*` and maps glyph taps
to `ring_app_launch()`, turning the demo into an actual app switcher. No further
file moves needed.

---

## 5. Examples

| Dir | Purpose | Exercises |
|---|---|---|
| `examples/launcher/` | The existing glyph-grid launcher, moved as-is (minus theme/RNG that moved into `ringkit`) | layered reveal, HSV cycle, touch |
| `examples/hello/` | The original centered `Montserrat 48` label | **bring-up smoke test** — if this shows, panel + touch + adapter are sane |
| `examples/clock/` | Round watch face: hands, ticks, battery arc | `ring_theme` geometry, `lv_scale`/`lv_line`, `ring_ui` centring |
| `examples/dashboard/` | Battery %, voltage, charging, IMU tilt | `bsp_battery_*`, `bsp_imu_*`, `tick` callback |

`hello` is deliberately kept: when a screen misbehaves it is the cheapest way
to rule out panel/adapter problems before blaming app code.

`examples/README.md` covers: choosing an app, per-app build dirs, the
`ring_example` contract (two exported symbols), and the dependency rule —
**do not add `idf_component.yml` per example**; managed dependencies stay in
`main/idf_component.yml`, which is always built.

---

## 6. Migration plan

Each step leaves a green build.

1. **`ringkit` scaffolding.** Add `components/ringkit/` with `ring_theme.h`,
   `ring_rand.{h,c}`, `ring_ui.{h,c}`, `ring_board.{h,c}`. No callers yet;
   build unchanged.
2. **Move the launcher.** `git mv main/launcher.{c,h} examples/launcher/`,
   add `examples/launcher/CMakeLists.txt` + `include/ring_example.h`, and
   replace the launcher's local copies of the green/black/HSV constants and
   RNG with `ringkit` includes. Drop `launcher.c` from `main/CMakeLists.txt`.
3. **Slim `main.c`.** Reduce to `ring_board_init()` → `ring_example_start()`,
   plus a summary log. Delete the inline BSP sequence (now in `ring_board.c`).
4. **Wire the selector.** Add `RING_APP` + `EXTRA_COMPONENT_DIRS` to the root
   `CMakeLists.txt`; `main/CMakeLists.txt` becomes
   `REQUIRES ringkit ring_example`. Verify `launcher` builds and flashes
   identically to today.
5. **Add `examples/hello/`.** Tests the whole selector path with a second app;
   verify both build dirs independently.
6. **Add `clock/`** (and optionally `dashboard/`). Pure addition.
7. **Docs.** Update `AGENTS.md` (architecture tree, commands, "adding an
   example"), and add `examples/README.md`.

---

## 7. What must NOT change

- `components/espressif__esp_codec_dev/` — the vendored override. It must stay
  in `components/` (project components outrank `managed_components/`). The
  `EXTRA_COMPONENT_DIRS` change is *additive* and does not affect it; re-verify
  with:
  ```bash
  grep -oE "(components|managed_components)/espressif__esp_codec_dev" \
      build/compile_commands.json | sort | uniq -c
  ```
- `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults` (IMU task `pdMS_TO_TICKS(4)`
  assert).
- The 40-line LVGL draw buffer (full-panel buffering exhausts internal RAM).
- Init order: `bsp_display_init()` first.
- `main/idf_component.yml` as the single dependency manifest.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| Stale build dir when switching `RING_APP` → weird link errors | Per-app `-B build/<name>`; documented; `idf.py fullclean` as fallback |
| `RING_APP` typo → cryptic CMake error | Explicit `EXISTS` check with `FATAL_ERROR` listing valid examples |
| Contributors add per-example `idf_component.yml` → duplicate managed deps | Called out in `examples/README.md` |
| `RING_UI_LOCKED` macro confuses debuggers/linters | Keep it a thin, one-line-macro wrapper; `ring_ui_lock/unlock` remain public for odd cases |
| Long app names break file-system paths on Windows | Restrict to `[a-z0-9-]+` |
| `ring_app` registry is speculative | Ship it as a header + trivial implementation in v1; the launcher stays a plain example until tier 2 |

---

## 9. Open questions

Answered:

1. **Selector** — cache variable `RING_APP` (default `glyph-anime`).
2. **Scope of `ringkit`** — wraps the **whole** BSP: board, display, touch,
   battery, power, audio, IMU, SD, plus UI/theme/RNG.
3. **Examples in the first pass** — `glyph-anime` (renamed from `launcher`) and
   `hello`. No `clock` / `dashboard` yet.
4. **Project name** — renamed to `smartring` (binary is now `smartring.bin`).
5. **App registry (`ring_app`)** — **not** implemented: single app per firmware,
   as requested. `ringkit` exposes no registry; `main.c` calls
   `ring_example_start()` directly.

---

## 10. Deviations from this plan (and why)

### 10.1 Examples are CMake fragments, not components

This plan said every example would be a component registered via
`EXTRA_COMPONENT_DIRS`, all named the same so `main` could always
`REQUIRES ring_example`. **That does not work in ESP-IDF.**

Two facts, both verified against this IDF (v6.1):

1. Component registration order puts `main` **before** `EXTRA_COMPONENT_DIRS`
   components. Observed tail of `__BUILD_COMPONENT_TARGETS`:
   `… ___idf_ringkit;___idf_main;___idf_glyph-anime`.
2. `__component_set_dependencies()` only links a requirement if the required
   component's target is *already* registered:

   ```cmake
   macro(__component_set_dependencies reqs type)
       foreach(req ${reqs})
           if(req IN_LIST build_component_targets)   # <-- silently skips otherwise
   ```

So a `REQUIRES` entry pointing at an example is dropped **silently** — the
build then fails on `#include "ring_example.h"` with no hint about the cause
(the IDF hint module even suggests adding the component to `REQUIRES`, which is
what it already is). This is not dash-specific: `hello` failed identically.

**Implemented instead:** `main/CMakeLists.txt` `include()`s the selected
example's `CMakeLists.txt`, which only sets three variables:

```cmake
set(RING_EXAMPLE_SRCS         "glyph_anime.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")
```

`main` then calls `idf_component_register()` with the example's sources, include
dirs and extra requirements folded in. Simpler, and it removes the whole class
of ordering bugs. Cost: examples are not standalone components, so they cannot
be unit-tested or depended on independently.

### 10.2 `main/CMakeLists.txt` is evaluated twice

ESP-IDF also runs a **requirements-gathering pass**
(`scripts/component_get_requirements.cmake` → `build_properties.temp.cmake`) that
`include()`s every component's `CMakeLists.txt` in a *separate CMake process*.
In that pass:

- cache variables are **not** set (`RING_APP` is empty), and
- `CMAKE_SOURCE_DIR` is the **build directory**, not the project.

Hence `main/CMakeLists.txt` resolves the root via
`${CMAKE_CURRENT_LIST_DIR}/..` and falls back to `glyph-anime` when `RING_APP`
is empty. Getting this wrong produced
`include could not find requested file: …/build/…/examples//CMakeLists.txt`.

### 10.3 `hello` is bigger than `glyph-anime`

Not a regression: `hello` is the only app using `lv_font_montserrat_48`, which
pulls in the 48 px font tables (~1.2 MB vs ~0.75 MB binary).
