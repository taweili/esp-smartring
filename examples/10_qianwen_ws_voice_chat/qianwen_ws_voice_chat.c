/*
 * 10_qianwen_ws_voice_chat — WebSocket voice conversation.
 *
 * The simplest voice assistant path: WebSocket over TCP/TLS 443 (the only
 * realtime transport that works on a UDP-blocked network), raw PCM in JSON
 * (no Opus, no DTLS, no SRTP), 16 kHz in / 24 kHz out, resampled to/from
 * the ring's fixed 48 kHz path.
 *
 * By default this app runs in **push-to-talk** mode (a round button on screen —
 * hold to talk, release to hear the reply). Tapping (or server VAD in hands-free
 * mode) triggers a barge-in to interrupt the model's reply.
 *
 * Audio: ring_audio is 48 kHz / 16-bit / stereo interleaved; ring_ws is
 * 48 kHz / 16-bit / mono. This file only does the stereo <-> mono fold.
 *
 * Task structure:
 *   qw-connect  8192 B internal  — Wi-Fi + SNTP + WS session
 *   qw-talk     8192 B internal  — PTT worker (sequential capture + play)
 *                                  or hands-free tasks
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

#define TAG "10_qianwen_ws_voice_chat"

/* ------------------------------------------------------------- config */

#include "ring_secrets.h"

#ifndef QIANWEN_API_KEY
#define QIANWEN_API_KEY ""
#endif

#define QIANWEN_MODEL "qwen-audio-3.1-realtime-flash"

/* ------------------------------------------------------------- audio */

#define MIC_RATE         48000
#define FRAME_SAMPLES    RING_WS_FRAME_SAMPLES     /* 960 = 20 ms */
#define FRAME_STEREO     (FRAME_SAMPLES * 2 * 2)   /* 3840 bytes */
#define OUT_STEREO_MAX   (FRAME_SAMPLES * 2 * 2)

/* Grace after the last played frame before the turn is declared over. Only a
 * safety net: response.done plus an empty downlink ring already mean the reply
 * is finished, so this is short — a long value leaves the button dead after
 * every answer. */
#define TALK_GRACE_MS    500
#define TALK_MAX_MS      15000

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK    8192
#define WORKER_STACK     8192
#define WORKER_PRIO      5

/* ------------------------------------------------------------- UI */

#define TITLE_Y   34
#define BTN_CX    180
#define BTN_CY    160
#define BTN_SZ    148
#define STATUS_Y  268
#define BAR_W    220
#define BAR_H    6
#define BAR_Y    298
#define HINT_Y   328
#define NETINFO_Y 348

#define COL_WAIT   RING_COLOR_GREEN
#define COL_CONN   lv_color_hex(0x3A4048)
#define COL_DIM    lv_color_hex(0x9E9E9E)
#define COL_TALK   lv_color_hex(0xFF1744)
#define COL_WS     lv_color_hex(0x448AFF)

/* ------------------------------------------------------------- state */

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_status;
static lv_obj_t *s_bar;
static lv_obj_t *s_hint;
static lv_obj_t *s_netinfo;

static volatile bool s_stop = false;
static volatile bool s_worker_busy = false;
static bool s_ui_ready = false;

static uint8_t  s_stereo[FRAME_STEREO];
static int16_t  s_mono[FRAME_SAMPLES];
static int16_t  s_reply[FRAME_SAMPLES];
static uint8_t  s_play[OUT_STEREO_MAX];

static char     s_api_key[160];
static char     s_wifi_ssid[32];
static char     s_wifi_ip[16];
static char     s_ws_state[20];

static ring_ws_session_cfg_t s_ws_cfg;

/* ------------------------------------------------------------- UI helpers */

static void ui_button(const char *text, const lv_font_t *font, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_text_font(s_btn_label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_btn_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(s_btn_label, LV_ALIGN_CENTER, 0, 0);
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

static void ui_netinfo(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_netinfo, text);
        lv_obj_set_style_text_color(s_netinfo, color, LV_PART_MAIN);
    }
}

