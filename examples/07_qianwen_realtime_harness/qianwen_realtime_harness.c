/*
 * 07_qianwen_realtime_harness — minimal WebRTC realtime test harness for the Qianwen
 * model.
 *
 * Unlike examples/06_qianwenai_push_to_talk (a push-to-talk UI), this example is a
 * hands-free loopback probe: it brings up Wi-Fi + the WebRTC session, then
 * automatically runs talk turns — record from the mic, stream Opus to the
 * model, play the spoken reply, and log the transcript — over and over.
 *
 * It exists to answer one question: does the model interaction actually work
 * end to end? Every step is logged:
 *
 *   ring_net: state -> ready
 *   07_qianwen_realtime_harness: turn: start
 *   07_qianwen_realtime_harness: turn: uplink done (N frames)
 *   ring_net: dc: {"type":"conversation.item.input_audio_transcription.completed",...}
 *   ring_net: dc: {"type":"response.audio_transcript.delta",...}
 *   07_qianwen_realtime_harness: turn: reply done (M samples, K frames)
 *   07_qianwen_realtime_harness: you: the capital of China
 *   07_qianwen_realtime_harness: transcript: ...
 *
 * Audio is the same 48 kHz / 16-bit / stereo ES8311 stream as the other
 * examples; ring_net wants 48 kHz / 16-bit / mono in 20 ms frames, so this
 * file only does the mono -> stereo fold for playback and the turn
 * bookkeeping.
 *
 * The talk task stack is large (24 KB) because the Opus encoder's SILK path
 * peaks at ~20 KB; see examples/06_qianwenai_push_to_talk/README.md §4.
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

#define TAG "07_qianwen_realtime_harness"

/* ------------------------------------------------------------- test clip */

/* Embedded known speech: "the capital of China" (espeak -> 48 kHz / 16-bit
 * mono PCM). Generated with:
 *   espeak -v en -s 150 -w capital.wav "the capital of China"
 *   ffmpeg -y -i capital.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
 *           examples/07_qianwen_realtime_harness/assets/test_clip.pcm
 */
/* Required embed (see CMakeLists.txt). A weak declaration would let
 * --gc-sections discard the file because the only reference is weak, so the
 * clip is deliberately a hard link-time dependency: no clip, no build. */
extern const uint8_t test_clip_pcm_start[] asm("_binary_test_clip_pcm_start");
extern const uint8_t test_clip_pcm_end[]   asm("_binary_test_clip_pcm_end");

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

#define MIC_RATE        48000
#define FRAME_SAMPLES   RING_NET_FRAME_SAMPLES        /* 960 = 20 ms      */
#define OUT_STEREO_MAX  (MIC_RATE * 2 * 2 / 1000 * 40) /* 40 ms = 15360 B */

#define TALK_GRACE_MS   3000    /* stop waiting this long after audio */
#define REPLY_MAX_MS    15000   /* cap on one reply                   */
#define TURN_GAP_MS     5000    /* pause between turns                */

/* ------------------------------------------------------------- task */

#define WORKER_STACK    8192
/* The Opus encoder's SILK path peaks at ~20 KB for mono and more for the
 * stereo frames the SDP negotiates (see RING_NET_OPUS_CHANNELS in ring_net.c);
 * 24 KB overflowed on the first encoded frame once the encoder went stereo. */
#define TALK_TASK_STACK 65536
#define WORKER_PRIO     5       /* below LVGL (6): button events win */

/* ------------------------------------------------------------- UI */

#define TITLE_Y        26
#define STATUS_Y       58
#define WIFI_Y         82
#define USER_Y         104
#define TRANSCRIPT_Y   150
#define HINT_Y         330

static lv_obj_t *s_status;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_user_label;
static lv_obj_t *s_transcript_label;

static char s_transcript[256];
static char s_user_text[128];
static char s_api_key[160];

/* The ring_net event task appends to s_transcript / s_user_text while
 * talk_task clears and logs them; serialise the two. */
static SemaphoreHandle_t s_text_mutex;
static bool s_ui_ready;

/* ------------------------------------------------------------- helpers */

static int16_t s_mono[FRAME_SAMPLES];
static int16_t s_reply[FRAME_SAMPLES];
static uint8_t s_play[OUT_STEREO_MAX];

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

/* ------------------------------------------------------------- UI helpers */

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
    case RING_WIFI_OFF:         return "off";
    case RING_WIFI_DISCONNECTED: return "disconnected";
    case RING_WIFI_CONNECTING:  return "connecting";
    case RING_WIFI_CONNECTED:   return "connected";
    }
    return "?";
}

/* Link state + address, refreshed once per talk turn. */
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

/* Append one model-output delta. Called from the ring_net event task. */
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

/* Replace the recognised user utterance. */
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
    const char *name = ring_net_state_name(st);
    lv_color_t color = lv_color_hex(0x9E9E9E);
    switch (st) {
    case RING_NET_READY:     color = RING_COLOR_GREEN; break;
    case RING_NET_LISTENING: color = lv_color_hex(0xFF1744); break;
    case RING_NET_SPEAKING:  color = lv_color_hex(0xFF9800); break;
    case RING_NET_ERROR:     color = lv_color_hex(0xFF1744); break;
    default: break;
    }
    ui_status(name, color);
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

