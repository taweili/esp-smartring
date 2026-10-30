/*
 * Smart Ring Plus (VIEWE) — application entry point.
 *
 * Deliberately application-agnostic: bring the board up through the ringkit
 * skeleton, then hand the screen to whichever example under examples/ was
 * selected with the RING_APP CMake variable (see examples/README.md).
 *
 * The board init ORDER lives in ring_board.c, not here — bsp_display_init()
 * must run first because it creates the shared I2C bus and starts the LVGL
 * task that every other subsystem (and the UI) depends on.
 *
 * NOTE: when the console is the USB-Serial-JTAG port, log writes BLOCK if the
 * host is not draining it. Always run `idf.py monitor` (or otherwise read
 * /dev/ttyACM0 continuously) — otherwise the whole system appears to freeze
 * mid-boot. See notes/esp-smartring.md.
 */
#include "ring_example.h"
#include "ringkit/ringkit.h"

#include "esp_log.h"

static const char *TAG = "smartring";

void app_main(void)
{
    ESP_LOGI(TAG, "Smart Ring Plus starting (ringkit %s, app: %s)",
             RINGKIT_VERSION, ring_example_name());

    ring_board_t board;
    ESP_ERROR_CHECK(ring_board_init(&board));
    ring_board_log_summary(&board);

    ring_display_backlight(100);

    /* The example takes the LVGL lock itself. */
    ESP_ERROR_CHECK(ring_example_start());

    ESP_LOGI(TAG, "app '%s' is on screen", ring_example_name());
}
