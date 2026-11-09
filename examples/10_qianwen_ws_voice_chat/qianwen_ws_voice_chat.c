/*
 * 10_qianwen_ws_voice_chat — WebSocket voice conversation.
 *
 * The simplest voice assistant path: WebSocket over TCP/TLS 443 (the only
 * realtime transport that works on a UDP-blocked network), raw PCM in JSON
 * (no Opus, no DTLS, no SRTP), 16 kHz in / 24 kHz out, resampled to/from
 * the ring's fixed 48 kHz path.
 *
 * Default mode is **push-to-talk** (a round button on screen — hold to talk,
 * release to hear the reply). A long-press on the title opens a settings
 * panel for turn mode / voice / speech rate / language; the settings persist
 * in NVS and a change that needs a reconnect (mode, voice) restarts the
 * session. A tap while the model is replying is a barge-in.
 *
 * The screen shows a rolling transcript (user in orange, model in white), a
 * wire-stats line (bytes/s, turn count, mode, battery) refreshed once a
 * second, and a level bar during capture/playback.
 *
 * Audio: ring_audio is 48 kHz / 16-bit / stereo interleaved; ring_ws is
 * 48 kHz / 16-bit / mono. This file only does the stereo <-> mono fold.
 *
 * Task structure:
 *   qw-connect  8192 B internal  — Wi-Fi + SNTP + WS session + reconnects
 *   qw-talk     8192 B internal  — PTT worker (sequential capture + play)
 *   LVGL task   stats timer      — 1 Hz wire/battery readout
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdarg.h>
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

/* Level-bar redraw cadence. The bar does not need 50 fps, and every call
 * takes the LVGL lock; 10 Hz is plenty. */
#define BAR_UPDATE_MS    100

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK    8192
#define WORKER_STACK     8192
#define WORKER_PRIO      5

/* ------------------------------------------------------------- UI */

#define TITLE_Y   30
#define BTN_CX    180
#define BTN_CY    160
#define BTN_SZ    148
#define STATUS_Y  250
#define USER_Y    272
#define MODEL_Y   292
#define STATS_Y   308
#define BAR_W     210
#define BAR_H     6
#define BAR_Y     322
/* The bottom of the round panel is only ~129 px wide; the netinfo line is
 * ~260 px, so it lives under the title (~281 px wide) instead. */
#define NETINFO_Y 64

/* Settings panel rows. */
#define SET_ROW_Y0   100
#define SET_ROW_DY   40
#define SET_BTN_W    300
#define SET_BACK_Y   290

#define COL_WAIT   RING_COLOR_GREEN
#define COL_CONN   lv_color_hex(0x3A4048)
#define COL_DIM    lv_color_hex(0x9E9E9E)
#define COL_TALK   lv_color_hex(0xFF1744)
#define COL_WS     lv_color_hex(0x448AFF)
#define COL_USER   lv_color_hex(0xFF9800)
#define COL_PANEL  lv_color_hex(0x14181D)
#define COL_ROW    lv_color_hex(0x242A31)

/* ------------------------------------------------------------- settings
 *
 * Session settings that survive a reboot via NVS. The Qianwen API only
 * accepts turn_detection / voice while IDLE, so changing them reconnects the
 * session (see plan-ws-voice-chat.md C1). */

static const char *const kTurnModes[] = { "ptt", "server_vad", "smart_turn_v2" };
static const char *const kVoices[] = {
    "longanqian_v3.1", "longanhuan_v3.1", "longanlingxin_v3.1",
    "longanfengyue_v3.1", "xunanchuan_v3.1"
};
static const float kRates[] = { 0.5f, 1.0f, 1.5f, 2.0f };
static const char *const kLangs[] = { "auto", "zh", "en" };

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int s_cfg_turn;
static int s_cfg_voice;
static int s_cfg_rate = 1;   /* index into kRates: 1.0 = normal speed */
static int s_cfg_lang;
static volatile bool s_cfg_dirty;

