/*
 * 07_qianwen_realtime_harness — hands-free Qianwen realtime voice loop.
 *
 * A reference example for the complete speech-to-speech path on the ring:
 *
 *   board init -> Wi-Fi -> SNTP -> HTTPS probe -> WebRTC session
 *   -> [ mic -> mono -> Opus -> Qianwen -> Opus -> speaker ] -> repeat
 *
 * The network gate is explicit: the audio loop is not entered until the
 * station has an IP, the wall clock is valid (TLS certificate checks need it)
 * and a real HTTPS GET has succeeded. Each step is shown on the screen and
 * retried with backoff, so a failure is diagnosable instead of silent.
 *
 * Interaction is hands-free and half-duplex. The uplink stays open and the
 * server VAD ("server_vad") detects the end of the user's speech; the first
 * reply frame flips ring_net to RING_NET_SPEAKING, which closes the uplink so
 * the model never hears itself. The ring has no acoustic echo cancellation
 * (the ES8311 has no AEC and the speaker sits next to the mic), so a
 * full-duplex version would need an AEC feed — see notes/networking-https-webrtc.md §5.2.
 *
 * Audio: ring_audio is 48 kHz / 16-bit / stereo interleaved; ring_net is
 * 48 kHz / 16-bit / mono. This file only folds/expands at the boundary.
 *
 * Task layout:
 *   qw-connect  32 KB, internal  — Wi-Fi + SNTP + probe + SDP session, retries
 *   qw-turn    256 KB, PSRAM     — the audio loop (Opus needs > 96 KB of stack;
 *                                   FreeRTOS stacks are internal-RAM-only, so
 *                                   this one is allocated with the WithCaps API)
 *   ring_net's own peer / signal / event tasks do the WebRTC work.
 */

#include "ring_example.h"

#include "ringkit/ringkit.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "07_qianwen_realtime_harness"

/* ------------------------------------------------------------- config */

/* Secrets are generated into ring_secrets.h by main/CMakeLists.txt from the
 * environment (tools/dev.sh sources secrets.local). Never committed. */
#include "ring_secrets.h"

#ifndef QIANWEN_API_KEY
#define QIANWEN_API_KEY ""
#endif

#define QIANWEN_MODEL          "qwen-audio-3.1-realtime-flash"

/* Well-known public host: exercises DNS + routing + TLS + HTTP. */
#define NET_PROBE_URL          "https://baidu.com"

/* ------------------------------------------------------------- audio */

#define FRAME_SAMPLES   RING_NET_FRAME_SAMPLES          /* 960 = 20 ms       */
#define MIC_FRAME_BYTES (FRAME_SAMPLES * 2 * 2)        /* stereo 16-bit     */
#define OUT_STEREO_MAX  (FRAME_SAMPLES * 2 * 2)        /* 20 ms stereo      */

#define LISTEN_MAX_MS   15000   /* cap one uplink if the VAD never fires  */
#define REPLY_MAX_MS    15000   /* cap one reply                         */
#define REPLY_GRACE_MS  3000    /* drain the ring after the last frame   */
#define SILENCE_TAIL_MS 1000    /* keep RTP alive so server VAD sees end */
#define TURN_GAP_MS     1000    /* pause between turns                   */
#define RETRY_MS        5000    /* backoff after a network/session error */

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK   8192    /* words; 32 KB — the probe runs TLS here  */
#define CONNECT_PRIO    5
#define TURN_STACK      131072  /* bytes; 128 KB in PSRAM (stereo Opus)    */
#define TURN_PRIO       5

/* ------------------------------------------------------------- UI */

#define TITLE_Y        26
#define STATUS_Y       58
#define WIFI_Y         82
#define MIC_BAR_Y      238
#define MIC_LABEL_Y    252
#define USER_Y         274
#define TRANSCRIPT_Y   294
#define HINT_Y         340

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_status;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_mic_bar;
static lv_obj_t *s_mic_label;
static lv_obj_t *s_user_label;
static lv_obj_t *s_transcript_label;

static char s_transcript[256];
static char s_user_text[128];
static char s_api_key[160];

/* ring_net's event task appends to s_transcript / s_user_text while the turn
 * task clears them; serialise the two. */
static SemaphoreHandle_t s_text_mutex;
static bool s_ui_ready;
static volatile bool s_stop;
static volatile bool s_worker_busy;

