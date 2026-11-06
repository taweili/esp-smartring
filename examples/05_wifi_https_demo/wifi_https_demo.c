/*
 * 05_wifi_https_demo — Wi-Fi station bring-up + HTTPS connectivity test.
 *
 * Demonstrates the ring_wifi module:
 *   - ring_wifi_start() loads credentials from NVS (seeded from the local
 *     secrets header on first boot, then persisted) and connects in station
 *     mode.
 *   - The screen shows SSID, IP and the station state, refreshed at 5 Hz.
 *   - Once associated, a task syncs the clock (SNTP) and GETs
 *     https://baidu.com to prove outbound TLS works; the HTTP status and
 *     content length are shown on screen.
 *
 * The connection itself is asynchronous: ring_wifi_start() returns once the
 * driver is started, and the WIFI_EVENT / IP_EVENT handlers drive the rest.
 * This example just reads ring_wifi_state()/ring_wifi_is_connected().
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "arpa/inet.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define TAG "05_wifi_https_demo"

#define UI_PERIOD_MS 200    /* 5 Hz status refresh */
#define WIFI_TIMEOUT_MS 20000
#define HTTPS_TIMEOUT_MS 10000
#define SNTP_TIMEOUT_MS 15000
#define PROBE_TASK_STACK 6144
#define PROBE_RETRY_MS 5000  /* delay after a failed GET */

static lv_obj_t *s_ssid_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_https_label;
static uint32_t  s_refresh_count;

/* Written by the probe task, read by the UI timer; both hold the LVGL lock. */
static char s_https_result[64];

static const char *state_name(ring_wifi_state_t st)
{
    switch (st) {
    case RING_WIFI_OFF:          return "OFF";
    case RING_WIFI_DISCONNECTED: return "disconnected";
    case RING_WIFI_CONNECTING:   return "connecting...";
    case RING_WIFI_CONNECTED:    return "connected";
    }
    return "?";
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    ring_wifi_state_t st = ring_wifi_state();

    /* Periodic console telemetry so the connection lifecycle is visible
     * without the serial monitor being attached continuously. */
    if ((s_refresh_count++ % 25) == 0) {
        char ip[16] = "";
        if (ring_wifi_is_connected()) {
            ring_wifi_get_ip(ip, sizeof(ip));
        }
        ESP_LOGI(TAG, "status: %s, ip %s", state_name(st), ip);
    }

    RING_UI_LOCKED(-1) {
        char ssid[32] = "";
        char ip[16]   = "";
        char line[64] = "";

        /* Show the active SSID (from NVS) even before association. */
        ring_wifi_get_credentials(ssid, sizeof(ssid), NULL, 0);
        if (ssid[0] == '\0') {
            snprintf(ssid, sizeof(ssid), "(none)");
        }
        lv_label_set_text(s_ssid_label, ssid);

        if (ring_wifi_is_connected()) {
            ring_wifi_get_ip(ip, sizeof(ip));
            snprintf(line, sizeof(line), "IP %s", ip);
        } else {
            snprintf(line, sizeof(line), "%s", state_name(st));
        }
        lv_label_set_text(s_state_label, line);
        lv_obj_set_style_text_color(s_state_label,
                                    ring_wifi_is_connected() ? lv_color_hex(0x2ecc71)
                                                             : lv_color_hex(0xe74c3c),
                                    LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------- HTTPS probe */

/* Format outside the lock, copy + redraw inside it. */
static void set_https_result(const char *fmt, ...)
{
    char buf[sizeof(s_https_result)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    RING_UI_LOCKED(-1) {
        snprintf(s_https_result, sizeof(s_https_result), "%s", buf);
        lv_label_set_text(s_https_label, s_https_result);
    }
}

/* The public-CA certificate check compares against the wall clock, and the
 * ring has no battery-backed RTC, so sync before the TLS handshake. */
static void sync_clock(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    config.start = true;
    config.wait_for_sync = true;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
        return;
    }
    esp_netif_sntp_start();

    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sntp sync wait: %s", esp_err_to_name(err));
            return;
        }
    }

    time_t now = 0;
    struct tm ti = { 0 };
    time(&now);
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

/* Log the resolved address so a connect failure can be told apart from a
 * DNS failure. */
static void log_dns(const char *host)
{
    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, "443", &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGW(TAG, "dns: %s -> getaddrinfo failed (%d)", host, rc);
        return;
    }

    char ip[16] = "";
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "dns: %s -> %s", host, ip);
    freeaddrinfo(res);
}