/* ------------------------------------------------------------- state */

static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_status;
static lv_obj_t *s_bar;
static lv_obj_t *s_user;
static lv_obj_t *s_model;
static lv_obj_t *s_stats;
static lv_obj_t *s_netinfo;
static lv_obj_t *s_settings;
static lv_obj_t *s_set_rows[4];

static volatile bool s_stop = false;
static volatile bool s_worker_busy = false;
static bool s_ui_ready = false;
static bool s_settings_visible = false;

/* The Qianwen server closes a session that has not generated a response for
 * 180 s. A session.update every 60 s keeps it alive without producing a
 * reply (verified with scripts/test_qianwen_ws_keepalive.py). */
#define KEEPALIVE_MS 60000
static TickType_t s_last_keepalive_tick;

static uint8_t  s_stereo[FRAME_STEREO];
static int16_t  s_mono[FRAME_SAMPLES];
static int16_t  s_reply[FRAME_SAMPLES];
static uint8_t  s_play[OUT_STEREO_MAX];

static char     s_api_key[160];
static char     s_wifi_ssid[32];
static char     s_wifi_ip[16];
static char     s_ws_state[20];

static ring_ws_session_cfg_t s_ws_cfg;

/* s_ws_state / s_wifi_* are written from the event task and the connect
 * task and read by the UI; guard them with a mutex. */
static SemaphoreHandle_t s_state_mtx;

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

static void ui_user(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_user, text);
    }
}

static void ui_model(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_model, text);
    }
}

static void ui_stats(const char *text)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_stats, text);
    }
}

static void ui_netinfo(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_netinfo, text);
        lv_obj_set_style_text_color(s_netinfo, color, LV_PART_MAIN);
    }
}

static void clear_transcript(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_user, "");
        lv_label_set_text(s_model, "");
    }
}

static void set_ws_state(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    vsnprintf(s_ws_state, sizeof(s_ws_state), fmt, ap);
    xSemaphoreGive(s_state_mtx);
    va_end(ap);
}

/* Update the status line with combined Wi-Fi + WebSocket state. Snapshot the
 * shared strings under the lock — an unsynchronised snprintf can tear. */
static void ui_update_netinfo(lv_color_t color)
{
    char line[96];
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
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
    xSemaphoreGive(s_state_mtx);
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
        lv_label_set_text(s_model, "hold = talk · release = reply");
    }
}

/* ------------------------------------------------------------- settings */

static void settings_load(void)
{
    char buf[32];

    if (ring_nvs_get_str("qianwen", "turn_mode", buf, sizeof(buf)) == ESP_OK) {
        for (int i = 0; i < (int)ARRAY_SIZE(kTurnModes); i++) {
            if (strcmp(buf, kTurnModes[i]) == 0) {
                s_cfg_turn = i;
                break;
            }
        }
    }
    if (ring_nvs_get_str("qianwen", "voice", buf, sizeof(buf)) == ESP_OK) {
        for (int i = 0; i < (int)ARRAY_SIZE(kVoices); i++) {
            if (strcmp(buf, kVoices[i]) == 0) {
                s_cfg_voice = i;
                break;
            }
        }
    }
    if (ring_nvs_get_str("qianwen", "rate", buf, sizeof(buf)) == ESP_OK) {
        for (int i = 0; i < (int)ARRAY_SIZE(kRates); i++) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%.1f", kRates[i]);
            if (strcmp(buf, tmp) == 0) {
                s_cfg_rate = i;
                break;
            }
        }
    }
    if (ring_nvs_get_str("qianwen", "lang", buf, sizeof(buf)) == ESP_OK) {
        for (int i = 0; i < (int)ARRAY_SIZE(kLangs); i++) {
            if (strcmp(buf, kLangs[i]) == 0) {
                s_cfg_lang = i;
                break;
            }
        }
    }
}

