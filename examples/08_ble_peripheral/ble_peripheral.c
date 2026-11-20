/*
 * 08_ble_peripheral — Bluetooth LE peripheral bring-up on the round screen.
 *
 * The ring advertises itself as a connectable BLE peripheral named
 * "SmartRing" with one vendor service (0xFFE0):
 *
 *   0xFFE1 "name"      READ     — returns the device name
 *   0xFFE2 "battery"   READ|NOTIFY — returns battery % (updated every 2 s)
 *   0xFFE3 "command"   WRITE|NOTIFY — write a command, get an "ok" notification
 *
 * A phone (nRF Connect / LightBlue) can scan for "SmartRing", connect, pair
 * (bonding is on), read the name/battery, write a command and receive a
 * notification. The screen shows the connection state and last activity.
 *
 * The connection itself is asynchronous: ring_bt_init() starts the host task
 * and advertising starts when the host syncs. This example just reads
 * ring_bt_state() / ring_bt_is_connected() from a 2 Hz UI timer.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#define TAG "08_ble_peripheral"

#define UI_PERIOD_MS 500    /* 2 Hz status + battery refresh */

/* --- UI handles ----------------------------------------------------------- */

static lv_obj_t *s_state_label;
static lv_obj_t *s_name_label;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_cmd_label;
static uint32_t  s_ticks;

/* --- GATT database ----------------------------------------------------------- */

static void bt_on_connect(uint16_t conn_handle, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "peer connected (handle %d)", conn_handle);
}

static void bt_on_disconnect(uint16_t conn_handle, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "peer disconnected (handle %d)", conn_handle);
}

/* The demo database lives in ringkit (name / battery / command) so the BLE
 * example and 12_ringkit_selftest cannot drift. */
static ring_bt_config_t bt_cfg = {
    .name          = "SmartRing",
    .svcs          = NULL,   /* set in ring_example_start() */
    .adv_uuid      = RING_BT_DEMO_SVC_UUID,
    .bonding       = true,     /* pair + bond so reconnects skip pairing */
    .mitm          = false,    /* Just Works (button-less ring)          */
    .secure_conn   = true,
    .io_cap        = 0x03,     /* BLE_SM_IO_CAP_NO_IO                    */
    .on_connect    = bt_on_connect,
    .on_disconnect = bt_on_disconnect,
};

/* --- UI ---------------------------------------------------------------------- */

static const char *state_name(ring_bt_state_t st)
{
    switch (st) {
    case RING_BT_OFF:          return "OFF";
    case RING_BT_IDLE:         return "idle";
    case RING_BT_ADVERTISING:  return "advertising";
    case RING_BT_CONNECTED:    return "connected";
    }
    return "?";
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    ring_bt_state_t st = ring_bt_state();
    int bat = ring_battery_percent();
    bool connected = ring_bt_is_connected();

    /* Notify the battery characteristic every 2 s (the UI tick is 500 ms). */
    if ((s_ticks % 4) == 0) {
        uint8_t b = (uint8_t)bat;
        ring_bt_notify(RING_BT_DEMO_CHR_BATTERY, &b, sizeof(b));
    }

    /* Periodic console telemetry so the lifecycle is visible in the log. */
    if ((s_ticks++ % 10) == 0) {
        ESP_LOGI(TAG, "status: %s, battery %d%%", state_name(st), bat);
    }

    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_state_label, state_name(st));
        lv_obj_set_style_text_color(s_state_label,
                                    connected ? lv_color_hex(0x2ecc71)
                                              : lv_color_hex(0xf39c12),
                                    LV_PART_MAIN);

        char line[24];
        snprintf(line, sizeof(line), "battery %d%%", bat);
        lv_label_set_text(s_batt_label, line);

        char last[32];
        char cmd[40];
        ring_bt_demo_last_command(last, sizeof(last));
        snprintf(cmd, sizeof(cmd), "last: %s", last);
        lv_label_set_text(s_cmd_label, cmd);
    }
}

esp_err_t ring_example_start(void)
{
    bt_cfg.svcs = ring_bt_demo_db();

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *title = ring_ui_add_label(scr, "BLE SmartRing",
                                            &lv_font_montserrat_24,
                                            lv_color_white());
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

        /* Device name (advertised) */
        s_name_label = ring_ui_add_label(scr, "SmartRing", &lv_font_montserrat_20,
                                         lv_color_hex(0x95a5a6));
        lv_obj_align(s_name_label, LV_ALIGN_CENTER, 0, -60);

        /* Connection state / battery */
        s_state_label = ring_ui_add_label(scr, "starting...", &lv_font_montserrat_20,
                                          lv_color_hex(0xf39c12));
        lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, -24);

        s_batt_label = ring_ui_add_label(scr, "battery --%", &lv_font_montserrat_20,
                                         lv_color_white());
        lv_obj_align(s_batt_label, LV_ALIGN_CENTER, 0, 8);

        /* Last received command (written by the shared demo DB). */
        s_cmd_label = ring_ui_add_label(scr, "last: -", &lv_font_montserrat_20,
                                        lv_color_hex(0x2ecc71));
        lv_obj_align(s_cmd_label, LV_ALIGN_CENTER, 0, 40);

        lv_obj_t *hint = ring_ui_add_label(scr, "scan + connect + pair",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x7f8c8d));
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

        lv_timer_create(refresh_cb, UI_PERIOD_MS, NULL);
    }

    /* Bring up the host; advertising starts once it syncs. */
    esp_err_t err = ring_bt_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_bt_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = ring_bt_start_advertising();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_bt_start_advertising failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BLE host up, advertising as '%s' (svc 0x%04X)",
             bt_cfg.name, RING_BT_DEMO_SVC_UUID);

    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "08_ble_peripheral";
}
