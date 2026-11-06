/*
 * 06_qianwenai_push_to_talk — one green button, hold-to-talk over WebRTC.
 *
 * The ring's screen is one round green button. Hold it to open the mic and
 * stream speech to qwen-audio-3.1-realtime-flash over WebRTC; release it and
 * the model's spoken reply comes out of the speaker.
 *
 * Why this shape:
 *  - WebRTC is the transport qianwenai.com documents for non-browser clients
 *    and it carries the model's echo cancellation / noise suppression.
 *  - The endpoint accepts only Opus on m=audio and supports only server-side
 *    VAD, so the button gates the uplink and the model decides the turn end.
 *    As soon as audio starts coming back we stop the uplink, which keeps the
 *    model from hearing itself through the ring's own speaker.
 *
 * Audio: the ES8311 is fixed at 48 kHz / 16-bit / stereo, which happens to be
 * exactly the rate ring_net runs Opus at. So this file only does the
 * stereo <-> mono conversion and the talk-turn bookkeeping.
 *
 * Task structure follows examples/04_voice_loopback: LVGL events run on the LVGL task
 * and only flip flags / spawn a worker; all blocking audio I/O happens on the
 * worker, which takes the LVGL lock only for brief UI updates.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"

#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "06_qianwenai_push_to_talk"

/* ------------------------------------------------------------- config */

/* Secrets are generated into ring_secrets.h by main/CMakeLists.txt from the
 * environment (tools/dev.sh sources secrets.local). Never committed. */
#include "ring_secrets.h"

/* Build-time fallback API key; NVS wins if present. */
#ifndef QIANWEN_API_KEY
#define QIANWEN_API_KEY ""
#endif

#define QIANWEN_MODEL          "qwen-audio-3.1-realtime-flash"
#define QIANWEN_NVS_NAMESPACE  "qianwen"
#define QIANWEN_NVS_KEY_KEY    "api_key"

/* ------------------------------------------------------------- audio */

/* BSP-fixed capture/playback format: 48 kHz / 16-bit / stereo interleaved.
 * ring_net wants 48 kHz / 16-bit / mono, in 20 ms frames. */
#define MIC_RATE        48000
#define FRAME_SAMPLES   RING_NET_FRAME_SAMPLES        /* 960 = 20 ms      */
#define FRAME_STEREO    (FRAME_SAMPLES * 2 * 2)       /* 3840 bytes       */
#define OUT_STEREO_MAX  (MIC_RATE * 2 * 2 / 1000 * 40) /* 40 ms = 15360 B */

#define TALK_GRACE_MS   3000    /* stop waiting this long after last audio */
#define TALK_MAX_MS     15000   /* hard cap on one reply                   */

/* ------------------------------------------------------------- task */

#define WORKER_STACK    8192
/* The Opus encoder's SILK path peaks at ~20 KB of stack (silk_encode_frame_FIX
 * alone has a ~9.5 KB frame); 8 KB overflowed on the first encoded frame. */
/* Mono SILK peaked at ~20 KB; the stereo frames the SDP negotiates (see
 * RING_NET_OPUS_CHANNELS in ring_net.c) need more, and 24 KB overflowed. */
#define TALK_TASK_STACK 65536
#define WORKER_PRIO     5       /* below LVGL (6): button events win */

/* ------------------------------------------------------------- UI */

#define TITLE_Y   34
#define BTN_CX    180
#define BTN_CY    158
#define BTN_R     78
#define STATUS_Y  258
#define BAR_W     220
#define BAR_H     6
#define BAR_Y     288
#define HINT_Y    320

#define COL_WAIT   RING_COLOR_GREEN          /* ready to talk   */
#define COL_CONN   lv_color_hex(0x3A4048)     /* connecting      */
#define COL_DIM    lv_color_hex(0x9E9E9E)
/* Red covers both recording and waiting on the server response. */
#define COL_TALK   lv_color_hex(0xFF1744)

/* ------------------------------------------------------------- state */

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_status;
static lv_obj_t *s_bar;
static lv_obj_t *s_hint;

static volatile bool s_stop = false;         /* RELEASED sets this        */
static volatile bool s_worker_busy = false;  /* a talk turn is in flight  */
static bool s_ui_ready = false;