/* Update the bottom status bar with combined Wi-Fi + WebSocket state. */
static void ui_update_netinfo(lv_color_t color)
{
    char line[96];
    if (s_wifi_ip[0]) {
        if (s_ws_state[0]) {
            snprintf(line, sizeof(line), "%s · %s  │ %s",
                     s_wifi_ssid, s_wifi_ip, s_ws_state);
        } else {
            snprintf(line, sizeof(line), "%s · %s",
                     s_wifi_ssid, s_wifi_ip);
        }
    } else {
        snprintf(line, sizeof(line), "%s", s_ws_state[0] ? s_ws_state : "");
    }
    ui_netinfo(line, color);
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

/* ------------------------------------------------------------- ring_ws callbacks */

static void ws_state_cb(ring_ws_state_t st, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    switch (st) {
    case RING_WS_READY: {
        snprintf(s_ws_state, sizeof(s_ws_state), "WS %s",
                 ring_ws_turn_mode() ? ring_ws_turn_mode() : "ptt");
        ui_update_netinfo(COL_WAIT);
        if (!s_worker_busy) {
            char line[64];
            snprintf(line, sizeof(line), "ready (%s) — hold to talk",
                     ring_ws_turn_mode() ? ring_ws_turn_mode() : "ptt");
            ui_idle(line);
        }
        break;
    }
    case RING_WS_LISTENING:
        snprintf(s_ws_state, sizeof(s_ws_state), "listening");
        ui_update_netinfo(COL_TALK);
        ui_button("TALK", &lv_font_montserrat_48, COL_TALK);
        ui_status("listening…", COL_TALK);
        ui_bar(0, COL_TALK);
        break;
    case RING_WS_SPEAKING:
        snprintf(s_ws_state, sizeof(s_ws_state), "reply…");
        ui_update_netinfo(COL_WS);
        ui_button("REPLY", &lv_font_montserrat_48, COL_WS);
        ui_status("replying…", COL_WS);
        ui_bar(100, COL_WS);
        break;
    case RING_WS_CONNECTING:
        snprintf(s_ws_state, sizeof(s_ws_state), "WS…");
        ui_update_netinfo(COL_DIM);
        ui_button("···", &lv_font_montserrat_48, COL_CONN);
        ui_status("connecting…", COL_DIM);
        ui_bar(0, COL_DIM);
        break;
    case RING_WS_ERROR:
        snprintf(s_ws_state, sizeof(s_ws_state), "WS err");
        ui_update_netinfo(COL_TALK);
        if (!s_worker_busy) {
            ui_button("!", &lv_font_montserrat_48, COL_TALK);
            ui_status("connection lost", COL_TALK);
        }
        break;
    case RING_WS_OFF:
    default:
        s_ws_state[0] = '\0';
        break;
    }
}

static void ws_text_cb(const char *text, size_t len, bool is_user, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    static char line[48];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    ui_hint(line);
}

/* ------------------------------------------------------------- level meter */

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
    uint32_t avg = acc / (uint32_t)samples;
    if (avg > 8000) {
        return 100;
    }
    return (uint8_t)(avg * 100 / 8000);
}

/* ------------------------------------------------------------- talk turn (PTT) */

static void turn_finish(void)
{
    ui_hint("");
    s_worker_busy = false;
    char line[64];
    snprintf(line, sizeof(line), "ready (%s) — hold to talk",
             ring_ws_turn_mode() ? ring_ws_turn_mode() : "ptt");
    ui_idle(line);
}

