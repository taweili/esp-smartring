/*
 * ringkit — NVS-backed secrets. See ring_secret.h.
 */
#include "ringkit/ring_secret.h"
#include "ringkit/ring_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"   /* ESP_ERR_NVS_NOT_FOUND */

#define TAG "ring_secret"

esp_err_t ring_secret_get(const char *ns, const char *key,
                          const char *build_fallback, char *out, size_t len)
{
    if (ns == NULL || key == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    esp_err_t err = ring_nvs_get_str(ns, key, out, len);
    if (err == ESP_OK && out[0] != '\0') {
        ESP_LOGI(TAG, "'%s/%s' loaded from NVS", ns, key);
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str(%s/%s): %s", ns, key, esp_err_to_name(err));
    }

    if (build_fallback == NULL || build_fallback[0] == '\0') {
        ESP_LOGE(TAG, "no '%s/%s' in NVS and no build-time value set", ns, key);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out, len, "%s", build_fallback);
    ESP_LOGW(TAG, "using build-time '%s/%s' (persisting to NVS)", ns, key);

    /* Persist so a later build does not need the value on the command line.
     * A failure here is not fatal: the value is still usable this run. */
    err = ring_nvs_set_str(ns, key, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str(%s/%s) failed: %s", ns, key,
                 esp_err_to_name(err));
    }
    return ESP_OK;
}
