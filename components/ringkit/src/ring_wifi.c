/*
 * ringkit — Wi-Fi station mode. See ring_wifi.h.
 *
 * Design notes:
 *  - NVS holds the SSID/password. On first boot ring_wifi_start() seeds it
 *    from the compile-time defaults so the ring auto-joins "HUAWEI-10P8UN".
 *  - All state transitions happen in the Wi-Fi / IP event handlers (event
 *    loop task); ringkit only reports them, protected by a mutex.
 *  - Disconnect auto-reconnects with a 2 s backoff so a flaky AP does not
 *    cause a tight reconnect loop.
 */
#include "ringkit/ring_wifi.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"

#define TAG "ring_wifi"

/* --- internal state ----------------------------------------------------- */

typedef struct {
    ring_wifi_state_t state;
    char ip[16];            /* dotted quad */
    char ssid[32];          /* active SSID */
    bool connected;         /* IP obtained */
} ring_wifi_ctx_t;

static ring_wifi_ctx_t s_state = {
    .state = RING_WIFI_OFF,
    .ip = "0.0.0.0",
    .ssid = "",
    .connected = false,
};
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;   /* driver/event-loop once-guard */

/* Backoff so WIFI_EVENT_STA_DISCONNECTED storms don't spin on esp_wifi_connect. */
static int64_t s_last_connect_ms = 0;
#define RECONNECT_BACKOFF_MS 2000

static void state_lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void state_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

/* --- NVS helpers -------------------------------------------------------- */

static esp_err_t nvs_get_blob_str(nvs_handle_t h, const char *key,
                                  char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        return err;
    }
    /* Guard against a malformed/unterminated stored value. */
    if (len >= out_len) {
        out[out_len - 1] = '\0';
    }
    return ESP_OK;
}

/**
 * Load persisted credentials; if the namespace/key is missing entirely,
 * seed it from the compile-time defaults and write them back. Returns ESP_OK
 * with ssid/password filled in on success.
 */
static esp_err_t load_or_seed_credentials(char *ssid, size_t ssid_len,
                                          char *password, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RING_WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob_str(h, RING_WIFI_NVS_KEY_SSID, ssid, ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_blob_str(h, RING_WIFI_NVS_KEY_PASS, password, pass_len);
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot: seed from compile-time defaults and persist. */
        snprintf(ssid, ssid_len, "%s", RING_WIFI_DEFAULT_SSID);
        snprintf(password, pass_len, "%s", RING_WIFI_DEFAULT_PASSWORD);
        err = nvs_set_str(h, RING_WIFI_NVS_KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, RING_WIFI_NVS_KEY_PASS, password);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "seeded default credentials for '%s' into NVS", ssid);
        }
    }

    nvs_close(h);
    return err;
}

/* --- event handlers ------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started, connecting...");
        state_lock();
        s_state.state = RING_WIFI_CONNECTING;
        state_unlock();
        esp_wifi_connect();

    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated, waiting for IP...");
        state_lock();
        s_state.state = RING_WIFI_CONNECTING;
        state_unlock();

    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected: reason %d", e ? e->reason : -1);
        state_lock();
        s_state.state = RING_WIFI_DISCONNECTED;
        s_state.connected = false;
        snprintf(s_state.ip, sizeof(s_state.ip), "0.0.0.0");
        state_unlock();

        /* Auto-reconnect with backoff. */
        if (now_ms() - s_last_connect_ms >= RECONNECT_BACKOFF_MS) {
            s_last_connect_ms = now_ms();
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reconnect attempt failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "reconnecting...");
            }
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        state_lock();
        s_state.state = RING_WIFI_CONNECTED;
        s_state.connected = true;
        snprintf(s_state.ip, sizeof(s_state.ip), IPSTR, IP2STR(&e->ip_info.ip));
        state_unlock();
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

/* --- public API ---------------------------------------------------------- */

esp_err_t ring_wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    /* NVS (Wi-Fi + credentials live here). Handle a clobbered flash. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs re-format, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    /* The BSP does not create a default loop; if something else did, that's fine. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_event_loop_create_default failed");
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    /* Handlers must be registered before esp_wifi_start(). */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, NULL, NULL),
        TAG, "WIFI_EVENT handler register failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &ip_event_handler, NULL, NULL),
        TAG, "IP_EVENT handler register failed");

    state_lock();
    s_state.state = RING_WIFI_DISCONNECTED;
    state_unlock();

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi stack initialised");
    return ESP_OK;
}

esp_err_t ring_wifi_start(void)
{
    esp_err_t err = ring_wifi_init();
    ESP_RETURN_ON_ERROR(err, TAG, "init failed");

    char ssid[32] = "", pass[64] = "";
    err = load_or_seed_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_RETURN_ON_ERROR(err, TAG, "credential load failed: %s", esp_err_to_name(err));

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config failed");

    state_lock();
    snprintf(s_state.ssid, sizeof(s_state.ssid), "%s", ssid);
    state_unlock();

    ESP_LOGI(TAG, "starting station for '%s'", ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    /* WIFI_EVENT_STA_START -> connect is driven by the event handler. */
    return ESP_OK;
}

esp_err_t ring_wifi_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(RING_WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");
    err = nvs_set_str(h, RING_WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, RING_WIFI_NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "credential store failed: %s", esp_err_to_name(err));

    /* Live reconnect with the new credentials. */
    if (s_initialized && ring_wifi_state() != RING_WIFI_OFF) {
        ESP_RETURN_ON_ERROR(esp_wifi_disconnect(), TAG, "disconnect failed");
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", password);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config failed");
        state_lock();
        snprintf(s_state.ssid, sizeof(s_state.ssid), "%s", ssid);
        state_unlock();
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect failed");
    }
    return ESP_OK;
}

esp_err_t ring_wifi_get_credentials(char *ssid, size_t ssid_len,
                                    char *password, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RING_WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid && ssid_len) {
        err = nvs_get_blob_str(h, RING_WIFI_NVS_KEY_SSID, ssid, ssid_len);
    }
    if (err == ESP_OK && password && pass_len) {
        err = nvs_get_blob_str(h, RING_WIFI_NVS_KEY_PASS, password, pass_len);
    }
    nvs_close(h);
    return err;
}

esp_err_t ring_wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_disconnect();
}

esp_err_t ring_wifi_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    state_lock();
    s_state.state = RING_WIFI_DISCONNECTED;
    s_state.connected = false;
    state_unlock();
    return esp_wifi_stop();
}

ring_wifi_state_t ring_wifi_state(void)
{
    ring_wifi_state_t st;
    state_lock();
    st = s_state.state;
    state_unlock();
    return st;
}

bool ring_wifi_is_connected(void)
{
    bool c;
    state_lock();
    c = s_state.connected;
    state_unlock();
    return c;
}

esp_err_t ring_wifi_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    state_lock();
    snprintf(buf, len, "%s", s_state.ip);
    state_unlock();
    return ESP_OK;
}

esp_err_t ring_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (ring_wifi_is_connected()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ring_wifi_is_connected() ? ESP_OK : ESP_ERR_TIMEOUT;
}