/* Downlink: play the reply until response.done + buffer drained. */
static void play_reply(void)
{
    /* The codec is unmuted once, just before the first frame, and muted again
     * when the reply is over. Toggling mute around every 20 ms buffer is an
     * I2C write per frame and cuts the I2S DMA queue mid-buffer (clicks and
     * dropped syllables). */
    ring_audio_set_mute(true);
    bool unmuted = false;
    /* The grace timer must not run before the first audio frame: the server
     * can take >500 ms to start the reply after commit (measured 373 ms on
     * the bench), and the state is still READY until response.created. Without
     * this guard the loop would declare the turn over and never play the
     * reply. */
    bool got_audio = false;
    TickType_t last = xTaskGetTickCount();
    TickType_t start = last;

    for (;;) {
        size_t got = 0;
        if (ring_ws_read_audio(s_reply, FRAME_SAMPLES, &got, 200) == ESP_OK &&
            got > 0) {
            got_audio = true;
            ring_audio_mono_to_stereo(s_reply, (int16_t *)s_play, got);
            size_t bytes = got * 4;
            last = xTaskGetTickCount();
            if (!unmuted) {
                ring_audio_set_mute(false);
                unmuted = true;
            }
            if (ring_audio_write(s_play, bytes) != ESP_OK) {
                ESP_LOGW(TAG, "speaker write failed");
            }
            ui_bar(level_of(s_reply, got), COL_WS);
        } else if (got_audio &&
                   ring_ws_state() == RING_WS_READY &&
                   !ring_ws_playback_pending() &&
                   (xTaskGetTickCount() - last) > pdMS_TO_TICKS(TALK_GRACE_MS)) {
            break;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(TALK_MAX_MS)) {
            ESP_LOGW(TAG, "reply cap reached");
            break;
        }
    }
    ring_audio_set_mute(true);

    ring_ws_stats_t st;
    if (ring_ws_get_stats(&st) == ESP_OK) {
        ESP_LOGI(TAG, "turn done: up=%u B down=%u B turns=%u "
                      "dropped up/down=%u/%u ring_high=%u ms "
                      "stack_hwm=%u B",
                 (unsigned)st.bytes_up, (unsigned)st.bytes_down,
                 (unsigned)st.turns, (unsigned)st.dropped_up,
                 (unsigned)st.dropped_down, (unsigned)st.ring_high_ms,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void talk_worker(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "turn: capture");
    ui_hint("");

    /* Uplink: one 20 ms frame at a time until released. */
    uint32_t frames = 0;
    while (!s_stop && ring_ws_state() == RING_WS_LISTENING) {
        if (ring_audio_read(s_stereo, sizeof(s_stereo), 200) != ESP_OK) {
            ESP_LOGW(TAG, "mic read failed");
            break;
        }
        ring_audio_stereo_to_mono((const int16_t *)s_stereo, s_mono,
                                   FRAME_SAMPLES);
        esp_err_t serr = ring_ws_send_audio(s_mono, FRAME_SAMPLES);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "send audio failed: %s", esp_err_to_name(serr));
            break;
        }
        if ((frames++ % 50) == 0) {
            ESP_LOGI(TAG, "uplink frame %u, mic level %u",
                     (unsigned)frames, level_of(s_mono, FRAME_SAMPLES));
        }
        ui_bar(level_of(s_mono, FRAME_SAMPLES), COL_TALK);
    }

    ESP_LOGI(TAG, "turn: release");
    ring_ws_set_listening(false);

    /* Commit + create response. Show the wait state while the server thinks;
     * the reply can take a few hundred ms to start. */
    ui_button("WAIT", &lv_font_montserrat_48, COL_TALK);
    ui_status("waiting…", COL_TALK);
    ring_ws_commit();

    play_reply();

    ESP_LOGI(TAG, "turn: done");
    turn_finish();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- self-test
 *
 * Enabled by the two commented lines in this example's CMakeLists.txt. Streams
 * an embedded speech clip through one complete turn on boot, so the whole
 * path (resampler → base64 → WS → session → reply → speaker) can be verified
 * on the bench without a finger on the button and without a live speaker.
 */
#ifdef QW_SELFTEST
extern const uint8_t qw_selftest_pcm_start[]
    asm("_binary_qw_selftest_pcm_start");
extern const uint8_t qw_selftest_pcm_end[]
    asm("_binary_qw_selftest_pcm_end");

static void selftest_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < 300 && ring_ws_state() != RING_WS_READY; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ring_ws_state() != RING_WS_READY) {
        ESP_LOGE(TAG, "selftest: session never became ready");
        vTaskDelete(NULL);
        return;
    }

    const int16_t *pcm = (const int16_t *)qw_selftest_pcm_start;
    size_t samples = (size_t)(qw_selftest_pcm_end - qw_selftest_pcm_start) / 2;
    ESP_LOGI(TAG, "selftest: streaming %u ms of 48 kHz PCM",
             (unsigned)(samples / (MIC_RATE / 1000)));

    s_worker_busy = true;
    if (ring_ws_set_listening(true) != ESP_OK) {
        ESP_LOGE(TAG, "selftest: set_listening failed");
        turn_finish();
        vTaskDelete(NULL);
        return;
    }

    for (size_t i = 0; i + FRAME_SAMPLES <= samples; i += FRAME_SAMPLES) {
        if (ring_ws_send_audio(pcm + i, FRAME_SAMPLES) != ESP_OK) {
            ESP_LOGW(TAG, "selftest: frame %u not sent",
                     (unsigned)(i / FRAME_SAMPLES));
        }
        vTaskDelay(pdMS_TO_TICKS(20));   /* real-time pacing, like the mic */
    }

    ring_ws_set_listening(false);
    ring_ws_commit();
    play_reply();

    ESP_LOGI(TAG, "selftest: done");
    turn_finish();
    vTaskDelete(NULL);
}
#endif /* QW_SELFTEST */

/* ------------------------------------------------------------- input */

