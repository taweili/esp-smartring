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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "arpa/inet.h"

#include <stdarg.h>
#include <stdio.h>

#define TAG "05_wifi_https_demo"

#define UI_PERIOD_MS 200    /* 5 Hz status refresh */
#define WIFI_TIMEOUT_MS 20000
#define PROBE_URL "https://baidu.com"
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

/* ------------------------------------------------------------- HTTPS probe
 *
 * The clock sync and the GET itself are ringkit's (`ring_net_sync_clock()` /
 * `ring_net_probe()`); this file only owns the retry loop and the UI. The DNS
 * lookup is kept because it separates "cannot resolve" from "cannot connect".
 * ------------------------------------------------------------------------- */

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

        /* The public-CA certificate check compares against the wall clock and
         * the ring has no battery-backed RTC, so sync before the handshake. */
        if (ring_net_sync_clock() != ESP_OK) {
            set_https_result("clock sync failed");
            vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_MS));
            continue;
        }
        log_dns("baidu.com");

        int status = ring_net_probe(PROBE_URL);
        if (status >= 200 && status < 300) {
            set_https_result("HTTP %d", status);
            /* Success: idle until the link drops, then probe again. */
            while (ring_wifi_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            set_https_result("link down, retrying");
        } else {
            set_https_result("probe failed");
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