static void settings_save(void)
{
    char tmp[8];
    ring_nvs_set_str("qianwen", "turn_mode", kTurnModes[s_cfg_turn]);
    ring_nvs_set_str("qianwen", "voice", kVoices[s_cfg_voice]);
    snprintf(tmp, sizeof(tmp), "%.1f", kRates[s_cfg_rate]);
    ring_nvs_set_str("qianwen", "rate", tmp);
    ring_nvs_set_str("qianwen", "lang", kLangs[s_cfg_lang]);
}

/* Fill s_ws_cfg from the selected settings. Called before every
 * ring_ws_start(); a mode/voice change forces a reconnect. */
static void settings_apply(void)
{
    memset(&s_ws_cfg, 0, sizeof(s_ws_cfg));
    s_ws_cfg.voice = kVoices[s_cfg_voice];
    s_ws_cfg.turn_detection = (strcmp(kTurnModes[s_cfg_turn], "ptt") == 0)
                                  ? NULL : kTurnModes[s_cfg_turn];
    s_ws_cfg.speech_rate = kRates[s_cfg_rate];
    s_ws_cfg.max_history_turns = 20;
    if (strcmp(kLangs[s_cfg_lang], "auto") != 0) {
        s_ws_cfg.input_language = kLangs[s_cfg_lang];
        s_ws_cfg.output_language = kLangs[s_cfg_lang];
    }
}

static void settings_refresh(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text_fmt(s_set_rows[0], "Mode:  %s", kTurnModes[s_cfg_turn]);
        lv_label_set_text_fmt(s_set_rows[1], "Voice: %s", kVoices[s_cfg_voice]);
        lv_label_set_text_fmt(s_set_rows[2], "Rate:  %.1f", kRates[s_cfg_rate]);
        lv_label_set_text_fmt(s_set_rows[3], "Lang:  %s", kLangs[s_cfg_lang]);
    }
}

static void settings_cycle(int row)
{
    switch (row) {
    case 0:
        s_cfg_turn = (s_cfg_turn + 1) % (int)ARRAY_SIZE(kTurnModes);
        break;
    case 1:
        s_cfg_voice = (s_cfg_voice + 1) % (int)ARRAY_SIZE(kVoices);
        break;
    case 2:
        s_cfg_rate = (s_cfg_rate + 1) % (int)ARRAY_SIZE(kRates);
        break;
    case 3:
        s_cfg_lang = (s_cfg_lang + 1) % (int)ARRAY_SIZE(kLangs);
        break;
    default:
        return;
    }
    settings_save();
    s_cfg_dirty = true;
    settings_refresh();
}

static void settings_row_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    settings_cycle(row);
}

static void settings_hide(void)
{
    s_settings_visible = false;
    RING_UI_LOCKED(-1) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    settings_hide();
}

static void settings_show(void)
{
    s_settings_visible = true;
    settings_refresh();
    RING_UI_LOCKED(-1) {
        lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
}

static void title_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ui_ready) {
        return;
    }
    if (s_settings_visible) {
        settings_hide();
    } else {
        settings_show();
    }
}

/* ------------------------------------------------------------- stats timer */

static uint32_t s_last_up;
static uint32_t s_last_down;
static uint32_t s_last_ms;
static bool s_stats_ready;

