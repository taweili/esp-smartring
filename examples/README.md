# Examples

Each directory here is a self-contained application for the VIEWE Smart Ring
Plus. Exactly **one** is compiled into a firmware image, selected by the
`RING_APP` CMake cache variable (default `glyph-anime`).

| App | Description |
|-----|-------------|
| `glyph-anime` | Full-screen glyph grid with a layered center-out reveal and hue-cycling accents. Touch to open/close. |
| `imu-dash` | Live IMU dashboard: bubble level (tilt) + accel (m/s²) + gyro (°/s) + temperature + level calibration. Tap to calibrate. |
| `gesture-test` | Touch gesture probe: shows the name of every single-finger gesture (tap / hold / swipe left/right/up/down, plus press/release) the moment it happens, with a dot following the finger. |
| `voice-note` | Mic + speaker loop-back: one round button — hold to record (ES8311 mic), release to play the captured PCM back through the speaker. Captures up to 8 s into PSRAM. |
| `wifi-demo` | Wi-Fi station bring-up: connects to the NVS-stored SSID (default `HUAWEI-10P8UN`), shows state/SSID/IP on the round screen. First boot seeds credentials into NVS. |
| `hello` | Bring-up smoke test: white screen, one large "Hello" glyph. |

## Building an app

```bash
# default app (glyph-anime)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor

# a specific app — ALWAYS use its own build directory
idf.py -DRING_APP=hello -B build/hello build
idf.py -DRING_APP=hello -B build/hello -p /dev/ttyACM0 flash monitor
```

> **Use a per-app build directory (`-B build/<app>`).** Switching `RING_APP`
> inside one build tree leaves stale objects and produces confusing link
> errors. If it happens, `idf.py fullclean` and rebuild.

An unknown name fails fast at configure time with the list of valid choices.

## The contract every example implements

Each example directory is a **CMake fragment** (not an ESP-IDF component) with
three files, and exports exactly two symbols via its own `include/ring_example.h`:

```c
esp_err_t   ring_example_start(void);  /* build the UI; takes the LVGL lock */
const char *ring_example_name(void);   /* e.g. "hello"                      */
```

`main/main.c` is application-agnostic: it calls `ring_board_init()` and then
`ring_example_start()`. `main/CMakeLists.txt` `include()`s the selected
example's `CMakeLists.txt` and compiles its sources into the `main` component,
so every example can use the same `ring_example.h` name.

> **Why not a component?** ESP-IDF registers `EXTRA_COMPONENT_DIRS` components
> *after* `main`, and `__component_set_dependencies()` silently drops a
> `REQUIRES` entry whose component isn't registered yet — losing both the
> include path and the link, with a misleading error. Detail in
> `notes/architecture.md` §6.1.

## Adding an example

```
examples/my-app/
├── CMakeLists.txt          # sets RING_EXAMPLE_SRCS / _INCLUDE_DIRS / _REQUIRES
├── include/ring_example.h  # copy from an existing example (identical)
└── my_app.c                # ring_example_start() + ring_example_name()
```

`examples/my-app/CMakeLists.txt` is three lines:

```cmake
set(RING_EXAMPLE_SRCS         "my_app.c")
set(RING_EXAMPLE_INCLUDE_DIRS "include")
set(RING_EXAMPLE_REQUIRES     "")   # extra component deps, e.g. "esp_driver_ledc"
```

Then `idf.py -DRING_APP=my-app -B build/my-app build`.

Rules:

- Use the directory name for the app; keep it `[a-z0-9-]+`.
- **Do not** add an `idf_component.yml` per example. Managed dependencies live
  in `main/idf_component.yml`.
- An app may carry its own `README.md` for a design/spec (see
  `glyph-anime/README.md`) — it is inert to the build, so it never touches the
  CMake fragment.
- **Do not** include `bsp/smartring_plus.h` and **do not** call `bsp_*`
  directly — everything goes through `ringkit/ringkit.h`.
- All LVGL work happens under `RING_UI_LOCKED(-1) { ... }`.

## Skeleton

`components/ringkit/` wraps the whole BSP: `ring_board`, `ring_display`,
`ring_touch`, `ring_battery`, `ring_power`, `ring_audio`, `ring_imu`,
`ring_sd`, plus `ring_ui` (lock/helpers), `ring_theme` (colours, geometry,
timings) and `ring_rand` (deterministic xorshift32).