static void btn_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (!s_ui_ready) {
            return;
        }
        /* Barge-in: a tap while the model is replying cancels it. */
        if (ring_ws_state() == RING_WS_SPEAKING || ring_ws_is_speaking()) {
            ESP_LOGI(TAG, "barge-in: tap");
            ring_ws_cancel();
            return;
        }
        if (s_worker_busy) {
            return;
        }
        if (ring_ws_state() != RING_WS_READY) {
            ui_status("not ready yet", COL_DIM);
            return;
        }
        s_stop = false;
        s_worker_busy = true;
        if (ring_ws_set_listening(true) != ESP_OK) {
            s_worker_busy = false;
            return;
        }
        if (xTaskCreate(talk_worker, "qw-talk", WORKER_STACK, NULL,
                        WORKER_PRIO, NULL) != pdPASS) {
            s_worker_busy = false;
            ring_ws_set_listening(false);
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
    if (ring_secret_get("qianwen", "api_key", QIANWEN_API_KEY,
                        s_api_key, sizeof(s_api_key)) != ESP_OK) {
        ESP_LOGE(TAG, "no API key: set QIANWEN_API_KEY");
    }
}

/* ------------------------------------------------------------- connect task */

static void connect_task(void *arg)
{
    (void)arg;

    ui_button("···", &lv_font_montserrat_48, COL_CONN);

    esp_err_t err = ring_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_wifi_init: %s", esp_err_to_name(err));
    }

    for (;;) {
        ui_netinfo("Wi-Fi: scanning…", COL_DIM);
        ui_status("connecting to Wi-Fi…", COL_DIM);
        err = ring_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(err));
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
        ring_wifi_get_ip(s_wifi_ip, sizeof(s_wifi_ip));
        ring_wifi_get_credentials(s_wifi_ssid, sizeof(s_wifi_ssid), NULL, 0);
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", s_wifi_ip);
        ui_update_netinfo(COL_WAIT);
        break;
    }

    snprintf(s_ws_state, sizeof(s_ws_state), "WS…");
    ui_update_netinfo(COL_DIM);
    ui_status("connecting to model…", COL_DIM);

    /* session config: PTT mode for first implementation */
    memset(&s_ws_cfg, 0, sizeof(s_ws_cfg));
    s_ws_cfg.voice = "longanqian_v3.1";
    s_ws_cfg.turn_detection = NULL;   /* PTT */
    s_ws_cfg.speech_rate = 0.0f;
    s_ws_cfg.max_history_turns = 20;

    for (;;) {
        if (ring_ws_start(QIANWEN_MODEL, s_api_key, &s_ws_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "session up — supervising");
            /* Stay alive: a successful start used to end this task, so a
             * later disconnect left the UI on "!" forever. Reconnect when
             * the session drops. */
            while (ring_ws_state() != RING_WS_ERROR &&
                   ring_ws_state() != RING_WS_OFF) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            /* Let an in-flight turn finish before tearing the client down. */
            while (s_worker_busy) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGW(TAG, "session lost — reconnecting");
            ui_status("session lost — reconnecting", COL_TALK);
            ring_ws_stop();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGW(TAG, "connect failed — retry in 5 s");
        ui_status("session failed — retrying", COL_TALK);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    load_api_key();

    esp_codec_dev_handle_t codec = ring_audio_codec();
    if (codec != NULL) {
        esp_codec_dev_set_in_gain(codec, RING_AUDIO_REC_GAIN_DB);
        esp_codec_dev_set_out_vol(codec, RING_AUDIO_OUT_VOL);
    }
    ring_audio_set_mute(true);

    esp_err_t err = ring_ws_init(ws_state_cb, ws_text_cb, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_ws_init: %s", esp_err_to_name(err));
        return err;
    }

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        lv_obj_t *title = ring_ui_add_label(scr, "QIANWEN WS",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, BTN_SZ, BTN_SZ);
        lv_obj_set_pos(s_btn, BTN_CX - BTN_SZ / 2, BTN_CY - BTN_SZ / 2);
        /* The button is round (README/design): a 148 px square with radius
         * 0 reads as a tile, not a talk button. */
        lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
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

        s_netinfo = ring_ui_add_label(scr, "",
                                      &lv_font_montserrat_14, COL_DIM);
        ring_ui_place_centered(s_netinfo, RING_CX, NETINFO_Y);

        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
    }
    s_ui_ready = true;

    if (s_api_key[0] == '\0') {
        ui_status("no API key — see README", COL_TALK);
        return ESP_OK;
    }

    if (xTaskCreate(connect_task, "qw-connect", CONNECT_STACK, NULL,
                    WORKER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(connect) failed");
    }

#ifdef QW_SELFTEST
    if (xTaskCreate(selftest_task, "qw-selftest", WORKER_STACK, NULL,
                    WORKER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(selftest) failed");
    }
#endif
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "10_qianwen_ws_voice_chat";
}