static uint8_t s_stereo[FRAME_STEREO];       /* 20 ms capture bounce      */
static int16_t s_mono[FRAME_SAMPLES];
static int16_t s_reply[FRAME_SAMPLES];
static uint8_t s_play[OUT_STEREO_MAX];

static char s_api_key[160];

/* ------------------------------------------------------------- UI helpers */

static void ui_button(const char *text, const lv_font_t *font, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_text_font(s_btn_label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_btn_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(s_btn_label, LV_ALIGN_CENTER, 0, 0);
        /* Solid fill: the whole circle is the state colour, not an outline. */
        lv_obj_set_style_bg_color(s_btn, color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(color, 60),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
    }
}

static void ui_status(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, color, LV_PART_MAIN);
    }
}

static void ui_bar(uint8_t pct, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, color, LV_PART_INDICATOR);
    }
}

static void ui_hint(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_hint, text);
    }
}

static void ui_idle(const char *status)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, "HOLD\nTO\nTALK");
        lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_20,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(s_btn_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(s_btn_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_btn, COL_WAIT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_WAIT, 60),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, COL_WAIT, LV_PART_INDICATOR);
        lv_label_set_text(s_status, status ? status : "press & hold to talk");
        lv_obj_set_style_text_color(s_status, COL_DIM, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------- ring_net glue */

static void net_state_cb(ring_net_state_t st, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    switch (st) {
    case RING_NET_READY:
        /* Don't stomp on a turn that is still draining its reply. */
        if (!s_worker_busy) {
            ui_idle("ready — press & hold to talk");
        }
        break;
    case RING_NET_LISTENING:
        ui_button("TALK", &lv_font_montserrat_48, COL_TALK);
        ui_status("listening…", COL_TALK);
        ui_bar(0, COL_TALK);
        break;
    case RING_NET_SPEAKING:
        ui_button("REPLY", &lv_font_montserrat_48, COL_TALK);
        ui_status("replying…", COL_TALK);
        ui_bar(100, COL_TALK);
        break;
    case RING_NET_CONNECTING:
        ui_button("···", &lv_font_montserrat_48, COL_CONN);
        ui_status("connecting…", COL_DIM);
        ui_bar(0, COL_DIM);
        break;
    case RING_NET_ERROR:
        if (!s_worker_busy) {
            ui_button("!", &lv_font_montserrat_48, COL_TALK);
            ui_status("connection lost — reconnecting", COL_TALK);
        }
        break;
    case RING_NET_OFF:
    default:
        break;
    }
}

static void net_text_cb(const char *text, size_t len, bool is_user, void *user)
{
    (void)user;
    (void)is_user;           /* the hint line shows the model's reply */
    if (!s_ui_ready) {
        return;
    }
    static char line[48];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    ui_hint(line);
}

/* ------------------------------------------------------------- conversion */

/* 48 kHz stereo interleaved -> mono (average L/R). */
static void stereo_to_mono(const uint8_t *in, int16_t *out, size_t samples)
{
    const int16_t *src = (const int16_t *)in;
    for (size_t i = 0; i < samples; i++) {
        out[i] = (int16_t)(((int32_t)src[i * 2] + (int32_t)src[i * 2 + 1]) / 2);
    }
}

/* mono -> 48 kHz stereo interleaved. Returns bytes written. */
static size_t mono_to_stereo(const int16_t *in, size_t samples, uint8_t *out)
{
    int16_t *dst = (int16_t *)out;
    for (size_t i = 0; i < samples; i++) {
        dst[i * 2] = in[i];
        dst[i * 2 + 1] = in[i];
    }
    return samples * 4;
}

static uint8_t level_of(const int16_t *pcm, size_t samples)
{
    if (samples == 0) {
        return 0;
    }
    uint32_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        acc += (uint32_t)(v < 0 ? -v : v);
    }
    uint32_t avg = acc / samples;
    if (avg > 8000) {
        return 100;
    }
    return (uint8_t)(avg * 100 / 8000);
}

/* ------------------------------------------------------------- talk turn */