/* ------------------------------------------------------------- audio helpers
 *
 * The stereo<->mono fold lives in ringkit (`ring_audio_stereo_to_mono` /
 * `ring_audio_mono_to_stereo`); only the level meter is app-specific.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------- UI helpers */

/* Average absolute sample -> 0..100%. The ring's silence floor is ~50;
 * speech lands in the high hundreds / low thousands. */
static uint8_t mic_level_percent(const int16_t *pcm, size_t samples)
{
    uint32_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        acc += (uint32_t)(v < 0 ? -v : v);
    }
    uint32_t avg = acc / samples;
    if (avg < 80) {
        return 0;
    }
    uint32_t pct = (avg - 80) * 100 / 2000;
    return (uint8_t)(pct > 100 ? 100 : pct);
}

static void ui_mic_level(uint8_t pct)
{
    RING_UI_LOCKED(-1) {
        lv_bar_set_value(s_mic_bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_mic_label, "mic %u%%", (unsigned)pct);
    }
}

static void ui_button(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_bg_color(s_btn, color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_darken(color, 60),
                                  LV_STATE_PRESSED);
    }
}

static void ui_status(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, color, LV_PART_MAIN);
    }
}

static const char *wifi_state_name(ring_wifi_state_t st)
{
    switch (st) {
    case RING_WIFI_OFF:          return "off";
    case RING_WIFI_DISCONNECTED: return "disconnected";
    case RING_WIFI_CONNECTING:   return "connecting";
    case RING_WIFI_CONNECTED:    return "connected";
    }
    return "?";
}

static void ui_wifi(void)
{
    char ip[16] = "";
    char line[48];
    ring_wifi_get_ip(ip, sizeof(ip));
    snprintf(line, sizeof(line), "Wi-Fi: %s %s",
             wifi_state_name(ring_wifi_state()), ip);
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_wifi_label, line);
    }
}

static void ui_transcript(void)
{
    xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_user_label, s_user_text);
        lv_label_set_text(s_transcript_label, s_transcript);
    }
    xSemaphoreGive(s_text_mutex);
}

static void transcript_append(const char *text, size_t len)
{
    xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    size_t n = sizeof(s_transcript) - 1 - strlen(s_transcript);
    if (len < n) {
        n = len;
    }
    if (n > 0) {
        strncat(s_transcript, text, n);
    }
    xSemaphoreGive(s_text_mutex);
}

static void transcript_set_user(const char *text, size_t len)
{
    xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    size_t n = len < sizeof(s_user_text) - 1 ? len : sizeof(s_user_text) - 1;
    memcpy(s_user_text, text, n);
    s_user_text[n] = '\0';
    xSemaphoreGive(s_text_mutex);
}

static void transcript_clear(void)
{
    xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    s_transcript[0] = '\0';
    s_user_text[0] = '\0';
    xSemaphoreGive(s_text_mutex);
}

static void transcript_log(void)
{
    xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "you: %s", s_user_text);
    ESP_LOGI(TAG, "transcript: %s", s_transcript);
    xSemaphoreGive(s_text_mutex);
}

/* ------------------------------------------------------------- net glue */

static void net_state_cb(ring_net_state_t st, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    const char *text = ring_net_state_name(st);
    lv_color_t color = lv_color_hex(0x9E9E9E);
    switch (st) {
    case RING_NET_READY:
        text = "ready — hold to talk";
        color = RING_COLOR_GREEN;
        if (!s_worker_busy) ui_button("HOLD\nTO TALK", RING_COLOR_GREEN);
        break;
    case RING_NET_LISTENING:
        text = "listening — release to send";
        color = lv_color_hex(0xFF1744);
        ui_button("RELEASE\nTO SEND", color);
        break;
    case RING_NET_SPEAKING:
        text = "assistant speaking…";
        color = lv_color_hex(0xFF9800);
        ui_button("PLAYING", color);
        break;
    case RING_NET_ERROR:
        text = "connection lost — reconnecting";
        color = lv_color_hex(0xFF1744);
        ui_button("...", color);
        break;
    default: break;
    }
    ui_status(text, color);
}

static void net_text_cb(const char *text, size_t len, bool is_user, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    if (is_user) {
        transcript_set_user(text, len);
    } else {
        transcript_append(text, len);
    }
    ui_transcript();
}

/* ------------------------------------------------------------- turn loop */

