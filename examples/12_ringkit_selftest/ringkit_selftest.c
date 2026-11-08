/*
 * 12_ringkit_selftest — bring-up diagnostic for every ringkit subsystem.
 *
 * main.c has already brought the board up (display, battery, power, audio,
 * SD, IMU) and logged the summary. This app goes one level deeper: it probes
 * each peripheral through the ringkit API, then exercises the optional
 * modules (Wi-Fi, BLE, and the three voice modules) and reports the result
 * on screen and in the log.
 *
 * The sequence runs in its own task so the UI stays alive; a 5 Hz timer copies
 * the status into the labels under the LVGL lock.
 *
 * What it proves:
 *   - display / touch / audio handles are live (the BSP wrappers work),
 *   - the IMU is producing samples,
 *   - the SD mount state is readable (no card is a normal "no card"),
 *   - the battery fuel gauge responds,
 *   - ring_wifi_init/start and ring_bt_init/advertising succeed,
 *   - ring_net / ring_ws / ring_chat initialise and tear down cleanly.
 *
 * It does NOT make a network call: Wi-Fi association depends on credentials
 * and an AP, and the voice modules are only initialised (a full call needs a
 * human + network — see examples/06, 07 and 10).
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAG "12_ringkit_selftest"

#define UI_PERIOD_MS       200
#define SELFTEST_STACK     6144
#define WIFI_WAIT_MS       10000
#define WIFI_POLL_MS       200

/* ------------------------------------------------------- status table */

enum {
    ST_DISPLAY = 0,
    ST_TOUCH,
    ST_AUDIO,
    ST_IMU,
    ST_SD,
    ST_BATTERY,
    ST_WIFI,
    ST_BT,
    ST_NET,
    ST_WS,
    ST_CHAT,
    ST_COUNT,
};

static const char *const s_names[ST_COUNT] = {
    "display", "touch", "audio", "imu", "sd", "battery",
    "wifi", "bt", "ring_net", "ring_ws", "ring_chat",
};

static lv_obj_t *s_labels[ST_COUNT];
static char        s_status[ST_COUNT][24];
static SemaphoreHandle_t s_status_mutex;

/* Written by the self-test task, read by the UI timer. */
static void set_status(int idx, const char *fmt, ...)
{
    char buf[sizeof(s_status[0])];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(s_status[idx], sizeof(s_status[idx]), "%s", buf);
        xSemaphoreGive(s_status_mutex);
    }
}

/* ------------------------------------------------------------- BLE config

 * A minimal GATT database: one vendor service with a readable name
 * characteristic. The selftest only proves the host comes up and advertises;
 * it does not wait for a peer. */
#define BT_SVC_UUID  0xFFE0
#define BT_CHR_NAME  0xFFE1

