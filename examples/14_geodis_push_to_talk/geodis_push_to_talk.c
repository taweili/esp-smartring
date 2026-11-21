/*
 * 14_geodis_push_to_talk — GEODIS Voice Agent, push-to-talk.
 *
 * Hold the round button: the ring streams 48 kHz mono mic PCM into
 * ring_geodis (Opus over WebRTC to Azure OpenAI Realtime through the GEODIS
 * gateway), release to commit the buffer and trigger a response. The reply is
 * Opus-decoded by the module into a PSRAM ring and played here; the user and
 * assistant transcripts are shown as they arrive.
 *
 * The RAG tool call (`rechercher_base_geodis`) is executed by ring_geodis on
 * its own task; this app only shows "looking up…" and the result. It never
 * touches esp_peer, esp_http_client or the codec directly.
 *
 * CONFIG_RINGKIT_GEODIS_MOCK replaces the network half of the module with a
 * canned turn, so the whole UI / state machine / PTT gesture can be verified
 * at the desk with no Wi-Fi, no UDP and no metered endpoint. The mock's
 * /tool-call fails by design, which exercises the real failure path.
 *
 * Audio: ring_audio is 48 kHz / 16-bit / stereo interleaved; ring_geodis is
 * 48 kHz / 16-bit / mono. This file only folds/expands at the boundary.
 *
 * Task layout:
 *   geodis-connect  8 KB internal  — ring_geodis_start() with retry
 *   geodis-audio    128 KB PSRAM   — one persistent turn loop: capture on
 *                                    press, commit on release, playback until
 *                                    response.done + the ring drains
 *   ring_geodis' own signal / peer / event / tool tasks do the WebRTC work.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#define TAG "14_geodis_push_to_talk"

/* ------------------------------------------------------------- config */

/* Optional NVS endpoint overrides (a URL is not a secret; use ring_nvs_get_str,
 * not ring_secret_get(), which would persist its fallback). */
#define NVS_NS           "geodis"
#define NVS_KEY_BASE_URL "base_url"
#define NVS_KEY_SDP_URL  "sdp_url"

/* ------------------------------------------------------------- audio */

#define FRAME_SAMPLES    RING_GEODIS_FRAME_SAMPLES        /* 960 = 20 ms     */
#define MIC_FRAME_BYTES  (FRAME_SAMPLES * 2 * 2)         /* stereo 16-bit   */
#define OUT_STEREO_MAX   (FRAME_SAMPLES * 2 * 2)         /* 20 ms stereo    */

/* The grace is a safety net for a lost response.done; the module's state plus
 * an empty downlink ring are the real end-of-turn signal. */
#define REPLY_GRACE_MS   500
#define REPLY_MAX_MS     15000
#define LEVEL_UPDATE_MS  100

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK    8192    /* bytes; TLS runs in esp-tls' own task */
#define CONNECT_PRIO     5
#define AUDIO_STACK      131072  /* bytes; 128 KB in PSRAM (Opus encoder) */
#define AUDIO_PRIO       5
#define RETRY_MS         5000

/* ------------------------------------------------------------- UI */

#define TITLE_Y          28
#define BTN_CX           180
#define BTN_CY           112
#define BTN_R            70
#define STAGE_Y          222
#define USER_Y           248
#define ASST_Y           276
#define BAR_CX           180
#define BAR_Y            320
#define BAR_W            220
#define BAR_H            6

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_stage;
static lv_obj_t *s_user_label;
static lv_obj_t *s_asst_label;
static lv_obj_t *s_bar;

static bool s_ui_ready;

/* ------------------------------------------------------------- turn state */

typedef enum {
    AUDIO_EV_PRESS = 0,
} audio_ev_t;

static QueueHandle_t s_audio_q;

/* Written by the LVGL task (button callback), read by the audio task. */
static volatile bool s_release;
static volatile bool s_cancel;
static volatile bool s_turn_active;

