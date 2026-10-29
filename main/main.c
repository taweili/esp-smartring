/*
 * Smart Ring Plus (VIEWE) — Circular app-grid launcher
 *
 * Boots the board via the viewesmart/smartring_plus BSP and shows the
 * radial app-grid launcher (see notes/launcher-spec.md) on the 1.8"
 * 360x360 ST77916 QSPI round display.
 *
 * Init order matters: bsp_display_init() creates the shared I2C bus and the
 * LVGL task, so it must be called first. All UI work must happen inside
 * esp_lv_adapter_lock()/esp_lv_adapter_unlock().
 *
 * NOTE: when the console is the USB-Serial-JTAG port, log writes BLOCK if
 * the host is not draining it. Always run `idf.py monitor` (or otherwise read
 * /dev/ttyACM0 continuously) — otherwise the whole system appears to freeze
 * mid-boot. See notes/esp-smartring.md.
 */
#include "launcher.h"
#include "bsp/smartring_plus.h"
#include "esp_log.h"

static const char *TAG = "smartring";

void app_main(void)
{
    ESP_LOGI(TAG, "Smart Ring Plus launcher starting...");

    /* Display + touch + LVGL adapter + backlight (must be first). */
    ESP_ERROR_CHECK(bsp_display_init());

    /* Optional peripherals — fail softly where hardware may be absent. */
    ESP_ERROR_CHECK(bsp_battery_init());
    ESP_ERROR_CHECK(bsp_power_init());

    if (bsp_sd_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted");
    }
    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed");
    }
    ESP_ERROR_CHECK(bsp_audio_init());

    bsp_display_backlight_set(100);

    /* Build the launcher UI (takes the LVGL lock internally). */
    ESP_ERROR_CHECK(launcher_start());

    ESP_LOGI(TAG, "Launcher is on screen.");
}
