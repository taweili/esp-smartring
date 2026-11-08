/*
 * 06_qianwenai_push_to_talk — STT → LLM → TTS cascade (push-to-talk).
 *
 * Press and hold the green button to record, release to transcribe (STT),
 * think (LLM) and speak the reply (TTS). All three calls are HTTPS on
 * TCP/443 to maas.qianwenaiapi.com — works even on UDP-blocked networks.
 *
 * Three sequential stages:
 *   1. ring_chat_stt()     — mic PCM → text
 *   2. ring_chat_llm_complete() — text → reply
 *   3. ring_chat_tts()     — reply → PCM → speaker (SSE stream)
 *
 * Audio: ring_audio is 48 kHz / 16-bit / stereo interleaved; ring_chat
 * operates on 48 kHz / 16-bit / mono. This file folds/expands at the boundary.
 *
 * Tasks:
 *   qw-connect  8 KB internal — Wi-Fi + SNTP, retry loop
 *   qw-talk     ~14 KB internal — capture + cascade (no codec, no 64 KB PSRAM)
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "06_qianwenai_push_to_talk"

/* ------------------------------------------------------------- config */

#include "ring_secrets.h"

#ifndef QIANWEN_API_KEY
#define QIANWEN_API_KEY ""
#endif

#define STT_MODEL         "qwen3-asr-flash"
#define LLM_MODEL         "qwen3.8-flash"
#define TTS_MODEL         "qwen-audio-3.1-tts-flash"
#define TTS_VOICE         "longanhuan_v3.1"
#define SYSTEM_PROMPT     "You are a helpful assistant. Answer concisely in English."

/* ------------------------------------------------------------- audio */

#define FRAME_SAMPLES      960                          /* 20 ms @ 48 kHz */
#define MIC_FRAME_BYTES    (FRAME_SAMPLES * 2 * 2)      /* stereo 16-bit  */
#define CAP_SEC            10
#define CAP_FRAMES         (CAP_SEC * 1000 / 20)        /* 500 */
#define CAP_SAMPLES_MAX    (CAP_FRAMES * FRAME_SAMPLES)
#define CAP_BYTES_MAX      (CAP_SAMPLES_MAX * 2)        /* mono 16-bit     */

#define LLM_REPLY_SZ       4096
#define STT_TRANSCRIPT_SZ  1024

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK      8192
#define CONNECT_PRIO       5
#define TALK_STACK         16384
#define TALK_PRIO          5

/* ------------------------------------------------------------- UI */

#define TITLE_Y        28
#define STATUS_Y       192
#define STAGE_Y        224
#define TRANSCRIPT_Y   250
#define REPLY_Y        278
#define BAR_CX         180
#define BAR_Y          320
#define BAR_W          220
#define BAR_H           6
#define BTN_CX         180
#define BTN_CY         114
#define BTN_R           70

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_status;
static lv_obj_t *s_stage;
static lv_obj_t *s_transcript_label;
static lv_obj_t *s_reply_label;
static lv_obj_t *s_bar;

static bool s_ui_ready;
static volatile bool s_stop;
static volatile bool s_worker_busy;

/* PSRAM capture buffer: 48 kHz / 16-bit / mono PCM (10 s max). */
static int16_t *s_capture = NULL;
static volatile size_t s_cap_samples;  /* written by capture loop, read by cascade */

static char s_transcript[STT_TRANSCRIPT_SZ];
static char s_reply[LLM_REPLY_SZ];
static char s_api_key[160];

static const char *stage_name(int stage)
{
    switch (stage) {
    case 0: return "idle";
    case 1: return "listening…";
    case 2: return "heard: ";
    case 3: return "thinking…";
    case 4: return "replying…";
    case -1: return "error";
    default: return "?";
    }
}

/* ------------------------------------------------------------- UI */

static void ui_button(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(color, 60), LV_STATE_PRESSED);
    }
}

static void ui_stage(int stage, const char *detail)
{
    char line[64];
    if (stage == 2 && detail) {
        snprintf(line, sizeof(line), "heard: %s", detail);
    } else {
        snprintf(line, sizeof(line), "%s", stage_name(stage));
    }
    lv_color_t col = lv_color_hex(0x9E9E9E);
    if (stage == 2 || stage == 3) col = lv_color_hex(0xFF9800);
    else if (stage == 4) col = lv_color_hex(0x2196F3);
    else if (stage == 1) col = lv_color_hex(0xFF1744);
    else if (stage == 0) col = RING_COLOR_GREEN;
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_stage, line);
        lv_obj_set_style_text_color(s_stage, col, LV_PART_MAIN);
    }
}

static void ui_transcript(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_transcript_label, text);
    }
}

static void ui_reply(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_reply_label, text);
    }
}

static void ui_bar(uint8_t pct, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, color, LV_PART_INDICATOR);
    }
}