static void talk_worker(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "turn: capture");
    ui_hint("");

    /* Uplink: one 20 ms Opus frame at a time until released or the model
     * starts answering. */
    uint32_t frames = 0;
    while (!s_stop && ring_net_state() == RING_NET_LISTENING) {
        if (ring_audio_read(s_stereo, sizeof(s_stereo), 200) != ESP_OK) {
            ESP_LOGW(TAG, "mic read failed");
            break;
        }
        stereo_to_mono(s_stereo, s_mono, FRAME_SAMPLES);
        esp_err_t serr = ring_net_send_audio(s_mono, FRAME_SAMPLES);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "send audio failed: %s", esp_err_to_name(serr));
            break;
        }
        if ((frames++ % 50) == 0) {
            ESP_LOGI(TAG, "uplink frame %u, mic level %u", (unsigned)frames,
                     level_of(s_mono, FRAME_SAMPLES));
        }
        ui_bar(level_of(s_mono, FRAME_SAMPLES), COL_TALK);
    }

    ESP_LOGI(TAG, "turn: release");
    ring_net_set_listening(false);

    /* Downlink: play the reply until the model reports done and the buffer has
     * drained. The button stays red while we wait for the server. If the model
     * is already speaking, its SPEAKING callback owns the label. */
    if (ring_net_state() == RING_NET_READY) {
        ui_button("WAIT", &lv_font_montserrat_48, COL_TALK);
        ui_status("waiting…", COL_TALK);
    }
    TickType_t last = xTaskGetTickCount();
    TickType_t start = last;
    ring_audio_set_mute(true);

    for (;;) {
        size_t got = 0;
        if (ring_net_read_audio(s_reply, FRAME_SAMPLES, &got, 200) == ESP_OK &&
            got > 0) {
            size_t bytes = mono_to_stereo(s_reply, got, s_play);
            last = xTaskGetTickCount();
            ring_audio_set_mute(false);
            if (ring_audio_write(s_play, bytes) != ESP_OK) {
                ESP_LOGW(TAG, "speaker write failed");
            }
            ring_audio_set_mute(true);
            ui_bar(level_of(s_reply, got), COL_TALK);
        } else if (ring_net_state() == RING_NET_READY &&
                   (xTaskGetTickCount() - last) > pdMS_TO_TICKS(TALK_GRACE_MS)) {
            break;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(TALK_MAX_MS)) {
            ESP_LOGW(TAG, "reply cap reached");
            break;
        }
    }
    ring_audio_set_mute(true);

    ESP_LOGI(TAG, "turn: done");
    ui_hint("");
    s_worker_busy = false;
    ui_idle("ready — press & hold to talk");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- input */

static void btn_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (s_worker_busy || !s_ui_ready) {
            return;
        }
        if (ring_net_state() != RING_NET_READY) {
            ui_status("not ready yet", COL_DIM);
            return;
        }
        s_stop = false;
        s_worker_busy = true;
        if (ring_net_set_listening(true) != ESP_OK) {
            s_worker_busy = false;
            return;
        }
        if (xTaskCreate(talk_worker, "qw-talk", TALK_TASK_STACK, NULL,
                        WORKER_PRIO, NULL) != pdPASS) {
            s_worker_busy = false;
            ring_net_set_listening(false);
            ui_status("could not start talk task", COL_DIM);
            ESP_LOGE(TAG, "xTaskCreate failed");
        }
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        s_stop = true;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------- credentials */

