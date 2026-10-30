/*
 * wifi-demo — Wi-Fi station bring-up + status on the round screen.
 *
 * Demonstrates the ring_wifi module:
 *   - ring_wifi_start() seeds/loads credentials from NVS (defaults to
 *     "HUAWEI-10P8UN" / "Xx041125" on first boot, then persisted) and
 *     connects in station mode.
 *   - The screen shows SSID, IP and the station state, refreshed at 5 Hz.
 *
 * The connection itself is asynchronous: ring_wifi_start() returns once the
 * driver is started, and the WIFI_EVENT / IP_EVENT handlers drive the rest.
 * This example just reads ring_wifi_state()/ring_wifi_is_connected().
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <stdio.h>

#define TAG "wifi-demo"

#define UI_PERIOD_MS 200   /* 5 Hz status refresh */

static lv_obj_t *s_ssid_label;
static lv_obj_t *s_state_label;
static uint32_t  s_refresh_count;

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
        lv_obj_align(s_ssid_label, LV_ALIGN_CENTER, 0, -28);

        /* State / IP, recoloured in refresh_cb. */
        s_state_label = ring_ui_add_label(scr, "starting...", &lv_font_montserrat_20,
                                          lv_color_hex(0xe74c3c));
        lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 4);

        lv_timer_create(refresh_cb, UI_PERIOD_MS, NULL);
    }

    /* Kick off the station. Non-blocking: events drive the rest. */
    esp_err_t err = ring_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi started, awaiting IP (see screen / log)");

    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "wifi-demo";
}