/* Transcript lines. Only the ring-geodis-event task writes them (text_cb and
 * the LISTENING branch of state_cb), so no extra lock is needed. */
static char s_user[256];
static char s_asst[512];

/* ------------------------------------------------------------- UI helpers */

static void ui_button(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, text);
        lv_obj_set_style_bg_color(s_btn, color, LV_PART_MAIN);
    }
}

static void ui_stage(const char *msg, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_stage, msg);
        lv_obj_set_style_text_color(s_stage, color, LV_PART_MAIN);
    }
}

static void ui_transcript(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_user_label, s_user);
        lv_label_set_text(s_asst_label, s_asst);
    }
}

static void ui_level(uint8_t pct)
{
    RING_UI_LOCKED(-1) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    }
}

static void ui_idle(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_btn_label, "HOLD\nTO\nTALK");
        lv_obj_set_style_bg_color(s_btn, RING_COLOR_GREEN, LV_PART_MAIN);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_stage, "ready — hold to talk");
        lv_obj_set_style_text_color(s_stage, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------- callbacks */

static void geodis_state_cb(ring_geodis_state_t st, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    switch (st) {
    case RING_GEODIS_CONNECTING:
        ui_button("···", lv_color_hex(0x3A4048));
        ui_stage("connecting…", lv_color_hex(0x9E9E9E));
        break;
    case RING_GEODIS_READY:
        /* READY is transient inside a turn: after release before the commit,
         * and while the last reply frames drain. Only claim "ready" when no
         * turn is active; the audio task drives the stage during a turn. */
        if (!s_turn_active) {
            ui_button("HOLD\nTO\nTALK", RING_COLOR_GREEN);
            ui_stage("ready — hold to talk", lv_color_hex(0x9E9E9E));
        }
        break;
    case RING_GEODIS_LISTENING:
        /* A new turn: drop the previous exchange. Runs on the event task, the
         * same task as text_cb, so the buffers need no lock. */
        s_user[0] = '\0';
        s_asst[0] = '\0';
        ui_transcript();
        ui_button("TALK", lv_color_hex(0xFF1744));
        ui_stage("listening…", lv_color_hex(0xFF1744));
        break;
    case RING_GEODIS_SPEAKING:
        ui_button("REPLY", lv_color_hex(0x2196F3));
        ui_stage("speaking…", lv_color_hex(0x2196F3));
        break;
    case RING_GEODIS_ERROR: {
        esp_err_t e = ring_geodis_last_err();
        const char *en = (e == ESP_OK) ? "session lost" : esp_err_to_name(e);
        char line[64];
        snprintf(line, sizeof(line), "error: %s", en);
        ui_button("!", lv_color_hex(0xFF1744));
        ui_stage(line, lv_color_hex(0xFF1744));
        break;
    }
    case RING_GEODIS_OFF:
    default:
        break;
    }
}

static void geodis_text_cb(const char *text, size_t len, bool is_user,
                           bool final, bool clipped, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    if (clipped) {
        ESP_LOGW(TAG, "%s fragment clipped at %u bytes",
                 is_user ? "user" : "assistant", (unsigned)len);
    }

    char *dst = is_user ? s_user : s_asst;
    size_t cap = is_user ? sizeof(s_user) : sizeof(s_asst);

    if (final) {
        /* A final replaces the line. A clipped final is only a prefix. */
        size_t n = (len < cap - 1) ? len : cap - 1;
        memcpy(dst, text, n);
        dst[n] = '\0';
    } else {
        size_t cur = strlen(dst);
        size_t n = len;
        if (cur + n > cap - 1) {
            n = cap - 1 - cur;
        }
        memcpy(dst + cur, text, n);
        dst[cur + n] = '\0';
    }
    ui_transcript();
}

/* Runs on ring-geodis-tool, before the built-in RAG POST. Return false so the
 * module executes the lookup; the result arrives in geodis_tool_done_cb(). */
static bool geodis_tool_cb(const char *name, const char *args_json, void *user)
{
    (void)args_json;
    (void)user;
    ESP_LOGI(TAG, "tool call: %s", name);
    if (s_ui_ready) {
        ui_stage("looking up…", lv_color_hex(0xFF9800));
    }
    return false;
}

static void geodis_tool_done_cb(const char *name, bool ok, int status,
                                int chunks, void *user)
{
    (void)user;
    char line[64];
    if (ok) {
        snprintf(line, sizeof(line), "lookup ok — %d chunk%s",
                 chunks, (chunks == 1) ? "" : "s");
    } else {
        snprintf(line, sizeof(line), "lookup failed (HTTP %d)", status);
    }
    ESP_LOGI(TAG, "tool done: %s ok=%d status=%d chunks=%d",
             name, ok, status, chunks);
    if (s_ui_ready) {
        ui_stage(line, ok ? lv_color_hex(0x9E9E9E) : lv_color_hex(0xFF1744));
    }
}

/* ------------------------------------------------------------- audio task */

/* Downlink: play the reply until response.done and the ring has drained.
 * Unmute once, just before the first frame; toggling mute per frame is an I2C
 * write per 20 ms and clicks. */
static void play_reply(void)
{
    uint8_t stereo[OUT_STEREO_MAX];
    int16_t mono[FRAME_SAMPLES];

    ring_audio_set_mute(true);
    bool unmuted = false;
    TickType_t start = xTaskGetTickCount();
    TickType_t last = start;

    for (;;) {
        if (s_cancel) {
            ESP_LOGI(TAG, "playback cancelled (barge-in)");
            break;
        }
        if (ring_geodis_state() == RING_GEODIS_ERROR) {
            ESP_LOGW(TAG, "session lost during reply");
            break;
        }

        size_t got = 0;
        if (ring_geodis_read_audio(mono, FRAME_SAMPLES, &got, 50) == ESP_OK &&
            got > 0) {
            if (!unmuted) {
                ring_audio_set_mute(false);
                unmuted = true;
            }
            ring_audio_mono_to_stereo(mono, (int16_t *)stereo, got);
            if (ring_audio_write(stereo, got * 4) != ESP_OK) {
                ESP_LOGW(TAG, "speaker write failed");
            }
            last = xTaskGetTickCount();
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if (ring_geodis_state() == RING_GEODIS_READY &&
            (now - last) > pdMS_TO_TICKS(REPLY_GRACE_MS)) {
            break;
        }
        if ((now - start) > pdMS_TO_TICKS(REPLY_MAX_MS)) {
            ESP_LOGW(TAG, "reply cap reached");
            break;
        }
    }
    ring_audio_set_mute(true);
}

static void log_stats(void)
{
    ring_geodis_stats_t st;
    if (ring_geodis_get_stats(&st) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "stats: setup=%u ms token=%u ms sdp=%u ms "
                  "commit->first audio=%u ms sent=%u dropped=%u "
                  "tools=%u tool=%u ms",
             (unsigned)st.session_setup_ms, (unsigned)st.token_ms,
             (unsigned)st.sdp_ms, (unsigned)st.commit_to_first_audio_ms,
             (unsigned)st.frames_sent, (unsigned)st.frames_dropped,
             (unsigned)st.tool_calls, (unsigned)st.tool_ms);
}

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t stereo[MIC_FRAME_BYTES];
    int16_t mono[FRAME_SAMPLES];

    ESP_LOGI(TAG, "audio task started (stack hwm %u words)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    for (;;) {
        audio_ev_t ev;
        if (xQueueReceive(s_audio_q, &ev, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (ev != AUDIO_EV_PRESS) {
            continue;
        }

        /* The press that started this turn was a barge-in if s_cancel was set;
         * the previous playback loop has already seen it and broken. */
        s_cancel = false;
        s_turn_active = true;

        if (ring_geodis_set_listening(true) != ESP_OK) {
            ESP_LOGW(TAG, "set_listening failed");
            s_turn_active = false;
            continue;
        }

        uint32_t frames = 0;
        TickType_t last_level = xTaskGetTickCount();
        while (!s_release && ring_geodis_state() == RING_GEODIS_LISTENING) {
            if (ring_audio_read(stereo, sizeof(stereo), 200) != ESP_OK) {
                continue;
            }
            ring_audio_stereo_to_mono((const int16_t *)stereo, mono,
                                      FRAME_SAMPLES);
            if (ring_geodis_send_audio(mono, FRAME_SAMPLES) != ESP_OK) {
                ESP_LOGW(TAG, "send audio failed");
                break;
            }
            frames++;

            TickType_t now = xTaskGetTickCount();
            if (now - last_level >= pdMS_TO_TICKS(LEVEL_UPDATE_MS)) {
                last_level = now;
                ui_level(ring_audio_level_pct(mono, FRAME_SAMPLES));
            }
        }

        ring_geodis_set_listening(false);
        ui_level(0);

        ring_geodis_state_t st = ring_geodis_state();
        if (st == RING_GEODIS_ERROR) {
            ESP_LOGW(TAG, "session lost during capture");
            s_turn_active = false;
            continue;
        }

        if (frames > 0 && st == RING_GEODIS_READY) {
            /* PTT: release commits the buffer and triggers the response. */
            ui_stage("waiting…", lv_color_hex(0xFF9800));
            if (ring_geodis_commit() != ESP_OK) {
                ESP_LOGW(TAG, "commit failed — no response will come");
            }
        } else if (frames == 0) {
            ESP_LOGW(TAG, "empty capture — no commit");
            s_turn_active = false;
            continue;
        }
        /* else: the model already started answering (server-VAD fallback);
         * do not commit on top of a response in flight. */

        play_reply();
        s_turn_active = false;
        log_stats();

        if (ring_geodis_state() == RING_GEODIS_READY) {
            ui_idle();
        }
    }
}

/* ------------------------------------------------------------- connect */

static void connect_task(void *arg)
{
    (void)arg;

    ring_geodis_cfg_t cfg = RING_GEODIS_CFG_DEFAULT();
    char base_url[192] = "";
    char sdp_url[192] = "";

    if (ring_nvs_get_str(NVS_NS, NVS_KEY_BASE_URL, base_url, sizeof(base_url)) == ESP_OK &&
        base_url[0] != '\0') {
        cfg.base_url = base_url;
    }
    if (ring_nvs_get_str(NVS_NS, NVS_KEY_SDP_URL, sdp_url, sizeof(sdp_url)) == ESP_OK &&
        sdp_url[0] != '\0') {
        cfg.sdp_url = sdp_url;
    }

    ESP_LOGI(TAG, "endpoint: %s",
             (cfg.base_url != NULL) ? cfg.base_url : RING_GEODIS_DEFAULT_BASE_URL);
    ESP_LOGI(TAG, "sdp: %s",
             (cfg.sdp_url != NULL) ? cfg.sdp_url : RING_GEODIS_DEFAULT_SDP_URL);

    for (;;) {
        esp_err_t err = ring_geodis_start(&cfg);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "session ready");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "start failed: %s (last err %s) — retry in %u ms",
                 esp_err_to_name(err), esp_err_to_name(ring_geodis_last_err()),
                 (unsigned)RETRY_MS);
        char line[64];
        snprintf(line, sizeof(line), "error: %s — retrying", esp_err_to_name(err));
        ui_stage(line, lv_color_hex(0xFF1744));
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
}

/* ------------------------------------------------------------- input */

static void btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        if (!s_ui_ready) {
            return;
        }
        ring_geodis_state_t st = ring_geodis_state();
        if (st != RING_GEODIS_READY && st != RING_GEODIS_SPEAKING) {
            return;
        }
        /* A press while the reply plays is a barge-in: the module cancels and
         * flushes its downlink ring, and the audio task breaks out of playback
         * to start the new turn. */
        s_release = false;
        s_cancel = (st == RING_GEODIS_SPEAKING) || s_turn_active;

        audio_ev_t ev = AUDIO_EV_PRESS;
        if (xQueueSend(s_audio_q, &ev, 0) != pdPASS) {
            ESP_LOGW(TAG, "audio queue full — press ignored");
            /* Do not leave a barge-in flag set for a press that was dropped. */
            s_cancel = false;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_release = true;
    }
}

/* ------------------------------------------------------------- build */

esp_err_t ring_example_start(void)
{
    /* NVS must be up before the endpoint overrides are read. */
    esp_err_t nvs_err = ring_nvs_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "ring_nvs_init: %s", esp_err_to_name(nvs_err));
    }

    /* Codec gain/volume defaults, then mute until a reply plays. */
    esp_codec_dev_handle_t codec = ring_audio_codec();
    if (codec != NULL) {
        esp_codec_dev_set_in_gain(codec, RING_AUDIO_REC_GAIN_DB);
        esp_codec_dev_set_out_vol(codec, RING_AUDIO_OUT_VOL);
    }
    ring_audio_set_mute(true);

    esp_err_t err = ring_geodis_init(geodis_state_cb, geodis_text_cb,
                                     geodis_tool_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ring_geodis_init: %s", esp_err_to_name(err));
        return err;
    }
    ring_geodis_set_tool_done_cb(geodis_tool_done_cb);

    s_audio_q = xQueueCreate(4, sizeof(audio_ev_t));
    if (s_audio_q == NULL) {
        ESP_LOGE(TAG, "cannot create audio queue");
        return ESP_ERR_NO_MEM;
    }

    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();

        char title[32];
