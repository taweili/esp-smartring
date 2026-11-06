/*
 * ringkit — NVS-backed secrets. See ring_secret.h.
 */
#include "ringkit/ring_secret.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "ring_secret"

esp_err_t ring_secret_get(const char *ns, const char *key,
                          const char *build_fallback, char *out, size_t len)
{
    if (ns == NULL || key == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t need = len;
        err = nvs_get_str(h, key, out, &need);
        nvs_close(h);
        if (err == ESP_OK && out[0] != '\0') {
            ESP_LOGI(TAG, "'%s/%s' loaded from NVS", ns, key);
            return ESP_OK;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open(%s): %s", ns, esp_err_to_name(err));
    }

    if (build_fallback == NULL || build_fallback[0] == '\0') {
        ESP_LOGE(TAG, "no '%s/%s' in NVS and no build-time value set", ns, key);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out, len, "%s", build_fallback);
    ESP_LOGW(TAG, "using build-time '%s/%s' (persisting to NVS)", ns, key);

    /* Persist so a later build does not need the value on the command line.
     * A failure here is not fatal: the value is still usable this run. */
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, key, out) == ESP_OK) {
            if (nvs_commit(h) != ESP_OK) {
                ESP_LOGW(TAG, "nvs_commit(%s/%s) failed", ns, key);
            }
        } else {
            ESP_LOGW(TAG, "nvs_set_str(%s/%s) failed", ns, key);
        }
        nvs_close(h);
    }
    return ESP_OK;
}
