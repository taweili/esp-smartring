/*
 * ringkit — NVS helpers shared by the Wi-Fi, secret and Bluetooth modules.
 *
 * Every module that touches NVS needs the same init dance (handle a clobbered
 * partition) and the same open/read/write sequence. Keeping it here means the
 * error handling lives in one place.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise NVS, erasing and re-formatting a clobbered partition.
 *
 * Idempotent. Safe to call from ring_wifi_init(), ring_bt_init() or
 * ring_secret_get().
 *
 * @return ESP_OK on success.
 */
esp_err_t ring_nvs_init(void);

/**
 * @brief Read a string key.
 *
 * @param ns    NVS namespace.
 * @param key   NVS key.
 * @param out   Destination; always NUL-terminated on success.
 * @param len   Size of @p out.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when the namespace or key is
 *         absent.
 */
esp_err_t ring_nvs_get_str(const char *ns, const char *key,
                           char *out, size_t len);

/**
 * @brief Write a string key and commit. Creates the namespace if needed.
 *
 * @param ns     NVS namespace.
 * @param key    NVS key.
 * @param value  Value to store.
 * @return ESP_OK on success.
 */
esp_err_t ring_nvs_set_str(const char *ns, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