static void stats_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ui_ready) {
        return;
    }
    ring_ws_stats_t st;
    if (ring_ws_get_stats(&st) != ESP_OK) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_stats_ready) {
        s_last_up = st.bytes_up;
        s_last_down = st.bytes_down;
        s_last_ms = now;
        s_stats_ready = true;
        return;
    }
    uint32_t dt = now - s_last_ms;
    if (dt < 1000) {
        return;
    }
    float up = (float)(st.bytes_up - s_last_up) * 1000.0f / (float)dt;
    float down = (float)(st.bytes_down - s_last_down) * 1000.0f / (float)dt;
    s_last_up = st.bytes_up;
    s_last_down = st.bytes_down;
    s_last_ms = now;

    char line[80];
    snprintf(line, sizeof(line), "↑%.0f ↓%.0f B/s · turn %u · %s · %d%%",
             up, down, (unsigned)st.turns,
             st.turn_mode ? st.turn_mode : "ptt",
             ring_battery_percent());
    ui_stats(line);
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
        set_ws_state("WS %s", ring_ws_turn_mode() ? ring_ws_turn_mode() : "ptt");
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
        set_ws_state("listening");
        ui_update_netinfo(COL_TALK);
        ui_button("TALK", &lv_font_montserrat_48, COL_TALK);
        ui_status("listening…", COL_TALK);
        ui_bar(0, COL_TALK);
        break;
    case RING_WS_SPEAKING:
        set_ws_state("reply…");
        ui_update_netinfo(COL_WS);
        ui_button("REPLY", &lv_font_montserrat_48, COL_WS);
        /* The half-duplex gate mutes the mic while the model talks. */
        ui_status("replying… · mic off", COL_WS);
        ui_bar(100, COL_WS);
        break;
    case RING_WS_CONNECTING:
        set_ws_state("WS…");
        ui_update_netinfo(COL_DIM);
        ui_button("···", &lv_font_montserrat_48, COL_CONN);
        ui_status("connecting…", COL_DIM);
        ui_bar(0, COL_DIM);
        break;
    case RING_WS_ERROR: {
        set_ws_state("WS err");
        ui_update_netinfo(COL_TALK);
        if (!s_worker_busy) {
            ui_button("!", &lv_font_montserrat_48, COL_TALK);
            /* Surface the protocol error (type/param) when we have one. */
            ring_ws_stats_t est;
            if (ring_ws_get_stats(&est) == ESP_OK &&
                est.last_error_type && est.last_error_type[0]) {
                char line[64];
                snprintf(line, sizeof(line), "error: %s %s",
                         est.last_error_type,
                         est.last_error_param ? est.last_error_param : "");
                ui_status(line, COL_TALK);
            } else {
                ui_status("connection lost", COL_TALK);
            }
        }
        break;
    }
    case RING_WS_OFF:
    default:
        set_ws_state("");
        break;
    }
}

static void ws_text_cb(const char *text, size_t len, bool is_user, void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    static char line[64];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    if (is_user) {
        ui_user(line);
    } else {
        ui_model(line);
    }
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
    clear_transcript();
    s_worker_busy = false;
    /* Only claim "ready" when the session actually is; a turn that ended
     * because the socket dropped must not leave the button looking live. */
    if (ring_ws_state() == RING_WS_READY) {
        char line[64];
        snprintf(line, sizeof(line), "ready (%s) — hold to talk",
                 ring_ws_turn_mode() ? ring_ws_turn_mode() : "ptt");
        ui_idle(line);
    }
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
    TickType_t last_bar = last;

    /* response.done (observed via the turn counter) is the authoritative end
     * of the reply; the audio grace is only a fallback for a lost done. */
    uint32_t turns_start = 0;
    ring_ws_stats_t stats;
    if (ring_ws_get_stats(&stats) == ESP_OK) {
        turns_start = stats.turns;
    }

    for (;;) {
        /* A dropped socket must end the turn now, not after the 15 s cap. */
        if (ring_ws_state() == RING_WS_ERROR) {
            ESP_LOGW(TAG, "session lost during reply");
            break;
        }
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
            TickType_t now = xTaskGetTickCount();
            if (now - last_bar >= pdMS_TO_TICKS(BAR_UPDATE_MS)) {
                last_bar = now;
                ui_bar(level_of(s_reply, got), COL_WS);
            }
        } else {
            /* Draining after response.done: stop once the ring is empty. */
            ring_ws_stats_t cur;
            if (ring_ws_get_stats(&cur) == ESP_OK && cur.turns > turns_start &&
                ring_ws_state() == RING_WS_READY && !ring_ws_playback_pending()) {
                break;
            }
            if (got_audio &&
                ring_ws_state() == RING_WS_READY &&
                !ring_ws_playback_pending() &&
                (xTaskGetTickCount() - last) > pdMS_TO_TICKS(TALK_GRACE_MS)) {
                break;
            }
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(TALK_MAX_MS)) {
            ESP_LOGW(TAG, "reply cap reached");
            break;
        }
    }
    ring_audio_set_mute(true);

    ring_ws_stats_t st;
    if (ring_ws_get_stats(&st) == ESP_OK) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "turn done: up=%u B down=%u B turns=%u "
                      "dropped up/down=%u/%u ring_high=%u ms "
                      "stack_hwm=%u B",
                 (unsigned)st.bytes_up, (unsigned)st.bytes_down,
                 (unsigned)st.turns, (unsigned)st.dropped_up,
                 (unsigned)st.dropped_down, (unsigned)st.ring_high_ms,
                 (unsigned)hwm);
        if (hwm < 1000) {
            ESP_LOGW(TAG, "talk task stack headroom low (%u B) — raise WORKER_STACK",
                     (unsigned)hwm);
        }
    }
}

