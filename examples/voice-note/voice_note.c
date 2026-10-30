/*
 * voice-note — mic + speaker loop-back demo.
 *
 * One big round button. Hold it to record from the ES8311 microphone; release
 * to play the captured PCM straight back through the speaker. A progress bar
 * shows the capture buffer filling while recording and the remaining audio
 * while playing back.
 *
 * Why a dedicated worker task:
 *   ring_audio_read()/ring_audio_write() are BLOCKING, and button events are
 *   delivered on the LVGL task. Recording inside the event callback would
 *   freeze the LVGL task, so the RELEASED event that has to stop the capture
 *   could never be processed. Instead LV_EVENT_PRESSED spawns a worker that
 *   owns the whole record -> playback sequence, and LV_EVENT_RELEASED simply
 *   flips a volatile stop flag.
 *
 *   The worker takes the LVGL lock (RING_UI_LOCKED) only for brief UI updates
 *   and NEVER while doing audio I/O, so rendering stays responsive and the
 *   audio path is never stalled by the display.
 *
 * Audio format is fixed by the BSP: 48 kHz / 16-bit / stereo interleaved
 * (~192 KB/s). The capture buffer lives in PSRAM (CONFIG_SPIRAM_USE_MALLOC)
 * and holds up to MAX_SEC seconds; when it fills, recording stops and the clip
 * plays back automatically. The clip is intentionally kept in RAM rather than
 * on the SD card so this example has no filesystem dependency (see ring_sd_*
 * for persistent notes).
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "voice-note"

/* ------------------------------------------------------------- audio */

#define SAMPLE_RATE   48000              /* BSP-fixed stream format       */
#define BYTES_PER_SEC (SAMPLE_RATE * 2 * 2) /* 48 kHz * stereo * 16-bit   */
#define MAX_SEC       8
#define PCM_CAP       (BYTES_PER_SEC * MAX_SEC) /* ~1.5 MB, PSRAM          */

#define REC_CHUNK   4096                 /* ~21 ms per record read         */
#define PLAY_CHUNK  16384                /* ~85 ms per playback write      */
#define REC_READ_TO_MS 200               /* per-chunk record timeout       */
#define UI_MS       250                  /* status refresh cadence         */

/* ------------------------------------------------------------- task */

#define REC_TASK_STACK 4096
#define REC_TASK_PRIO  5                 /* below LVGL (6): events win     */

/* ------------------------------------------------------------- UI */

#define TITLE_Y   34
#define BTN_CX    180
#define BTN_CY    158
#define BTN_R     78                     /* round button radius (px)       */
#define STATUS_Y  258
#define BAR_W     220
#define BAR_H     6
#define BAR_Y     288
#define HINT_Y    320

#define COL_BTN_BG lv_color_hex(0x101418)
#define COL_DIM    lv_color_hex(0x9E9E9E)
#define COL_REC    lv_color_hex(0xFF1744)
#define COL_PLAY   lv_color_hex(0xFF9800)

/* ------------------------------------------------------------- state */

static uint8_t *s_pcm = NULL;            /* capture buffer (PSRAM)        */
static uint8_t  s_chunk[REC_CHUNK];      /* internal-RAM I2S bounce        */

static volatile bool s_stop = false;     /* RELEASED sets this to stop rec */
static volatile bool s_worker_busy = false; /* a record/playback worker is up */

static lv_obj_t *s_btn;                  /* the one big round button       */
static lv_obj_t *s_btn_label;            /* label inside the button        */
static lv_obj_t *s_status;               /* status line below the button   */
static lv_obj_t *s_bar;                  /* capture / playback progress    */

/* ------------------------------------------------------------- UI updates
 *
 * Each helper takes the LVGL lock for itself and releases it before returning.
 * They are called from the worker task, so the lock is the only way to touch
 * LVGL objects safely from there. Never hold the lock around audio I/O.
 * ------------------------------------------------------------------------- */

static void ui_btn(const char *text, const lv_font_t *font,
                   lv_color_t text_color, lv_color_t border_color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_text_font(s_btn_label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_btn_label, text_color, LV_PART_MAIN);
        lv_obj_align(s_btn_label, LV_ALIGN_CENTER, 0, 0);

        lv_obj_set_style_border_color(s_btn, border_color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, COL_BTN_BG, LV_PART_MAIN);
    }
}

static void ui_bar(uint8_t pct, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, color, LV_PART_INDICATOR);
    }
}

static void ui_status(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, color, LV_PART_MAIN);
    }
}