/* One HTTPS GET. Returns true on a completed HTTP exchange (any status). */
static bool https_get_once(void)
{
    esp_http_client_config_t http = {
        .url = "https://baidu.com",
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTPS_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        /* Attach the bundled public root CAs (built because
         * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y). Without this esp-tls has no
         * trust anchor and refuses to start the handshake. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&http);
    if (c == NULL) {
        ESP_LOGE(TAG, "https client init failed");
        set_https_result("init failed");
        return false;
    }

    bool ok = false;
    esp_err_t err = esp_http_client_perform(c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "https GET failed: %s (status %d)",
                 esp_err_to_name(err), esp_http_client_get_status_code(c));
        set_https_result("retry: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(c);
        int length = esp_http_client_get_content_length(c);
        ESP_LOGI(TAG, "https://baidu.com -> HTTP %d, %d bytes", status, length);
        if (length >= 0) {
            set_https_result("HTTP %d, %d B", status, length);
        } else {
            /* Chunked response: no Content-Length. */
            set_https_result("HTTP %d", status);
        }
        ok = true;
    }

    esp_http_client_cleanup(c);
    return ok;
}

/*
 * Follow the link for the life of the app: wait for the station (ring_wifi
 * reconnects on its own after a reboot or an AP dropout), probe, and on a
 * failure or a lost link go back to waiting. Without this the probe would
 * only ever run once, so an AP that was down at boot would leave the screen
 * stuck on "no IP" until the next reboot.
 */
static void https_probe_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (ring_wifi_wait_connected(WIFI_TIMEOUT_MS) != ESP_OK) {
            set_https_result("waiting for Wi-Fi");
            continue;
        }

        sync_clock();
        log_dns("baidu.com");

        if (https_get_once()) {
            /* Success: idle until the link drops, then probe again. */
            while (ring_wifi_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            set_https_result("link down, retrying");
        } else {
            vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_MS));
        }
    }
}

/* ------------------------------------------------------------- app */

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *title = ring_ui_add_label(scr, "Wi-Fi", &lv_font_montserrat_24,
                                            lv_color_white());
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

        /* SSID (active credentials). */
        s_ssid_label = ring_ui_add_label(scr, "-", &lv_font_montserrat_20,
                                         lv_color_hex(0x95a5a6));
        lv_obj_align(s_ssid_label, LV_ALIGN_CENTER, 0, -64);

        /* State / IP, recoloured in refresh_cb. */
        s_state_label = ring_ui_add_label(scr, "starting...", &lv_font_montserrat_20,
                                          lv_color_hex(0xe74c3c));
        lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, -36);

        /* HTTPS probe result, updated by https_probe_task. */
        snprintf(s_https_result, sizeof(s_https_result), "baidu.com: ...");
        s_https_label = ring_ui_add_label(scr, s_https_result,
                                          &lv_font_montserrat_20,
                                          lv_color_hex(0x95a5a6));
        lv_obj_align(s_https_label, LV_ALIGN_CENTER, 0, -8);

        lv_timer_create(refresh_cb, UI_PERIOD_MS, NULL);
    }

    /* Kick off the station. Non-blocking: events drive the rest. */
    esp_err_t err = ring_wifi_start();
    if (err != ESP_OK) {
        /* No credentials yet (or the radio failed): keep the UI alive and
         * show why, rather than aborting the app. */
        ESP_LOGE(TAG, "ring_wifi_start failed: %s", esp_err_to_name(err));
        set_https_result("no Wi-Fi: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Wi-Fi started, awaiting IP (see screen / log)");

    xTaskCreate(https_probe_task, "https_probe", PROBE_TASK_STACK, NULL, 5, NULL);

    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "05_wifi_https_demo";
}
