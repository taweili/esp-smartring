/*
 * ringkit — board bring-up. See ring_board.h.
 */
#include "ringkit/ring_board.h"

#include "bsp/smartring_plus.h"
#include "esp_log.h"

static const char *TAG = "ringkit";

esp_err_t ring_board_init(ring_board_t *out)
{
    ring_board_t b = { 0 };
    esp_err_t err;

    /* 1. Display FIRST: creates the shared I2C bus and starts the LVGL task. */
    err = bsp_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        if (out) { *out = b; }
        return err;
    }
    b.display_ok = true;

    /* 2. Battery (V2 reuses the I2C bus created above). */
    err = bsp_battery_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery init failed: %s", esp_err_to_name(err));
        if (out) { *out = b; }
        return err;
    }
    b.battery_ok = true;

    /* 3. Power (soft power-off GPIO). */
    err = bsp_power_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power init failed: %s", esp_err_to_name(err));
        if (out) { *out = b; }
        return err;
    }
    b.power_ok = true;

    /* 4. Audio (mandatory, matches the original app behaviour). */
    err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        if (out) { *out = b; }
        return err;
    }
    b.audio_ok = true;

    /* 5. Optional peripherals: never fatal. */
    if (bsp_sd_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted (normal with no card inserted)");
    } else {
        b.sd_ok = true;
    }

    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (optional)");
    } else {
        b.imu_ok = true;
    }

    b.battery_v2 = (bsp_battery_get_pmic_type() == BSP_PMIC_V2_AXP2101);

    if (out) { *out = b; }
    return ESP_OK;
}

void ring_board_log_summary(const ring_board_t *b)
{
    if (b == NULL) {
        return;
    }
    ESP_LOGI(TAG, "display=%s battery=%s(%s) power=%s audio=%s sd=%s imu=%s",
             b->display_ok ? "ok" : "FAIL",
             b->battery_ok ? "ok" : "FAIL",
             b->battery_v2 ? "AXP2101/V2" : "ADC/V1",
             b->power_ok ? "ok" : "FAIL",
             b->audio_ok ? "ok" : "FAIL",
             b->sd_ok ? "ok" : "none",
             b->imu_ok ? "ok" : "FAIL");
}

i2c_master_bus_handle_t ring_board_i2c_bus(void)
{
    return bsp_i2c_get_bus();
}