/* Back to the resting state: instruction text, green accent, empty bar. */
static void ui_idle(const char *status)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, "HOLD\nTO\nREC");
        lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_20,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(s_btn_label, lv_color_white(),
                                    LV_PART_MAIN);
        lv_obj_align(s_btn_label, LV_ALIGN_CENTER, 0, 0);

        lv_obj_set_style_border_color(s_btn, RING_COLOR_GREEN, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, COL_BTN_BG, LV_PART_MAIN);

        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, RING_COLOR_GREEN, LV_PART_INDICATOR);

        lv_label_set_text(s_status,
                          status ? status : "press & hold the button");
        lv_obj_set_style_text_color(s_status, COL_DIM, LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------- worker task
 *
 * Owns the full lifecycle for one press: record until release (or buffer
 * full), then play the clip back, then put the UI back to idle and delete
 * itself. Only this task touches s_pcm / s_chunk.
 * ------------------------------------------------------------------------- */

static void voice_worker(void *arg)
{
    (void)arg;

    ui_btn("REC", &lv_font_montserrat_48, COL_REC, COL_REC);
    ui_status("recording…", COL_REC);
    ui_bar(0, COL_REC);
    ESP_LOGI(TAG, "recording");

    /* Capture. The first chunk is read unconditionally so even a quick tap
     * records something; afterwards a set stop flag ends the capture as soon
     * as the chunk in flight completes. */
    size_t len = 0;
    uint32_t last_ui = 0;
    for (;;) {
        if (len >= PCM_CAP) {
            break;                      /* buffer full: auto-stop and play */
        }
        size_t want = REC_CHUNK;
        if (len + want > PCM_CAP) {
            want = PCM_CAP - len;
        }
        if (ring_audio_read(s_chunk, want, REC_READ_TO_MS) != ESP_OK) {
            ESP_LOGW(TAG, "record read failed; stopping capture");
            break;
        }
        memcpy(s_pcm + len, s_chunk, want);
        len += want;

        if (s_stop || len >= PCM_CAP) {
            break;
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui >= UI_MS) {
            last_ui = now;
            char buf[48];
            snprintf(buf, sizeof buf, "recording %.1f s / %d s",
                     (float)len / BYTES_PER_SEC, MAX_SEC);
            ui_status(buf, COL_REC);
            ui_bar((uint8_t)((uint64_t)len * 100 / PCM_CAP), COL_REC);
        }
    }

    ESP_LOGI(TAG, "recorded %u bytes (%.2f s)", (unsigned)len,
             (float)len / BYTES_PER_SEC);

    /* Playback: unmute once, stream in chunks (so the bar can move), mute. */
    if (len > 0) {
        ui_btn("PLAY", &lv_font_montserrat_48, COL_PLAY, COL_PLAY);
        ui_status("playing back…", COL_PLAY);
        ui_bar(100, COL_PLAY);
        ESP_LOGI(TAG, "playback");

        ring_audio_set_mute(false);
        size_t played = 0;
        while (played < len) {
            size_t n = PLAY_CHUNK;
            if (played + n > len) {
                n = len - played;
            }
            if (ring_audio_write(s_pcm + played, n) != ESP_OK) {
                ESP_LOGW(TAG, "playback write failed");
                break;
            }
            played += n;
            ui_bar((uint8_t)((uint64_t)(len - played) * 100 / len), COL_PLAY);
        }
        ring_audio_set_mute(true);
        ESP_LOGI(TAG, "playback done");

        char buf[48];
        snprintf(buf, sizeof buf, "played %.1f s — hold to record again",
                 (float)len / BYTES_PER_SEC);
        ui_idle(buf);
    } else {
        ui_idle("nothing recorded — hold to record");
    }

    s_worker_busy = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- input */

static void btn_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (s_worker_busy) {
            return;                     /* already recording or playing */
        }
        if (s_pcm == NULL) {
            ui_status("audio buffer unavailable — restart app", COL_DIM);
            return;
        }
        s_stop = false;
        s_worker_busy = true;
        if (xTaskCreate(voice_worker, "voice-note", REC_TASK_STACK, NULL,
                        REC_TASK_PRIO, NULL) != pdPASS) {
            s_worker_busy = false;
            ui_status("could not start recorder", COL_DIM);
            ESP_LOGE(TAG, "xTaskCreate failed");
        }
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        /* Stop the capture (no effect if the worker is already playing). */
        s_stop = true;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    /* Board-tuned codec settings from the BSP notes. */
    esp_codec_dev_handle_t codec = ring_audio_codec();
    if (codec != NULL) {
        esp_codec_dev_set_in_gain(codec, RING_AUDIO_REC_GAIN_DB);
        esp_codec_dev_set_out_vol(codec, RING_AUDIO_OUT_VOL);
    }

    /* Capture buffer: 8 s of 48 kHz / 16-bit / stereo in PSRAM (~1.5 MB). */
    s_pcm = heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM);
    if (s_pcm == NULL) {
        ESP_LOGE(TAG, "PSRAM capture buffer (%d bytes) allocation failed",
                 (int)PCM_CAP);
    }

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        /* Title. */
        lv_obj_t *title = ring_ui_add_label(scr, "VOICE NOTE",
                                            &lv_font_montserrat_20,
                                            RING_COLOR_GREEN);
        ring_ui_place_centered(title, RING_CX, TITLE_Y);

        /* The one big round button. */
        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, BTN_R * 2, BTN_R * 2);
        lv_obj_set_pos(s_btn, BTN_CX - BTN_R, BTN_CY - BTN_R);
        lv_obj_set_style_radius(s_btn, BTN_R, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, COL_BTN_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_btn, RING_COLOR_GREEN, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
        /* Instant pressed feedback, before the worker takes over. */
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x2A0A0E),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_btn, COL_REC, LV_STATE_PRESSED);
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);

        s_btn_label = ring_ui_add_label(s_btn, "HOLD\nTO\nREC",
                                        &lv_font_montserrat_20,
                                        lv_color_white());
        lv_obj_center(s_btn_label);

        /* Status line. */
        s_status = ring_ui_add_label(scr, "press & hold the button",
                                     &lv_font_montserrat_16, COL_DIM);
        ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

        /* Progress bar: recording fill / playback remaining. */
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
        lv_obj_remove_flag(s_bar,
                           LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* Bottom hint. */
        lv_obj_t *hint = ring_ui_add_label(scr, "hold = record · release = play",
                                           &lv_font_montserrat_14, COL_DIM);
        ring_ui_place_centered(hint, RING_CX, HINT_Y);

        /* Hold-to-record / release-to-play on the button only. */
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
    }

    if (s_pcm != NULL) {
        ESP_LOGI(TAG, "ready: hold to record, release to play back "
                      "(up to %d s, %.1f MB PSRAM)",
                 MAX_SEC, (double)PCM_CAP / (1024.0 * 1024.0));
    } else {
        ESP_LOGW(TAG, "running without a capture buffer (recording disabled)");
    }
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "voice-note";
}
