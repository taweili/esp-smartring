# Architecture & rationale — generic skeleton + `examples/`

**Status:** implemented and current. This note records **why** the repo is
structured as a generic skeleton (`components/ringkit/`) plus interchangeable
application examples (`examples/<name>/`), exactly one of which is compiled into
each firmware image. It is the design-rationale counterpart to `README.md`
(what/how to build) and `AGENTS.md` (developer guidance); the two ESP-IDF traps
in §6 are the reason the layout is *not* the obvious "one component per app" one.

---

## 1. Why the layout

The repo used to be a single app: `main/main.c` did BSP bring-up *and* app
selection, and `main/launcher.c` was the only UI. Nothing was reusable — the
bring-up order, LVGL lock discipline, theme constants and the xorshift32 RNG
were all trapped in the launcher, and adding a second UI meant editing
`main.c`, `main/CMakeLists.txt` and fighting `launcher.h`.

The split makes the skeleton the only place that talks to the BSP, and gives
every app for free: the correct bring-up order, a leak-proof LVGL lock, the
shared look and the deterministic RNG. **Adding an app is one directory** and
never touches `main/` or `components/`.

---

## 2. Current tree

```
.
├── CMakeLists.txt                  # RING_APP selection, then project(smartring)
├── README.md                       # project entry: overview, quick start, docs map
├── sdkconfig.defaults              # PSRAM octal, FREERTOS_HZ=1000, LVGL/Montserrat
├── main/
│   ├── CMakeLists.txt              # include()s examples/${RING_APP}/CMakeLists.txt
│   ├── idf_component.yml           # the ONLY dependency manifest
│   └── main.c                      # app-agnostic: ring_board_init -> ring_example_start
├── components/
│   ├── ringkit/                    # generic skeleton: wraps the whole BSP
│   ├── espressif__esp_codec_dev/   # vendored override (ESP-IDF v6 compat)
│   └── viewesmart__smartring_plus/ # vendored override (gyro+temp exposure)
├── examples/
│   ├── README.md                   # app contract + how to add one
│   ├── glyph-anime/                # default: 13×13 glyph grid, layered reveal (README: design spec)
│   ├── imu-dash/                   # live IMU dashboard (accel/gyro/temp/tilt)
│   ├── gesture-test/               # single-finger gesture probe
│   ├── voice-note/                 # mic+speaker loop-back (hold to record)
│   └── hello/                      # bring-up smoke test
└── notes/
    ├── esp-smartring.md            # hardware + BSP reference
    └── architecture.md          # this file
```

---

## 3. The skeleton (`components/ringkit/`)

`ringkit/ringkit.h` is the one header apps include; it exposes the whole board
through `ring_*` wrappers plus UI/theme/RNG. Apps **never** include
`bsp/smartring_plus.h` and never call `bsp_*` directly.

| Module | Covers |
|--------|--------|
| `ring_board` | `ring_board_init()` (bring-up order + summary), `ring_board_log_summary()`, `ring_board_i2c_bus()` |
| `ring_display` | backlight, LVGL display handle, RGB565 swap-bytes |
| `ring_touch` | `esp_lcd_touch` handle (raw coordinates) |
| `ring_battery` | PMIC type (V1 ADC / V2 AXP2101), snapshot, percent / voltage / charging |
| `ring_power` | `ring_power_shutdown()` (GPIO47 hold ~3.5 s) |
| `ring_audio` | codec handle, I2S RX, blocking read/write, mute |
| `ring_imu` | accel, gyro, temperature, screen-plane tilt, level calibration |
| `ring_sd` | mount state, card handle, sorted directory listing |
| `ring_ui` | LVGL lock + `RING_UI_LOCKED(-1) { ... }`, label add, exact centring |
| `ring_theme.h` | colours, panel geometry (360×360, CX/CY), motion timings |
| `ring_rand` | deterministic xorshift32 (UI randomness reproducible) |
| `ring_types.h` | shared value types mirroring BSP structs/enums |

### Board bring-up

`ring_board_init()` encodes the order, which is not obvious from the individual
`bsp_*` calls: **display first** — `bsp_display_init()` creates the shared I2C
bus *and* starts the LVGL task — then battery → power → audio as mandatory
steps (first failure returned, later stages skipped), then SD → IMU which fail
soft and are reported in `ring_board_t`. `ring_board_log_summary()` prints one
line per subsystem.

### Lock discipline

All LVGL work happens under `RING_UI_LOCKED(-1) { ... }`, a scoped macro built
on `ring_ui_lock()` / `ring_ui_unlock()`. It releases the lock on every exit
path, including `return` and `break` — which a hand-written
`esp_lv_adapter_lock()` / `unlock()` pair leaks on early return.

---

## 4. Applications (`examples/`)

Selected at build time by the `RING_APP` cache variable (default `glyph-anime`).
Each app is three files and exports two symbols:

```c
esp_err_t   ring_example_start(void);   /* builds the UI; takes the LVGL lock */
const char *ring_example_name(void);
```

| App | Purpose |
|-----|---------|
| `glyph-anime` | **Default.** 13×13 glyph grid, layered center-out reveal, hue-cycling accents. Tap to open/close. Design: in-app `README.md`. |
| `imu-dash` | Live IMU dashboard: bubble level + accel + gyro + temperature + level calibration. |
| `gesture-test` | Single-finger gesture probe: tap / hold / swipe / press / release. |
| `voice-note` | Mic + speaker loop-back: hold to record, release to play back. |
| `hello` | Bring-up smoke test: white screen + one `Montserrat 48` label. |

`hello` is deliberately kept: it is the cheapest way to rule out
panel/adapter problems before blaming an app. It is also the only app using
`lv_font_montserrat_48`, so its binary is larger (~800 KB vs ~729 KB) — not a
regression.

### How exactly one app is selected

The project `CMakeLists.txt` is evaluated before `sdkconfig` exists, so a
**CMake cache variable** is the only viable selector there (a Kconfig `choice`
would compile the selection into `sdkconfig`, which is harder to script and
invites "I flashed the wrong app"). `main/CMakeLists.txt` `include()`s
`examples/${RING_APP}/CMakeLists.txt` — a three-line fragment that just sets
`RING_EXAMPLE_SRCS` / `RING_EXAMPLE_INCLUDE_DIRS` / `RING_EXAMPLE_REQUIRES` —
and folds those into its own `idf_component_register()`. An invalid `RING_APP`
fails fast at configure time with the list of valid choices.

```bash
# default app
idf.py build
idf.py -p /dev/ttyACM0 flash monitor

# a specific app — ALWAYS its own build dir, same -B for build/flash/monitor
idf.py -DRING_APP=imu-dash -B build/imu-dash build
idf.py -DRING_APP=imu-dash -B build/imu-dash -p /dev/ttyACM0 flash monitor
```

> Switching `RING_APP` inside one build tree leaves stale objects and produces
> confusing link errors.

---

## 5. What must not change

- `components/espressif__esp_codec_dev/` and
  `components/viewesmart__smartring_plus/` — vendored overrides. Project
  `components/` outrank `managed_components/`, so these win even if the
  component manager re-downloads the originals. Re-verify with the greps in
  `AGENTS.md` ("Critical fixes already applied").
- `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults` (the IMU sample task uses
  `pdMS_TO_TICKS(4)`; at the default 100 Hz tick that rounds to 0 and asserts
  `xTaskDelayUntil`).
- The 40-line LVGL draw buffer (full-panel buffering exhausts internal RAM).
- Init order: `bsp_display_init()` first.
- `main/idf_component.yml` as the single dependency manifest — no per-example
  manifests.

---

## 6. The two ESP-IDF traps that shaped the layout

### 6.1 Examples are CMake fragments, not components

The obvious design — every example a component registered via
`EXTRA_COMPONENT_DIRS`, all named the same so `main` could always
`REQUIRES ring_example` — **does not work in ESP-IDF.** Two facts, both verified
against this IDF (v6.1):

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
what it already is). Not dash-specific: `hello` failed identically.

**Implemented instead:** `main/CMakeLists.txt` `include()`s the selected
example's `CMakeLists.txt`, which only sets three variables
(`RING_EXAMPLE_SRCS` / `RING_EXAMPLE_INCLUDE_DIRS` / `RING_EXAMPLE_REQUIRES`);
`main` then folds them into its own `idf_component_register()`. Simpler, and it
removes the whole class of ordering bugs. Cost: examples are not standalone
components, so they cannot be unit-tested or depended on independently.

### 6.2 `main/CMakeLists.txt` is evaluated twice

ESP-IDF also runs a **requirements-gathering pass**
(`scripts/component_get_requirements.cmake` → `build_properties.temp.cmake`)
that `include()`s every component's `CMakeLists.txt` in a *separate CMake
process*. In that pass cache variables are **not** set (`RING_APP` is empty)
and `CMAKE_SOURCE_DIR` is the **build directory**, not the project. Hence
`main/CMakeLists.txt` resolves the root via `${CMAKE_CURRENT_LIST_DIR}/..` and
falls back to `glyph-anime` when `RING_APP` is empty. Getting this wrong
produced `include could not find requested file: …/build/…/examples//CMakeLists.txt`.

---

## 7. Deliberately not done

- **App registry (`ring_app`)** — the original plan sketched a `ring_app_t`
  descriptor with `ring_app_register()` / `ring_app_launch()` so the launcher
  could become an app switcher. Not implemented: one app per firmware, and it
  can be added later without another restructure.
- **`clock` / `dashboard` examples** — planned first-pass apps, not built. The
  examples that exist instead (`imu-dash`, `gesture-test`, `voice-note`)
  exercise the same peripheral surface.