static void turn_task(void *arg)
{
    (void)arg;
    uint8_t stereo[MIC_FRAME_BYTES];
    int16_t mono[FRAME_SAMPLES];
    int frame_count = 0;

    ESP_LOGI(TAG, "turn: capture");
    while (!s_stop && ring_net_state() == RING_NET_LISTENING) {
        if (ring_audio_read(stereo, sizeof(stereo), 50) != ESP_OK) {
            ESP_LOGW(TAG, "mic read failed");
            continue;
        }
        ring_audio_stereo_to_mono((const int16_t *)stereo, mono,
                                   FRAME_SAMPLES);
        uint8_t level = mic_level_percent(mono, FRAME_SAMPLES);
        if ((frame_count++ % 10) == 0) {
            ESP_LOGI(TAG, "mic=%u%%, sent=%d frames", level, frame_count);
            ui_mic_level(level);
        }
        if (ring_net_send_audio(mono, FRAME_SAMPLES) != ESP_OK) {
            ESP_LOGW(TAG, "send audio failed");
            break;
        }
    }
    /* Stopping RTP immediately is not enough to signal end-of-turn to the
     * WebRTC endpoint. Send real zero-valued PCM for a short tail so Qianwen's
     * server VAD observes silence and emits the transcription/response. */
    memset(mono, 0, sizeof(mono));
    ESP_LOGI(TAG, "turn: sending %d ms silence tail", SILENCE_TAIL_MS);
    for (int i = 0; i < SILENCE_TAIL_MS / 20 &&
                ring_net_state() == RING_NET_LISTENING; i++) {
        if (ring_net_send_audio(mono, FRAME_SAMPLES) != ESP_OK) {
            ESP_LOGW(TAG, "silence tail send failed at frame %d", i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ring_net_set_listening(false);
    ui_mic_level(0);
    ui_status("waiting for assistant…", lv_color_hex(0xFF9800));

    size_t played = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t last = start;
    for (;;) {
        size_t got = 0;
        if (ring_net_read_audio(mono, FRAME_SAMPLES, &got, 100) == ESP_OK && got > 0) {
            ring_audio_mono_to_stereo(mono, (int16_t *)stereo, got);
            ring_audio_set_mute(false);
            ring_audio_write(stereo, got * 4);
            ring_audio_set_mute(true);
            played += got;
            last = xTaskGetTickCount();
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        if ((ring_net_state() == RING_NET_READY && played > 0) ||
            (now - last) > pdMS_TO_TICKS(REPLY_GRACE_MS) ||
            (now - start) > pdMS_TO_TICKS(REPLY_MAX_MS)) break;
    }
    ring_audio_set_mute(true);
    ESP_LOGI(TAG, "turn: done (%u samples)", (unsigned)played);
    transcript_log();
    s_worker_busy = false;
    ui_status("ready — hold to talk", RING_COLOR_GREEN);
    ui_button("HOLD\nTO TALK", RING_COLOR_GREEN);
    vTaskDeleteWithCaps(NULL);
}

static void button_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (s_worker_busy || ring_net_state() != RING_NET_READY) return;
        s_stop = false;
        s_worker_busy = true;
        transcript_clear();
        ui_transcript();
        if (ring_net_set_listening(true) != ESP_OK ||
            xTaskCreateWithCaps(turn_task, "qw-turn", TURN_STACK, NULL,
                                TURN_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_worker_busy = false;
            ring_net_set_listening(false);
            ui_status("could not start recording", lv_color_hex(0xFF1744));
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_stop = true;
    }
}

/* ------------------------------------------------------------- connect */

static void connect_task(void *arg)
{
    (void)arg;

    /* 1. Wi-Fi. */
    esp_err_t werr = ring_wifi_init();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "ring_wifi_init: %s", esp_err_to_name(werr));
    }
    ui_status("wi-fi…", lv_color_hex(0x9E9E9E));
    for (;;) {
        if (ring_wifi_start() != ESP_OK) {
            ESP_LOGW(TAG, "wifi start failed");
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        if (ring_wifi_wait_connected(20000) != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect timed out");
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        char ip[16] = "";
        ring_wifi_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip);
        ui_wifi();
        break;
    }

    /* 2. Networking gate: a valid clock (TLS) and a real HTTPS round trip. */
    ui_status("network check…", lv_color_hex(0x9E9E9E));
    for (;;) {
        int probe = ring_net_probe(NET_PROBE_URL);
        if (ring_net_sync_clock() == ESP_OK && probe >= 200 && probe < 300) {
            break;
        }
        ESP_LOGW(TAG, "network not ready — retry in %d s", RETRY_MS / 1000);
        ui_status("network check failed", lv_color_hex(0xFF1744));
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }

    /* 3. WebRTC session. Keep this task alive after the first connection:
     * the peer can drop later, and the turn task deliberately does not own
     * session setup or teardown. */
    for (;;) {
        ui_status("session…", lv_color_hex(0x9E9E9E));
        while (ring_net_start(QIANWEN_MODEL, s_api_key) != ESP_OK) {
            ESP_LOGW(TAG, "session failed — retry in %d s", RETRY_MS / 1000);
            ui_status("session failed", lv_color_hex(0xFF1744));
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }

        ESP_LOGI(TAG, "session monitor: connected");
        while (ring_net_state() == RING_NET_READY ||
               ring_net_state() == RING_NET_LISTENING ||
               ring_net_state() == RING_NET_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        ESP_LOGW(TAG, "session monitor: state %s; reconnecting",
                 ring_net_state_name(ring_net_state()));
        ui_status("connection lost — retrying", lv_color_hex(0xFF1744));
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
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

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    s_text_mutex = xSemaphoreCreateMutex();
    if (s_text_mutex == NULL) {
        ESP_LOGE(TAG, "could not create transcript mutex");
        return ESP_ERR_NO_MEM;
    }

    load_api_key();

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

        lv_obj_t *title = ring_ui_add_label(scr, "QIANWEN VOICE",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        s_status = ring_ui_add_label(scr, "connecting…",
                                     &lv_font_montserrat_24,
                                     lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, 132, 96);
        lv_obj_set_pos(s_btn, RING_CX - 66, 126);
        lv_obj_set_style_radius(s_btn, 24, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x3A4048), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);
        s_btn_label = ring_ui_add_label(s_btn, "...",
                                        &lv_font_montserrat_20,
                                        lv_color_white());
        lv_obj_center(s_btn_label);
        lv_obj_add_event_cb(s_btn, button_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn, button_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn, button_cb, LV_EVENT_PRESS_LOST, NULL);

        s_wifi_label = ring_ui_add_label(scr, "Wi-Fi: …",
                                         &lv_font_montserrat_14,
                                         lv_color_hex(0x9E9E9E));
        lv_obj_set_width(s_wifi_label, 320);
        lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        ring_ui_place_centered(s_wifi_label, RING_CX, WIFI_Y);

        s_mic_bar = lv_bar_create(scr);
        lv_obj_set_size(s_mic_bar, 240, 10);
        lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(0x2A2A2A),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_mic_bar, RING_COLOR_GREEN,
                                  LV_PART_INDICATOR);
        lv_bar_set_range(s_mic_bar, 0, 100);
        lv_bar_set_value(s_mic_bar, 0, LV_ANIM_OFF);
        ring_ui_place_centered(s_mic_bar, RING_CX, MIC_BAR_Y);

        s_mic_label = ring_ui_add_label(scr, "mic 0%",
                                        &lv_font_montserrat_14,
                                        lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(s_mic_label, RING_CX, MIC_LABEL_Y);

        s_user_label = ring_ui_add_label(scr, "",
                                         &lv_font_montserrat_14,
                                         RING_COLOR_GREEN);
        lv_obj_set_width(s_user_label, 320);
        lv_obj_set_style_text_align(s_user_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_label_set_long_mode(s_user_label, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_user_label, RING_CX, USER_Y);

        s_transcript_label = ring_ui_add_label(scr, "",
                                         &lv_font_montserrat_14,
                                         lv_color_white());
        lv_obj_set_width(s_transcript_label, 320);
        lv_obj_set_style_max_height(s_transcript_label, 100, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_transcript_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_label_set_long_mode(s_transcript_label, LV_LABEL_LONG_WRAP);
        ring_ui_place_centered(s_transcript_label, RING_CX, TRANSCRIPT_Y);

        lv_obj_t *hint = ring_ui_add_label(scr, "hold button to talk · release to send",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(hint, RING_CX, HINT_Y);
    }
    s_ui_ready = true;

    if (s_api_key[0] == '\0') {
        ui_status("no API key", lv_color_hex(0xFF1744));
        return ESP_OK;
    }

    if (xTaskCreate(connect_task, "qw-connect", CONNECT_STACK, NULL,
                    CONNECT_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not create connect task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "07_qianwen_realtime_harness";
}