/* ------------------------------------------------------------- talk turn */

/* Send the embedded test clip to the model, frame by frame, and play it
 * through the speaker so the phrase is audible while it is being sent. */
static uint32_t send_test_clip(void)
{
    const int16_t *clip = (const int16_t *)test_clip_pcm_start;
    size_t clip_bytes = (size_t)(test_clip_pcm_end - test_clip_pcm_start);
    size_t clip_samples = clip_bytes / sizeof(int16_t);
    size_t frames = (clip_samples + FRAME_SAMPLES - 1) / FRAME_SAMPLES;

    ESP_LOGI(TAG, "sending test clip: %u samples (%u frames)",
             (unsigned)clip_samples, (unsigned)frames);

    uint32_t sent = 0;
    for (size_t f = 0; f < frames; f++) {
        size_t off = f * FRAME_SAMPLES;
        size_t n = clip_samples - off;
        if (n > FRAME_SAMPLES) {
            n = FRAME_SAMPLES;
        }
        memset(s_mono, 0, sizeof(s_mono));
        memcpy(s_mono, clip + off, n * sizeof(int16_t));

        if (ring_net_send_audio(s_mono, FRAME_SAMPLES) != ESP_OK) {
            ESP_LOGW(TAG, "send clip frame failed");
            return sent;
        }
        sent++;
        size_t bytes = mono_to_stereo(s_mono, FRAME_SAMPLES, s_play);
        ring_audio_set_mute(false);
        if (ring_audio_write(s_play, bytes) != ESP_OK) {
            ESP_LOGW(TAG, "speaker write failed");
        }
        ring_audio_set_mute(true);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return sent;
}

static void talk_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Wait for the session to be ready. */
        while (ring_net_state() != RING_NET_READY) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "turn: start");
        ui_wifi();
        transcript_clear();
        ui_transcript();

        if (ring_net_set_listening(true) != ESP_OK) {
            ESP_LOGW(TAG, "could not start listening");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Uplink: the embedded clip, frame by frame. */
        uint32_t frames = send_test_clip();
        ESP_LOGI(TAG, "turn: uplink done (%u frames)", (unsigned)frames);
        ring_net_set_listening(false);

        /* Downlink: Opus -> PCM -> speaker until the reply is done. */
        size_t played = 0;
        TickType_t last = xTaskGetTickCount();
        TickType_t dl_start = last;
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
                played += got;
            } else if (played > 0 &&
                       ring_net_state() == RING_NET_READY &&
                       (xTaskGetTickCount() - last) > pdMS_TO_TICKS(TALK_GRACE_MS)) {
                /* The grace only starts once a reply has arrived: until then
                 * REPLY_MAX_MS is the only cap, so a slow first token does not
                 * truncate the answer. */
                break;
            }
            if ((xTaskGetTickCount() - dl_start) > pdMS_TO_TICKS(REPLY_MAX_MS)) {
                break;
            }
        }
        ring_audio_set_mute(true);
        ESP_LOGI(TAG, "turn: reply done (%u samples, %u frames)",
                 (unsigned)played, (unsigned)(played / FRAME_SAMPLES));
        transcript_log();

        vTaskDelay(pdMS_TO_TICKS(TURN_GAP_MS));
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

/* ------------------------------------------------------------- connect */

static void connect_task(void *arg)
{
    (void)arg;

    /* 1. Wi-Fi first, same as 06_qianwenai_push_to_talk. */
    esp_err_t werr = ring_wifi_init();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "ring_wifi_init: %s", esp_err_to_name(werr));
    }
    for (;;) {
        if (ring_wifi_start() != ESP_OK) {
            ESP_LOGW(TAG, "wifi start failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (ring_wifi_wait_connected(20000) != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect timed out");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        char ip[16] = "";
        ring_wifi_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip);
        ui_wifi();
        break;
    }

    /* 2. WebRTC session. */
    for (;;) {
        if (ring_net_start(QIANWEN_MODEL, s_api_key) == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "session failed — retry in 5 s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
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

        lv_obj_t *title = ring_ui_add_label(scr, "QIANWEN RT",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        s_status = ring_ui_add_label(scr, "connecting…",
                                     &lv_font_montserrat_20,
                                     lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

        s_wifi_label = ring_ui_add_label(scr, "Wi-Fi: …",
                                         &lv_font_montserrat_14,
                                         lv_color_hex(0x9E9E9E));
        lv_obj_set_width(s_wifi_label, 320);
        lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        ring_ui_place_centered(s_wifi_label, RING_CX, WIFI_Y);

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

        lv_obj_t *hint = ring_ui_add_label(scr, "auto talk loop",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(hint, RING_CX, HINT_Y);
    }
    s_ui_ready = true;

    if (s_api_key[0] == '\0') {
        ui_status("no API key", lv_color_hex(0xFF1744));
        return ESP_OK;
    }

    xTaskCreate(connect_task, "qw-connect", WORKER_STACK, NULL,
                WORKER_PRIO, NULL);
    xTaskCreate(talk_task, "qw-talk", TALK_TASK_STACK, NULL,
                WORKER_PRIO, NULL);
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "07_qianwen_realtime_harness";
}
