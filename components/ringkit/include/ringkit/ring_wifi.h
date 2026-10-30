/*
 * ringkit — Wi-Fi station mode with NVS-persisted credentials.
 *
 * This is a thin, self-contained wrapper around the ESP-IDF Wi-Fi stack that
 * the Smart Ring Plus can use without the application ever touching
 * esp_wifi/esp_netif directly.
 *
 * Credentials are stored in NVS (namespace "ring_wifi") so they survive
 * reboots and can be changed at runtime with ring_wifi_set_credentials().
 * If NVS has no credentials yet, ring_wifi_start() seeds them from the
 * compile-time defaults (RING_WIFI_DEFAULT_SSID / _PASSWORD, override with
 * -D or Kconfig) and persists them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time bootstrap credentials, used only when NVS is empty.
 *  Override from the build:  -DRING_WIFI_DEFAULT_SSID="..."  */
#ifndef RING_WIFI_DEFAULT_SSID
#define RING_WIFI_DEFAULT_SSID "HUAWEI-10P8UN"
#endif
#ifndef RING_WIFI_DEFAULT_PASSWORD
#define RING_WIFI_DEFAULT_PASSWORD "Xx041125"
#endif

/** NVS namespace holding the stored SSID/password. */
#define RING_WIFI_NVS_NAMESPACE "ring_wifi"
#define RING_WIFI_NVS_KEY_SSID  "ssid"
#define RING_WIFI_NVS_KEY_PASS  "pass"

/** Station lifecycle, safe to read from any task. */
typedef enum {
    RING_WIFI_OFF = 0,      /**< driver not started                          */
    RING_WIFI_DISCONNECTED, /**< started, not connected                      */
    RING_WIFI_CONNECTING,   /**< scan/auth/assoc in progress                 */
    RING_WIFI_CONNECTED,    /**< IP obtained (IP_EVENT_STA_GOT_IP)           */
} ring_wifi_state_t;

/**
 * @brief Initialise NVS, netif and the Wi-Fi driver.
 *
 * Idempotent (safe to call repeatedly). Does NOT start the radio — call
 * ring_wifi_start() afterwards. Should be called once from app_main, after
 * ring_board_init().
 */
esp_err_t ring_wifi_init(void);

/**
 * @brief Load credentials from NVS (seeding defaults if empty) and connect.
 *
 * Non-blocking: returns after esp_wifi_start()/esp_wifi_connect() are queued.
 * Use ring_wifi_wait_connected() if you need to block until the IP arrives.
 */
esp_err_t ring_wifi_start(void);

/**
 * @brief Persist credentials to NVS, then disconnect and reconnect with them.
 *
 * Useful for runtime provisioning: call with a new SSID/password and the
 * station re-associates immediately. Also seeds NVS if it was empty.
 */
esp_err_t ring_wifi_set_credentials(const char *ssid, const char *password);

/** Read the persisted credentials (NULL-tolerant pointers). */
esp_err_t ring_wifi_get_credentials(char *ssid, size_t ssid_len,
                                    char *password, size_t pass_len);

/** Disconnect (keeps the driver running and NVS credentials intact). */
esp_err_t ring_wifi_disconnect(void);

/** Stop the radio entirely (keeps NVS credentials). */
esp_err_t ring_wifi_stop(void);

/** Current station state (thread safe). */
ring_wifi_state_t ring_wifi_state(void);

/** True when IP_EVENT_STA_GOT_IP has been received. */
bool ring_wifi_is_connected(void);

/** Copy the station IPv4 as dotted quad, e.g. "192.168.1.50". */
esp_err_t ring_wifi_get_ip(char *buf, size_t len);

/**
 * @brief Block until connected or @p timeout_ms elapses.
 *
 * @return ESP_OK if the IP arrived, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t ring_wifi_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
