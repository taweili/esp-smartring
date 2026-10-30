/*
 * Example application contract.
 *
 * Every directory under examples/ provides this same header and compiles to a
 * component named after its directory. Only one example is on the component
 * search path at a time (see RING_APP in the top-level CMakeLists.txt), so
 * main/ can always include "ring_example.h" and call these two functions
 * regardless of which application is selected.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build this application's UI on the active LVGL screen.
 *
 * Called from app_main() AFTER ring_board_init(), so the LVGL task is running.
 * Implementations take the LVGL lock themselves (RING_UI_LOCKED).
 */
esp_err_t ring_example_start(void);

/** Short name for logging, e.g. "glyph-anime". */
const char *ring_example_name(void);

#ifdef __cplusplus
}
#endif
