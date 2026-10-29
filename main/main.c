/*
 * Smart Ring Plus (VIEWE) — Hello World
 *
 * Boots the board via the viewesmart/smartring_plus BSP and shows
 * "Hello World" centered on the 1.8" 360x360 ST77916 QSPI round display.
 *
 * Init order matters: bsp_display_init() creates the shared I2C bus and the
 * LVGL task, so it must be called first. All UI work must happen inside
 * esp_lv_adapter_lock()/esp_lv_adapter_unlock().
 */
#include "bsp/smartring_plus.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "smartring";

void app_main(void)
{
    ESP_LOGI(TAG, "Smart Ring Plus Hello World starting...");

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

    /* --- Build the UI under the LVGL lock --- */
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    lv_obj_t *scr = lv_screen_active();
    /* White background. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* "Hello World" in a large font that fills the screen; black text.
     * Keep the label auto-sized to its text and center the object so the
     * text is centered both horizontally AND vertically on the round panel. */
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello World");
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_center(label);

    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "Hello World is on screen.");
}