#ifdef CONFIG_RINGKIT_GEODIS_MOCK
        snprintf(title, sizeof(title), "GEODIS MOCK");
#else
        snprintf(title, sizeof(title), "GEODIS");
#endif
        lv_obj_t *title_label = ring_ui_add_label(scr, title,
                                                  &lv_font_montserrat_20,
                                                  RING_COLOR_GREEN);
        ring_ui_place_centered(title_label, RING_CX, TITLE_Y);

        /* The big round talk button. */
        s_btn = lv_obj_create(scr);
        lv_obj_set_size(s_btn, BTN_R * 2, BTN_R * 2);
        lv_obj_set_pos(s_btn, BTN_CX - BTN_R, BTN_CY - BTN_R);
        lv_obj_set_style_radius(s_btn, BTN_R, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x3A4048), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn,
                                  lv_color_darken(lv_color_hex(0x3A4048), 60),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);

        s_btn_label = ring_ui_add_label(s_btn, "···",
                                        &lv_font_montserrat_20,
                                        lv_color_white());
        lv_obj_center(s_btn_label);

        s_stage = ring_ui_add_label(scr, "connecting…",
                                    &lv_font_montserrat_16,
                                    lv_color_hex(0x9E9E9E));
        lv_obj_set_width(s_stage, 320);
        lv_obj_set_style_text_align(s_stage, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_stage, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_stage, RING_CX, STAGE_Y);

        s_user_label = ring_ui_add_label(scr, "",
                                         &lv_font_montserrat_14,
                                         lv_color_hex(0xFF9800));
        lv_obj_set_width(s_user_label, 320);
        lv_obj_set_style_text_align(s_user_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_user_label, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_user_label, RING_CX, USER_Y);

        s_asst_label = ring_ui_add_label(scr, "",
                                         &lv_font_montserrat_14,
                                         lv_color_white());
        lv_obj_set_width(s_asst_label, 320);
        lv_obj_set_style_text_align(s_asst_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_asst_label, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_asst_label, RING_CX, ASST_Y);

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

    if (xTaskCreateWithCaps(audio_task, "geodis-audio", AUDIO_STACK, NULL,
                            AUDIO_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "cannot create audio task");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(connect_task, "geodis-connect", CONNECT_STACK, NULL,
                    CONNECT_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create connect task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "14_geodis_push_to_talk";
}
