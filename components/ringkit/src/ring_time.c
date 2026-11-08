/*
 * ringkit — wall-clock sync. See ring_time.h.
 */
#include "ringkit/ring_time.h"

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ring_time"

#define RING_TIME_SERVERS  "ntp.aliyun.com", "pool.ntp.org"

esp_err_t ring_time_sync(uint32_t timeout_ms)
{
    static bool inited = false;
    if (!inited) {
        /* Init once: a retry must not re-init SNTP, and the clock may already
         * be valid (waiting for a fresh sync event would then just time out). */
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST(RING_TIME_SERVERS));
        config.start = true;
        config.wait_for_sync = true;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
            return err;
        }
        inited = true;
    }
    esp_netif_sntp_start();

    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (timeout_ms == 0) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sntp sync wait: %s", esp_err_to_name(err));
            return err;
        }
    }

    time_t now = 0;
    struct tm ti = { 0 };
    time(&now);
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return ESP_OK;
}
