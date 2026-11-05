/*
 * qianwen-realtime — minimal WebRTC realtime test harness for the Qianwen
 * model.
 *
 * Unlike examples/qianwenai-button (a push-to-talk UI), this example is a
 * hands-free loopback probe: it brings up Wi-Fi + the WebRTC session, then
 * automatically runs talk turns — record from the mic, stream Opus to the
 * model, play the spoken reply, and log the transcript — over and over.
 *
 * It exists to answer one question: does the model interaction actually work
 * end to end? Every step is logged:
 *
 *   ring_net: state -> ready
 *   qianwen-rt: turn: start
 *   qianwen-rt: turn: uplink done (N frames)
 *   ring_net: dc: {"type":"response.audio_transcript.delta",...}
 *   qianwen-rt: turn: reply done (M frames)
 *   qianwen-rt: transcript: ...
 *
 * Audio is the same 48 kHz / 16-bit / stereo ES8311 stream as the other
 * examples; ring_net wants 48 kHz / 16-bit / mono in 20 ms frames, so this
 * file only does the stereo <-> mono fold and the turn bookkeeping.
 *
 * The talk task stack is large (24 KB) because the Opus encoder's SILK path
 * peaks at ~20 KB; see examples/qianwenai-button/README.md §4.
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

#define TAG "qianwen-rt"

/* ------------------------------------------------------------- test clip */

/* Embedded known speech: "the capital of China" (espeak -> 48 kHz / 16-bit
 * mono PCM). Generated with:
 *   espeak -v en -s 150 -w capital.wav "the capital of China"
 *   ffmpeg -y -i capital.wav -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
 *           examples/qianwen-realtime/assets/test_clip.pcm
 */
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
#define FRAME_STEREO    (FRAME_SAMPLES * 2 * 2)       /* 3840 bytes       */
#define OUT_STEREO_MAX  (MIC_RATE * 2 * 2 / 1000 * 40) /* 40 ms = 15360 B */

#define TURN_MAX_MS     8000    /* cap on one uplink                  */
#define TALK_GRACE_MS   3000    /* stop waiting this long after audio */
#define REPLY_MAX_MS    15000   /* cap on one reply                   */
#define TURN_GAP_MS     5000    /* pause between turns                */

/* ------------------------------------------------------------- task */

#define WORKER_STACK    8192
#define TALK_TASK_STACK 24576
#define WORKER_PRIO     5       /* below LVGL (6): button events win */

/* ------------------------------------------------------------- UI */

#define TITLE_Y        30
#define STATUS_Y       96
#define TRANSCRIPT_Y   140
#define HINT_Y         330

static lv_obj_t *s_status;
static lv_obj_t *s_transcript_label;

static char s_transcript[256];
static char s_api_key[160];

/* ------------------------------------------------------------- helpers */

static uint8_t s_stereo[FRAME_STEREO];
static int16_t s_mono[FRAME_SAMPLES];
static int16_t s_reply[FRAME_SAMPLES];
static uint8_t s_play[OUT_STEREO_MAX];

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

/* ------------------------------------------------------------- UI helpers */

static void ui_status(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, color, LV_PART_MAIN);
    }
}

static void ui_transcript(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_transcript_label, s_transcript);
    }
}

/* ------------------------------------------------------------- net glue */

static void net_state_cb(ring_net_state_t st, void *user)
{
    (void)user;
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

static void net_text_cb(const char *text, size_t len, void *user)
{
    (void)user;
    size_t n = sizeof(s_transcript) - 1 - strlen(s_transcript);
    if (len < n) {
        n = len;
    }
    if (n == 0) {
        return;
    }
    strncat(s_transcript, text, n);
    ui_transcript();
}

/* ------------------------------------------------------------- talk turn */

/* Send the embedded test clip to the model, frame by frame, and play it
 * through the speaker so the phrase is audible while it is being sent. */
static void send_test_clip(void)
{
    const int16_t *clip = (const int16_t *)test_clip_pcm_start;
    size_t clip_bytes = (size_t)(test_clip_pcm_end - test_clip_pcm_start);
    size_t clip_samples = clip_bytes / sizeof(int16_t);
    size_t frames = (clip_samples + FRAME_SAMPLES - 1) / FRAME_SAMPLES;

    ESP_LOGI(TAG, "sending test clip: %u samples (%u frames)",
             (unsigned)clip_samples, (unsigned)frames);

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
            return;
        }
        size_t bytes = mono_to_stereo(s_mono, FRAME_SAMPLES, s_play);
        ring_audio_set_mute(false);
        if (ring_audio_write(s_play, bytes) != ESP_OK) {
            ESP_LOGW(TAG, "speaker write failed");
        }
        ring_audio_set_mute(true);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
        s_transcript[0] = '\0';
        ui_transcript();

        if (ring_net_set_listening(true) != ESP_OK) {
            ESP_LOGW(TAG, "could not start listening");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Uplink: the known test clip if embedded, otherwise the mic. */
        uint32_t frames = 0;
        TickType_t start = xTaskGetTickCount();
        if ((void *)test_clip_pcm_start != (void *)test_clip_pcm_end) {
            send_test_clip();
            frames = (uint32_t)((test_clip_pcm_end - test_clip_pcm_start) /
                                sizeof(int16_t) / FRAME_SAMPLES);
        } else {
            while (ring_net_state() == RING_NET_LISTENING) {
                if (ring_audio_read(s_stereo, sizeof(s_stereo), 200) != ESP_OK) {
                    ESP_LOGW(TAG, "mic read failed");
                    break;
                }
                stereo_to_mono(s_stereo, s_mono, FRAME_SAMPLES);
                if (ring_net_send_audio(s_mono, FRAME_SAMPLES) != ESP_OK) {
                    ESP_LOGW(TAG, "send audio failed");
                    break;
                }
                frames++;
                if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(TURN_MAX_MS)) {
                    break;
                }
            }
        }
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
            } else if (ring_net_state() == RING_NET_READY &&
                       (xTaskGetTickCount() - last) > pdMS_TO_TICKS(TALK_GRACE_MS)) {
                break;
            }
            if ((xTaskGetTickCount() - dl_start) > pdMS_TO_TICKS(REPLY_MAX_MS)) {
                break;
            }
        }
        ring_audio_set_mute(true);
        ESP_LOGI(TAG, "turn: reply done (%u frames)", (unsigned)played);
        ESP_LOGI(TAG, "transcript: %s", s_transcript);

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

    /* 1. Wi-Fi first, same as qianwenai-button. */
    ring_wifi_init();
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

        s_transcript_label = ring_ui_add_label(scr, "",
                                         &lv_font_montserrat_14,
                                         lv_color_white());
        lv_obj_set_width(s_transcript_label, 320);
        lv_obj_set_style_max_height(s_transcript_label, 140, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_transcript_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_label_set_long_mode(s_transcript_label, LV_LABEL_LONG_WRAP);
        ring_ui_place_centered(s_transcript_label, RING_CX, TRANSCRIPT_Y);

        lv_obj_t *hint = ring_ui_add_label(scr, "auto talk every 5 s",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0x9E9E9E));
        ring_ui_place_centered(hint, RING_CX, HINT_Y);
    }

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
    return "qianwen-realtime";
}