static void ui_idle(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, "HOLD\nTO\nTALK");
        lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, RING_COLOR_GREEN, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(RING_COLOR_GREEN, 60), LV_STATE_PRESSED);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, RING_COLOR_GREEN, LV_PART_INDICATOR);
        lv_label_set_text(s_stage, "ready — press & hold to talk");
        lv_obj_set_style_text_color(s_stage, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
        lv_label_set_text(s_transcript_label, "");
        lv_label_set_text(s_reply_label, "");
    }
}

static void ui_error(const char *msg)
{
    ui_button("!", lv_color_hex(0xFF1744));
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_stage, msg ? msg : "error");
        lv_obj_set_style_text_color(s_stage, lv_color_hex(0xFF1744), LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------- audio helpers */

static uint8_t level_pct(const int16_t *pcm, size_t samples)
{
    if (samples == 0) return 0;
    uint32_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        acc += (uint32_t)(v < 0 ? -v : v);
    }
    uint32_t avg = acc / samples;
    if (avg < 80) return 0;
    uint32_t pct = (avg - 80) * 100 / 2000;
    return (uint8_t)(pct > 100 ? 100 : pct);
}

/* TTS audio callback: fold mono to stereo, mute-sequence, write to speaker. */
static void tts_play_cb(const int16_t *mono, size_t samples, void *user)
{
    /* A 20 ms frame at 48 kHz stereo = 3840 bytes. We use a small on-stack
     * buffer; the TTS SSE callback fires on the worker task which has
     * TALK_STACK = 16 KB, so ~4 KB of stack for this is fine. */
    uint8_t stereo[FRAME_SAMPLES * 4];  /* 3840 bytes (960 samples × 4) */
    size_t out_s = samples > FRAME_SAMPLES ? FRAME_SAMPLES : samples;
    ring_audio_mono_to_stereo(mono, (int16_t *)stereo, out_s);
    ring_audio_set_mute(false);
    ring_audio_write(stereo, out_s * 4);
    ring_audio_set_mute(true);
    (void)user;
}

/* ------------------------------------------------------------- talk turn */