static void load_api_key(void)
{
    s_api_key[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(QIANWEN_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_api_key);
        if (nvs_get_str(h, QIANWEN_NVS_KEY_KEY, s_api_key, &len) == ESP_OK) {
            ESP_LOGI(TAG, "API key loaded from NVS");
        }
        nvs_close(h);
    }

    if (s_api_key[0] == '\0' && QIANWEN_API_KEY[0] != '\0') {
        snprintf(s_api_key, sizeof(s_api_key), "%s", QIANWEN_API_KEY);
        ESP_LOGW(TAG, "using build-time API key (persisting to NVS)");
        /* Persist so later builds don't need the key on the command line. */
        if (nvs_open(QIANWEN_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_str(h, QIANWEN_NVS_KEY_KEY, s_api_key) == ESP_OK) {
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }

    if (s_api_key[0] == '\0') {
        ESP_LOGE(TAG, "no API key: build with -DQIANWEN_API_KEY=... or set "
                      "NVS '%s'/'%s'", QIANWEN_NVS_NAMESPACE, QIANWEN_NVS_KEY_KEY);
    }
}

/* ------------------------------------------------------------- connect task */

static void connect_task(void *arg)
{
    (void)arg;

    /* 1. Wi-Fi first. The WebRTC session needs the link (and, via SNTP, a
     * wall clock) before it can do anything, so bring the station up and show
     * the link state on the screen before starting the session. */
    ui_button("···", &lv_font_montserrat_48, COL_CONN);

    esp_err_t err = ring_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_wifi_init: %s", esp_err_to_name(err));
    }

    for (;;) {
        char ssid[32] = "";
        ring_wifi_get_credentials(ssid, sizeof(ssid), NULL, 0);

        char line[64];
        snprintf(line, sizeof(line), "connecting to %s…",
                 ssid[0] ? ssid : "Wi-Fi");
        ui_status(line, COL_DIM);

        err = ring_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(err));
            ui_status("no Wi-Fi credentials — retrying", COL_TALK);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        err = ring_wifi_wait_connected(20000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect: %s", esp_err_to_name(err));
            ui_status("Wi-Fi timed out — retrying", COL_TALK);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char ip[16] = "";
        ring_wifi_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip);
        snprintf(line, sizeof(line), "Wi-Fi connected · %s", ip);
        ui_status(line, COL_WAIT);
        break;
    }

    /* 2. WebRTC session. ring_net_start() reuses the station we just brought
     * up (ring_wifi_start() is idempotent). */
    for (;;) {
        if (ring_net_start(QIANWEN_MODEL, s_api_key) == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "connect failed — retry in 5 s");
        ui_status("session failed — retrying", COL_TALK);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    load_api_key();

    /* Board-tuned codec settings, same as 04_voice_loopback. */
    esp_codec_dev_handle_t codec = ring_audio_codec();
    if (codec != NULL) {
        esp_codec_dev_set_in_gain(codec, RING_AUDIO_REC_GAIN_DB);
        esp_codec_dev_set_out_vol(codec, RING_AUDIO_OUT_VOL);
    }
    ring_audio_set_mute(true);

    esp_err_t err = ring_net_init(net_state_cb, net_text_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_net_init: %s", esp_err_to_name(err));
        return err;
    }

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        lv_obj_t *title = ring_ui_add_label(scr, "QIANWEN",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        /* The one big round button: a solid colour-filled circle. */
        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, BTN_R * 2, BTN_R * 2);
        lv_obj_set_pos(s_btn, BTN_CX - BTN_R, BTN_CY - BTN_R);
        lv_obj_set_style_radius(s_btn, BTN_R, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, COL_CONN, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_CONN, 60),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);

        s_btn_label = ring_ui_add_label(s_btn, "···",
                                        &lv_font_montserrat_48,
                                        lv_color_white());
        lv_obj_center(s_btn_label);

        s_status = ring_ui_add_label(scr, "connecting…",
                                     &lv_font_montserrat_16, COL_DIM);
        ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

        s_bar = lv_bar_create(scr);
        lv_obj_set_size(s_bar, BAR_W, BAR_H);
        lv_obj_set_pos(s_bar, RING_CX - BAR_W / 2, BAR_Y);
        lv_bar_set_range(s_bar, 0, 100);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2A2F36), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_bar, RING_COLOR_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(s_bar, 0, LV_PART_INDICATOR);
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s_hint = ring_ui_add_label(scr, "hold = talk · release = reply",
                                   &lv_font_montserrat_14, COL_DIM);
        ring_ui_place_centered(s_hint, RING_CX, HINT_Y);

        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
    }
    s_ui_ready = true;

    if (s_api_key[0] == '\0') {
        ui_status("no API key — see README", COL_TALK);
        return ESP_OK;
    }

    /* Connect in the background so the screen stays responsive. */
    if (xTaskCreate(connect_task, "qw-connect", WORKER_STACK, NULL,
                    WORKER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(connect) failed");
    }
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "06_qianwenai_push_to_talk";
}
