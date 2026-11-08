/*
 * ringkit — NVS helpers. See ring_nvs.h.
 */
#include "ringkit/ring_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "ring_nvs"

esp_err_t ring_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs re-format, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t ring_nvs_get_str(const char *ns, const char *key,
                           char *out, size_t len)
{
    if (ns == NULL || key == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t need = len;
    err = nvs_get_str(h, key, out, &need);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    /* Guard against a malformed/unterminated stored value. */
    if (need >= len) {
        out[len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t ring_nvs_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