static void talk_worker(void *arg)
{
    (void)arg;
    esp_err_t err;

    /* Capture. Read stereo frames from the mic, fold to mono, store in PSRAM. */
    ui_stage(1, NULL);
    ui_button("TALK", lv_color_hex(0xFF1744));
    ui_transcript("");
    ui_reply("");

    uint8_t stereo[MIC_FRAME_BYTES];
    int16_t mono[FRAME_SAMPLES];
    size_t captured = 0;

    while (!s_stop && captured + FRAME_SAMPLES <= CAP_SAMPLES_MAX) {
        err = ring_audio_read(stereo, sizeof(stereo), 50);
        if (err != ESP_OK) continue;

        ring_audio_stereo_to_mono((const int16_t *)stereo, mono, FRAME_SAMPLES);
        memcpy(s_capture + captured, mono, FRAME_SAMPLES * 2);
        captured += FRAME_SAMPLES;

        uint8_t level = level_pct(mono, FRAME_SAMPLES);
        static int frame_cnt = 0;
        if ((frame_cnt++ % 25) == 0) {
            ESP_LOGI(TAG, "capture: %u ms, level %u%%",
                     (unsigned)(captured * 1000 / 48000), (unsigned)level);
        }
        ui_bar(level, lv_color_hex(0xFF1744));
    }

    if (captured == 0) {
        ui_error("empty capture");
        s_worker_busy = false;
        ui_idle();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "capture: %u samples = %u ms",
             (unsigned)captured, (unsigned)(captured * 1000 / 48000));

    /* Stage 2: STT. */
    ui_stage(2, NULL);
    ui_bar(0, lv_color_hex(0xFF9800));
    err = ring_chat_stt(s_capture, captured, s_transcript, sizeof(s_transcript));
    if (err != ESP_OK) {
        ui_error("STT failed");
        s_worker_busy = false;
        ui_idle();
        vTaskDelete(NULL);
        return;
    }
    ui_transcript(s_transcript);

    /* Stage 3: LLM. */
    ui_stage(3, NULL);
    ring_chat_add_user(s_transcript);
    err = ring_chat_llm_complete(s_reply, sizeof(s_reply));
    if (err != ESP_OK) {
        ui_error("LLM failed");
        s_worker_busy = false;
        ui_idle();
        vTaskDelete(NULL);
        return;
    }
    ui_reply(s_reply);

    /* Stage 4: TTS (SSE stream to speaker). */
    ui_stage(4, NULL);
    ui_button("REPLY", lv_color_hex(0x2196F3));
    ring_audio_set_mute(true);
    err = ring_chat_tts(s_reply, tts_play_cb, NULL);
    ring_audio_set_mute(true);
    if (err != ESP_OK) {
        ui_error("TTS failed");
    }

    ESP_LOGI(TAG, "turn complete");
    s_worker_busy = false;
    ui_idle();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- input */

static void btn_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (s_worker_busy || !s_ui_ready) return;
        if (s_capture == NULL) {
            s_capture = heap_caps_malloc(CAP_BYTES_MAX, MALLOC_CAP_SPIRAM);
            if (!s_capture) {
                ui_error("no capture buffer");
                return;
            }
        }
        s_stop = false;
        s_worker_busy = true;
        if (xTaskCreate(talk_worker, "qw-talk", TALK_STACK, NULL,
                        TALK_PRIO, NULL) != pdPASS) {
            s_worker_busy = false;
            ui_error("could not start talk task");
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
    if (ring_secret_get("qianwen", "api_key", QIANWEN_API_KEY,
                        s_api_key, sizeof(s_api_key)) != ESP_OK) {
        ESP_LOGE(TAG, "no API key: build with -DQIANWEN_API_KEY=... or set "
                      "NVS 'qianwen'/'api_key'");
    }
}

/* ------------------------------------------------------------- connect task */

static void connect_task(void *arg)
{
    (void)arg;

    ui_button("···", lv_color_hex(0x3A4048));
    ui_stage(-1, "connecting…");

    /* 1. Wi-Fi. */
    esp_err_t err = ring_wifi_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "ring_wifi_init: %s", esp_err_to_name(err));

    for (;;) {
        char ssid[32] = "";
        ring_wifi_get_credentials(ssid, sizeof(ssid), NULL, 0);

        char line[64];
        snprintf(line, sizeof(line), "connecting to %s…",
                 ssid[0] ? ssid : "Wi-Fi");
        ui_stage(-1, line);

        err = ring_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        err = ring_wifi_wait_connected(20000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect: %s", esp_err_to_name(err));
            ui_stage(-1, "Wi-Fi timed out — retrying");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char ip[16] = "";
        ring_wifi_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip);
        break;
    }

    /* 2. SNTP clock (needed for TLS cert validation). */
    ui_stage(-1, "syncing clock…");
    err = ring_net_sync_clock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clock sync: %s", esp_err_to_name(err));
        /* Non-fatal — continue; HTTPS may or may not work without a clock. */
    }

    /* 3. Initialise ring_chat. */
    ring_chat_cfg_t cfg = {
        .stt_model = STT_MODEL,
        .llm_model = LLM_MODEL,
        .tts_model = TTS_MODEL,
        .voice = TTS_VOICE,
        .system_prompt = SYSTEM_PROMPT,
        .stt_language = NULL,
        .max_history_turns = 10,
    };
    err = ring_chat_init(&cfg, s_api_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_chat_init: %s", esp_err_to_name(err));
        ui_stage(-1, "chat init failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ready");
    ui_idle();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    load_api_key();

    /* Tune audio codec (same defaults as all other examples). */
    esp_codec_dev_handle_t codec = ring_audio_codec();
    if (codec != NULL) {
        esp_codec_dev_set_in_gain(codec, RING_AUDIO_REC_GAIN_DB);
        esp_codec_dev_set_out_vol(codec, RING_AUDIO_OUT_VOL);
    }
    ring_audio_set_mute(true);

    /* Build UI. */
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        lv_obj_t *title = ring_ui_add_label(scr, "QIANWEN",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        /* The big round button. */
        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, BTN_R * 2, BTN_R * 2);
        lv_obj_set_pos(s_btn, BTN_CX - BTN_R, BTN_CY - BTN_R);
        lv_obj_set_style_radius(s_btn, BTN_R, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x3A4048), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(lv_color_hex(0x3A4048), 60),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);

        s_btn_label = ring_ui_add_label(s_btn, "···",
                                        &lv_font_montserrat_20,
                                        lv_color_white());
        lv_obj_center(s_btn_label);

        s_status = ring_ui_add_label(scr, "connecting…",
                                     &lv_font_montserrat_16,
                                     lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

        s_stage = ring_ui_add_label(scr, "connecting…",
                                    &lv_font_montserrat_16,
                                    lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(s_stage, RING_CX, STAGE_Y);

        s_transcript_label = ring_ui_add_label(scr, "",
                                               &lv_font_montserrat_14,
                                               lv_color_hex(0xFF9800));
        lv_obj_set_width(s_transcript_label, 320);
        lv_obj_set_style_text_align(s_transcript_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_transcript_label, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_transcript_label, RING_CX, TRANSCRIPT_Y);

        s_reply_label = ring_ui_add_label(scr, "",
                                          &lv_font_montserrat_14,
                                          lv_color_white());
        lv_obj_set_width(s_reply_label, 320);
        lv_obj_set_style_text_align(s_reply_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_reply_label, RING_CX, REPLY_Y);

        s_bar = lv_bar_create(scr);
        lv_obj_set_size(s_bar, BAR_W, BAR_H);
        lv_obj_set_pos(s_bar, BAR_CX - BAR_W / 2, BAR_Y);
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

        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
    }
    s_ui_ready = true;

    if (s_api_key[0] == '\0') {
        ui_stage(-1, "no API key — see README");
        return ESP_OK;
    }

    if (xTaskCreate(connect_task, "qw-connect", CONNECT_STACK, NULL,
                    CONNECT_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(connect) failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "06_qianwenai_push_to_talk";
}