static esp_err_t bt_access(uint16_t conn_handle, uint16_t chr_uuid,
                           bool is_read, const uint8_t *data, size_t len,
                           uint8_t *out, size_t *out_len, void *user)
{
    (void)conn_handle; (void)data; (void)len; (void)user;

    if (chr_uuid == BT_CHR_NAME && is_read) {
        const char *name = "SmartRing";
        size_t n = strlen(name);
        if (n > *out_len) n = *out_len;
        memcpy(out, name, n);
        *out_len = n;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static const ring_bt_chr_t bt_chrs[] = {
    { .uuid = BT_CHR_NAME, .flags = RING_BT_CHR_READ, .max_len = 32, .cb = bt_access },
    { .uuid = 0 },
};

static const ring_bt_svc_t bt_svcs[] = {
    { .svc_uuid = BT_SVC_UUID, .chars = bt_chrs },
    { .svc_uuid = 0 },
};

static const ring_bt_config_t bt_cfg = {
    .name        = "SmartRing",
    .svcs        = bt_svcs,
    .adv_uuid    = BT_SVC_UUID,
    .bonding     = true,
    .mitm        = false,
    .secure_conn = true,
    .io_cap      = 0x03,   /* BLE_SM_IO_CAP_NO_IO */
};

/* ------------------------------------------------------------- voice config */

static const ring_chat_cfg_t chat_cfg = {
    .stt_model         = "qwen3-asr-flash",
    .llm_model         = "qwen3.8-flash",
    .tts_model         = "qwen-audio-3.1-tts-flash",
    .voice             = "longanhuan_v3.1",
    .system_prompt     = "You are a helpful assistant.",
    .stt_language      = NULL,
    .max_history_turns = 10,
};

/* --------------------------------------------------------------- helpers */

static const char *bt_state_name(ring_bt_state_t st)
{
    switch (st) {
    case RING_BT_OFF:          return "off";
    case RING_BT_IDLE:         return "idle";
    case RING_BT_ADVERTISING:  return "advertising";
    case RING_BT_CONNECTED:    return "connected";
    }
    return "?";
}

/* --------------------------------------------------------------- UI timer */

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    RING_UI_LOCKED(-1) {
        for (int i = 0; i < ST_COUNT; i++) {
            char line[48];
            if (xSemaphoreTake(s_status_mutex, 0) == pdTRUE) {
                snprintf(line, sizeof(line), "%-9s %s",
                         s_names[i], s_status[i]);
                xSemaphoreGive(s_status_mutex);
            } else {
                snprintf(line, sizeof(line), "%-9s ?", s_names[i]);
            }
            lv_label_set_text(s_labels[i], line);
        }
    }
}

/* ----------------------------------------------------------- self-test */

static void selftest_task(void *arg)
{
    (void)arg;

    /* ---- board peripherals (already brought up by main.c) ---- */
    set_status(ST_DISPLAY, "%s", ring_display_handle() ? "ok" : "fail");
    set_status(ST_TOUCH,   "%s", ring_touch_handle() ? "ok" : "fail");
    set_status(ST_AUDIO,   "%s", ring_audio_codec() ? "ok" : "fail");

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    ring_imu_accel(&ax, &ay, &az);
    set_status(ST_IMU, "%.2f %.2f %.2f", ax, ay, az);

    set_status(ST_SD, "%s", ring_sd_is_mounted() ? "mounted" : "no card");

    ring_battery_data_t bat;
    ring_battery_get(&bat);
    set_status(ST_BATTERY, "%d%%", bat.percent);

    /* ---- Wi-Fi ---- */
    esp_err_t err = ring_wifi_init();
    set_status(ST_WIFI, "%s", err == ESP_OK ? "init ok" : "init fail");
    if (err == ESP_OK) {
        if (ring_wifi_start() == ESP_OK) {
            int waited = 0;
            while (!ring_wifi_is_connected() && waited < WIFI_WAIT_MS) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
                waited += WIFI_POLL_MS;
            }
            char ip[16] = "";
            ring_wifi_get_ip(ip, sizeof(ip));
            set_status(ST_WIFI, "%s",
                       ring_wifi_is_connected() ? ip : "no ip");
            ESP_LOGI(TAG, "wifi: %s (%s)", ring_wifi_is_connected() ? "connected" : "no ip",
                     ip);
        } else {
            set_status(ST_WIFI, "start fail");
        }
    }

    /* ---- BLE ---- */
    err = ring_bt_init(&bt_cfg);
    set_status(ST_BT, "%s", err == ESP_OK ? "init ok" : "init fail");
    if (err == ESP_OK) {
        ring_bt_start_advertising();
        vTaskDelay(pdMS_TO_TICKS(500));
        set_status(ST_BT, "%s", bt_state_name(ring_bt_state()));
        ESP_LOGI(TAG, "bt: %s", bt_state_name(ring_bt_state()));
    }

    /* ---- voice modules: init then stop ---- */
    err = ring_net_init(NULL, NULL, NULL);
    set_status(ST_NET, "%s", err == ESP_OK ? "init ok" : "init fail");
    ESP_LOGI(TAG, "ring_net_init: %s", esp_err_to_name(err));

    err = ring_ws_init(NULL, NULL, NULL, NULL);
    set_status(ST_WS, "%s", err == ESP_OK ? "init ok" : "init fail");
    ESP_LOGI(TAG, "ring_ws_init: %s", esp_err_to_name(err));

    err = ring_chat_init(&chat_cfg, "sk-test");
    set_status(ST_CHAT, "%s", err == ESP_OK ? "init ok" : "init fail");
    ESP_LOGI(TAG, "ring_chat_init: %s", esp_err_to_name(err));

    /* Tear the sessions back down. The voice modules have no full deinit
     * (the PSRAM buffers stay allocated), but stop must be safe. */
    ring_net_stop();
    ring_ws_stop();
    set_status(ST_NET, "stopped");
    set_status(ST_WS, "stopped");

    ESP_LOGI(TAG, "selftest complete");
}

/* --------------------------------------------------------------- app */

esp_err_t ring_example_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        ESP_LOGE(TAG, "status mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *title = ring_ui_add_label(scr, "ringkit selftest",
                                            &lv_font_montserrat_24,
                                            lv_color_white());
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

        /* Status lines, evenly spaced down the screen. */
        for (int i = 0; i < ST_COUNT; i++) {
            s_labels[i] = ring_ui_add_label(scr, "...", &lv_font_montserrat_16,
                                            lv_color_hex(0x95a5a6));
            lv_obj_align(s_labels[i], LV_ALIGN_CENTER, 0,
                         -120 + i * 22);
        }

        lv_timer_create(refresh_cb, UI_PERIOD_MS, NULL);
    }

    xTaskCreate(selftest_task, "ringkit-selftest", SELFTEST_STACK, NULL, 5, NULL);
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "12_ringkit_selftest";
}