static void talk_worker(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "turn: capture");
    clear_transcript();

    /* Uplink: one 20 ms frame at a time until released. */
    uint32_t frames = 0;
    TickType_t last_bar = xTaskGetTickCount();
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
        TickType_t now = xTaskGetTickCount();
        if (now - last_bar >= pdMS_TO_TICKS(BAR_UPDATE_MS)) {
            last_bar = now;
            ui_bar(level_of(s_mono, FRAME_SAMPLES), COL_TALK);
        }
    }

    ESP_LOGI(TAG, "turn: release");
    ring_ws_set_listening(false);

    /* A socket that dropped during capture has no session to commit to. */
    if (ring_ws_state() == RING_WS_ERROR) {
        ESP_LOGW(TAG, "session lost during capture");
        turn_finish();
        vTaskDelete(NULL);
        return;
    }

    /* Commit + create response. Show the wait state while the server thinks;
     * the reply can take a few hundred ms to start. */
    ui_button("WAIT", &lv_font_montserrat_48, COL_TALK);
    ui_status("waiting…", COL_TALK);
    if (ring_ws_commit() != ESP_OK) {
        ESP_LOGW(TAG, "commit failed — no response will come");
    }

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
    if (ring_ws_commit() != ESP_OK) {
        ESP_LOGW(TAG, "selftest: commit failed");
    }
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

    set_ws_state("WS…");
    ui_update_netinfo(COL_DIM);
    ui_status("connecting to model…", COL_DIM);

    for (;;) {
        settings_apply();
        if (ring_ws_start(QIANWEN_MODEL, s_api_key, &s_ws_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "session up — supervising");
            s_last_keepalive_tick = xTaskGetTickCount();
            /* Stay alive: a successful start used to end this task, so a
             * later disconnect left the UI on "!" forever. Reconnect when
             * the session drops or the settings changed. */
            while (ring_ws_state() != RING_WS_ERROR &&
                   ring_ws_state() != RING_WS_OFF) {
                if (s_cfg_dirty) {
                    break;
                }
                TickType_t now = xTaskGetTickCount();
                if (ring_ws_state() == RING_WS_READY &&
                    (now - s_last_keepalive_tick) > pdMS_TO_TICKS(KEEPALIVE_MS)) {
                    s_last_keepalive_tick = now;
                    ESP_LOGI(TAG, "keep-alive: session.update");
                    esp_err_t ka_err = ring_ws_keepalive();
                    if (ka_err != ESP_OK && ka_err != ESP_ERR_INVALID_STATE) {
                        ESP_LOGW(TAG, "keep-alive failed: %s",
                                 esp_err_to_name(ka_err));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            /* Let an in-flight turn finish before tearing the client down. */
            while (s_worker_busy) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (s_cfg_dirty) {
                ESP_LOGI(TAG, "settings changed — reconnecting");
                ui_status("applying settings…", COL_DIM);
                ring_ws_stop();
                s_cfg_dirty = false;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
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
    s_state_mtx = xSemaphoreCreateMutex();
    if (s_state_mtx == NULL) {
        ESP_LOGE(TAG, "cannot create state mutex");
        return ESP_ERR_NO_MEM;
    }

    /* NVS must be up before ring_secret_get() reads the persisted key.
     * ring_wifi_init() would do it later, but the key is needed now. */
    esp_err_t nvs_err = ring_nvs_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "ring_nvs_init: %s (build-time key still usable)",
                 esp_err_to_name(nvs_err));
    }

    load_api_key();
    settings_load();
    settings_apply();

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
        lv_obj_set_width(s_status, 300);
        lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
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

        /* Rolling transcript: user (orange) above, model (grey) below. Both
         * are capped to the round panel's width at their y. */
        s_user = ring_ui_add_label(scr, "", &lv_font_montserrat_14, COL_USER);
        lv_obj_set_width(s_user, 300);
        lv_obj_set_style_text_align(s_user, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_user, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_user, RING_CX, USER_Y);

        s_model = ring_ui_add_label(scr, "hold = talk · release = reply",
                                    &lv_font_montserrat_14, COL_DIM);
        lv_obj_set_width(s_model, 270);
        lv_obj_set_style_text_align(s_model, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_model, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_model, RING_CX, MODEL_Y);

        s_stats = ring_ui_add_label(scr, "", &lv_font_montserrat_14, COL_DIM);
        lv_obj_set_width(s_stats, 250);
        lv_obj_set_style_text_align(s_stats, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_stats, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_stats, RING_CX, STATS_Y);

        s_netinfo = ring_ui_add_label(scr, "",
                                      &lv_font_montserrat_14, COL_DIM);
        /* Under the title the panel is ~281 px wide. */
        lv_obj_set_width(s_netinfo, 270);
        lv_obj_set_style_text_align(s_netinfo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_netinfo, LV_LABEL_LONG_DOT);
        ring_ui_place_centered(s_netinfo, RING_CX, NETINFO_Y);

        /* Settings panel — hidden until the title is long-pressed. */
        s_settings = lv_obj_create(scr);
        lv_obj_set_size(s_settings, RING_PANEL_W, RING_PANEL_H);
        lv_obj_center(s_settings);
        lv_obj_set_style_bg_color(s_settings, COL_PANEL, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_settings, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_settings, 0, LV_PART_MAIN);
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *set_title = ring_ui_add_label(s_settings, "SETTINGS",
                                                &lv_font_montserrat_20,
                                                RING_COLOR_GREEN);
        ring_ui_place_centered(set_title, RING_CX, 40);

        for (int i = 0; i < 4; i++) {
            s_set_rows[i] = lv_label_create(s_settings);
            lv_obj_set_width(s_set_rows[i], SET_BTN_W);
            lv_obj_set_style_text_align(s_set_rows[i], LV_TEXT_ALIGN_CENTER,
                                        LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_set_rows[i], COL_ROW, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_set_rows[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(s_set_rows[i], 8, LV_PART_MAIN);
            lv_obj_add_flag(s_set_rows[i], LV_OBJ_FLAG_CLICKABLE);
            ring_ui_place_centered(s_set_rows[i], RING_CX,
                                   SET_ROW_Y0 + i * SET_ROW_DY);
            lv_obj_add_event_cb(s_set_rows[i], settings_row_cb,
                                LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        lv_obj_t *back = lv_label_create(s_settings);
        lv_label_set_text(back, "BACK");
        lv_obj_set_width(back, 120);
        lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(back, COL_ROW, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(back, 8, LV_PART_MAIN);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        ring_ui_place_centered(back, RING_CX, SET_BACK_Y);
        lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);

        settings_refresh();

        /* Long-press the title to toggle the settings panel. */
        lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title, title_cb, LV_EVENT_LONG_PRESSED, NULL);

        /* 1 Hz wire/battery readout. */
        lv_timer_create(stats_timer_cb, 1000, NULL);

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
