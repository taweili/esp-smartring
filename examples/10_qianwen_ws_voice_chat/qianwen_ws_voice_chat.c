/*
 * 10_qianwen_ws_voice_chat — WebSocket voice conversation.
 *
 * The simplest voice assistant path: WebSocket over TCP/TLS 443 (the only
 * realtime transport that works on a UDP-blocked network), raw PCM in JSON
 * (no Opus, no DTLS, no SRTP), 16 kHz in / 24 kHz out, resampled to/from
 * the ring's fixed 48 kHz path.
 *
 * Default mode is **push-to-talk** (a round orb on screen — hold to talk,
 * release to hear the reply). A tap while the model is replying is a
 * barge-in. Three screens:
 *
 *   Talk   — the orb (button + radial level ring), the last exchange and
 *            the state line.
 *   Chat   — the full scrolling transcript plus wire/battery stats.
 *   Set    — turn mode / voice / rate / language, persisted in NVS; a mode
 *            or voice change reconnects the session.
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

/* Level-ring redraw cadence. The ring does not need 50 fps, and every call
 * takes the LVGL lock; 10 Hz is plenty. */
#define LEVEL_UPDATE_MS  100

/* ------------------------------------------------------------- tasks */

#define CONNECT_STACK    8192
#define WORKER_STACK     8192
#define WORKER_PRIO      5

/* ------------------------------------------------------------- UI */

/* Talk screen. The orb (ring + button) is centred at (BTN_CX, BTN_CY). */
#define TOPBAR_Y         26
#define BTN_CX          180
#define BTN_CY          158
#define BTN_SZ          148
#define RING_SZ         176
#define RING_WEAR        8      /* ring thickness */

#define TALK_USER_Y     256
#define TALK_MODEL_Y    288
#define TALK_TEXT_W     280
#define TALK_TEXT_H      32      /* two wrapped lines */

#define STATUS_Y        326
#define STATUS_W        210

/* Chat screen. The box must fit the circle at its top edge (y=56). */
#define CHAT_Y           56
#define CHAT_W          260
#define CHAT_H          224
#define CHAT_LABEL_W    (CHAT_W - 16)
#define CHAT_STATS_Y    290
#define CHAT_NETINFO_Y  310

/* Settings screen. */
#define SET_TITLE_Y      40
#define SET_ROW_Y0      100
#define SET_ROW_DY       40
#define SET_BTN_W       300
#define SET_BTN_H        32
#define SET_BACK_Y      290

#define COL_WAIT   RING_COLOR_GREEN
#define COL_CONN   lv_color_hex(0x3A4048)
#define COL_DIM    lv_color_hex(0x9E9E9E)
#define COL_TALK   lv_color_hex(0xFF1744)
#define COL_WS     lv_color_hex(0x448AFF)
#define COL_USER   lv_color_hex(0xFF9800)
#define COL_PANEL  lv_color_hex(0x14181D)
#define COL_ROW    lv_color_hex(0x242A31)
#define COL_RING   lv_color_hex(0x2A2F36)

/* ------------------------------------------------------------- settings
 *
 * Session settings that survive a reboot via NVS. The Qianwen API only
 * accepts turn_detection / voice while IDLE, so changing them reconnects the
 * session (see README.md "Implementation notes"). */

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

/* Screens. */
static lv_obj_t *s_talk_scr;
static lv_obj_t *s_chat_scr;
static lv_obj_t *s_set_scr;

/* Talk screen. */
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_ring;
static lv_obj_t *s_status;
static lv_obj_t *s_talk_user;
static lv_obj_t *s_talk_model;

/* Chat screen. */
static lv_obj_t *s_chat_box;
static lv_obj_t *s_chat_stats;
static lv_obj_t *s_chat_netinfo;

/* Settings screen. */
static lv_obj_t *s_set_rows[4];

static volatile bool s_stop = false;
static volatile bool s_worker_busy = false;
static bool s_ui_ready = false;

/* The Qianwen server closes a session that has not generated a response for
 * 180 s. A session.update every 60 s keeps it alive without producing a
 * reply (verified with scripts/test_qianwen_ws_keepalive.py). */
#define KEEPALIVE_MS 60000
static TickType_t s_last_keepalive_tick;

/* Rolling transcript: the talk screen shows the last exchange, the chat
 * screen the full history. The buffers accumulate streaming deltas and are
 * replaced by the complete text on the .done/.completed events. */
#define CHAT_TEXT_MAX 256
static char s_talk_user_buf[CHAT_TEXT_MAX];
static char s_talk_model_buf[CHAT_TEXT_MAX];
static bool s_user_streaming;
static bool s_model_streaming;

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

/* 1 Hz wire/battery readout. */
static uint32_t s_last_up;
static uint32_t s_last_down;
static uint32_t s_last_ms;
static bool s_stats_ready;

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

/* The radial level ring: value 0..100, colour follows the state. */
static void ui_level(uint8_t pct, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_arc_set_value(s_ring, pct);
        lv_obj_set_style_arc_color(s_ring, color, LV_PART_INDICATOR);
    }
}

static void ui_talk_transcript(void)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_talk_user, s_talk_user_buf);
        if (s_talk_model_buf[0]) {
            lv_label_set_text(s_talk_model, s_talk_model_buf);
        } else if (s_talk_user_buf[0] == '\0') {
            lv_label_set_text(s_talk_model, "hold = talk · release = reply");
        } else {
            lv_label_set_text(s_talk_model, "");
        }
    }
}

static void ui_netinfo(const char *text, lv_color_t color)
{
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_chat_netinfo, text);
        lv_obj_set_style_text_color(s_chat_netinfo, color, LV_PART_MAIN);
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

/* Update the chat screen's net line with Wi-Fi + WebSocket state. Snapshot
 * the shared strings under the lock — an unsynchronised snprintf can tear. */
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
        lv_arc_set_value(s_ring, 0);
        lv_obj_set_style_arc_color(s_ring, COL_WAIT, LV_PART_INDICATOR);
        lv_label_set_text(s_status, status ? status : "press & hold to talk");
        lv_obj_set_style_text_color(s_status, COL_DIM, LV_PART_MAIN);
        ui_talk_transcript();
    }
}

/* ------------------------------------------------------------- chat log */

static void chat_append(bool is_user, const char *text)
{
    if (!s_ui_ready || text[0] == '\0') {
        return;
    }
    RING_UI_LOCKED(-1) {
        lv_obj_t *lbl = lv_label_create(s_chat_box);
        lv_obj_set_width(lbl, CHAT_LABEL_W);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, is_user ? COL_USER : COL_WS, LV_PART_MAIN);
        lv_label_set_text(lbl, text);
        lv_obj_update_layout(s_chat_box);
        lv_obj_scroll_to_y(s_chat_box, LV_COORD_MAX, LV_ANIM_OFF);
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

/* ------------------------------------------------------------- navigation */

static void go_chat(void)
{
    lv_screen_load_anim(s_chat_scr, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void go_settings(void)
{
    lv_screen_load_anim(s_set_scr, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void go_talk(void)
{
    lv_screen_load_anim(s_talk_scr, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void nav_back_cb(lv_event_t *e)
{
    (void)e;
    go_talk();
}

static void go_chat_cb(lv_event_t *e)
{
    (void)e;
    go_chat();
}

static void go_settings_cb(lv_event_t *e)
{
    (void)e;
    go_settings();
}

/* ------------------------------------------------------------- stats timer */

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
    RING_UI_LOCKED(-1) {
        lv_label_set_text(s_chat_stats, line);
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
        ui_level(0, COL_TALK);
        break;
    case RING_WS_SPEAKING:
        set_ws_state("reply…");
        ui_update_netinfo(COL_WS);
        ui_button("REPLY", &lv_font_montserrat_48, COL_WS);
        /* The half-duplex gate mutes the mic while the model talks. */
        ui_status("replying… · mic off", COL_WS);
        ui_level(100, COL_WS);
        break;
    case RING_WS_CONNECTING:
        set_ws_state("WS…");
        ui_update_netinfo(COL_DIM);
        ui_button("···", &lv_font_montserrat_48, COL_CONN);
        ui_status("connecting…", COL_DIM);
        ui_level(0, COL_DIM);
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
            ui_level(0, COL_TALK);
        }
        break;
    }
    case RING_WS_OFF:
    default:
        set_ws_state("");
        break;
    }
}

static void ws_text_cb(const char *text, size_t len, bool is_user, bool final,
                       void *user)
{
    (void)user;
    if (!s_ui_ready) {
        return;
    }
    char *buf = is_user ? s_talk_user_buf : s_talk_model_buf;
    bool *streaming = is_user ? &s_user_streaming : &s_model_streaming;

    if (final) {
        /* The .done/.completed events carry the complete text. */
        size_t n = len < CHAT_TEXT_MAX - 1 ? len : CHAT_TEXT_MAX - 1;
        memcpy(buf, text, n);
        buf[n] = '\0';
        *streaming = false;
        chat_append(is_user, buf);
    } else {
        /* Streaming deltas are incremental. */
        if (!*streaming) {
            buf[0] = '\0';
            *streaming = true;
        }
        size_t cur = strlen(buf);
        if (cur < CHAT_TEXT_MAX - 1) {
            size_t n = len < CHAT_TEXT_MAX - 1 - cur ? len : CHAT_TEXT_MAX - 1 - cur;
            memcpy(buf + cur, text, n);
            buf[cur + n] = '\0';
        }
    }
    ui_talk_transcript();
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
    TickType_t last_level = last;

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
            if (now - last_level >= pdMS_TO_TICKS(LEVEL_UPDATE_MS)) {
                last_level = now;
                ui_level(level_of(s_reply, got), COL_WS);
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
    s_talk_user_buf[0] = '\0';
    s_talk_model_buf[0] = '\0';
    s_user_streaming = s_model_streaming = false;
    ui_talk_transcript();

    /* Uplink: one 20 ms frame at a time until released. */
    uint32_t frames = 0;
    TickType_t last_level = xTaskGetTickCount();
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
        if (now - last_level >= pdMS_TO_TICKS(LEVEL_UPDATE_MS)) {
            last_level = now;
            ui_level(level_of(s_mono, FRAME_SAMPLES), COL_TALK);
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

/* A top bar with a left icon button, a centred title and an optional right
 * icon button. Returns the title so callers can style it. */
static lv_obj_t *build_topbar(lv_obj_t *scr, const char *left_sym,
                              const char *title, const char *right_sym,
                              lv_event_cb_t left_cb, lv_event_cb_t right_cb)
{
    if (left_sym) {
        lv_obj_t *b = lv_label_create(scr);
        lv_label_set_text(b, left_sym);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(b, COL_DIM, LV_PART_MAIN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        ring_ui_place_centered(b, 100, TOPBAR_Y);
        if (left_cb) {
            lv_obj_add_event_cb(b, left_cb, LV_EVENT_CLICKED, NULL);
        }
    }

    lv_obj_t *t = ring_ui_add_label(scr, title, &lv_font_montserrat_20,
                                    RING_COLOR_GREEN);
    ring_ui_place_centered(t, 182, TOPBAR_Y);

    if (right_sym) {
        lv_obj_t *b = lv_label_create(scr);
        lv_label_set_text(b, right_sym);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(b, COL_DIM, LV_PART_MAIN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        ring_ui_place_centered(b, 258, TOPBAR_Y);
        if (right_cb) {
            lv_obj_add_event_cb(b, right_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    return t;
}

static void build_talk(void)
{
    build_topbar(s_talk_scr, LV_SYMBOL_LIST, "QIANWEN WS", LV_SYMBOL_SETTINGS,
                 go_chat_cb, go_settings_cb);

    /* Radial level ring, behind the button. */
    s_ring = lv_arc_create(s_talk_scr);
    lv_obj_set_size(s_ring, RING_SZ, RING_SZ);
    lv_obj_set_pos(s_ring, BTN_CX - RING_SZ / 2, BTN_CY - RING_SZ / 2);
    lv_arc_set_range(s_ring, 0, 100);
    lv_arc_set_value(s_ring, 0);
    lv_arc_set_mode(s_ring, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_rotation(s_ring, 270);   /* 0 % at 12 o'clock */
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_ring, RING_WEAR, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, COL_RING, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_WEAR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, COL_WAIT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* The talk button. */
    s_btn = lv_obj_create(s_talk_scr);
    lv_obj_set_size(s_btn, BTN_SZ, BTN_SZ);
    lv_obj_set_pos(s_btn, BTN_CX - BTN_SZ / 2, BTN_CY - BTN_SZ / 2);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_btn, COL_CONN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_CONN, 60),
                              LV_STATE_PRESSED);
    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_label = ring_ui_add_label(s_btn, "···", &lv_font_montserrat_48,
                                    lv_color_white());
    lv_obj_center(s_btn_label);

    /* State line. */
    s_status = ring_ui_add_label(s_talk_scr, "connecting…",
                                 &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(s_status, STATUS_W);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    ring_ui_place_centered(s_status, RING_CX, STATUS_Y);

    /* Last exchange: user above, model below. Two wrapped lines each. */
    s_talk_user = ring_ui_add_label(s_talk_scr, "", &lv_font_montserrat_14,
                                    COL_USER);
    lv_obj_set_size(s_talk_user, TALK_TEXT_W, TALK_TEXT_H);
    lv_obj_set_pos(s_talk_user, RING_CX - TALK_TEXT_W / 2, TALK_USER_Y);
    lv_label_set_long_mode(s_talk_user, LV_LABEL_LONG_WRAP);

    s_talk_model = ring_ui_add_label(s_talk_scr, "hold = talk · release = reply",
                                     &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_size(s_talk_model, TALK_TEXT_W, TALK_TEXT_H);
    lv_obj_set_pos(s_talk_model, RING_CX - TALK_TEXT_W / 2, TALK_MODEL_Y);
    lv_label_set_long_mode(s_talk_model, LV_LABEL_LONG_WRAP);

    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void build_chat(void)
{
    build_topbar(s_chat_scr, LV_SYMBOL_LEFT, "CHAT", NULL, nav_back_cb, NULL);

    s_chat_box = lv_obj_create(s_chat_scr);
    lv_obj_set_size(s_chat_box, CHAT_W, CHAT_H);
    lv_obj_set_pos(s_chat_box, RING_CX - CHAT_W / 2, CHAT_Y);
    lv_obj_set_flex_flow(s_chat_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_chat_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_chat_box, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chat_box, lv_color_hex(0x1A1F26), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chat_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_chat_box, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chat_box, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_chat_box, LV_OBJ_FLAG_SCROLLABLE);

    s_chat_stats = ring_ui_add_label(s_chat_scr, "",
                                     &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(s_chat_stats, CHAT_W);
    lv_obj_set_style_text_align(s_chat_stats, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_chat_stats, LV_LABEL_LONG_DOT);
    ring_ui_place_centered(s_chat_stats, RING_CX, CHAT_STATS_Y);

    s_chat_netinfo = ring_ui_add_label(s_chat_scr, "",
                                       &lv_font_montserrat_14, COL_DIM);
    /* y=310 is only ~249 px wide; keep the box inside the circle. */
    lv_obj_set_width(s_chat_netinfo, 240);
    lv_obj_set_style_text_align(s_chat_netinfo, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_chat_netinfo, LV_LABEL_LONG_DOT);
    ring_ui_place_centered(s_chat_netinfo, RING_CX, CHAT_NETINFO_Y);
}

static void build_settings(void)
{
    build_topbar(s_set_scr, LV_SYMBOL_LEFT, "SETTINGS", NULL, nav_back_cb, NULL);

    for (int i = 0; i < 4; i++) {
        s_set_rows[i] = lv_label_create(s_set_scr);
        lv_obj_set_size(s_set_rows[i], SET_BTN_W, SET_BTN_H);
        lv_obj_set_pos(s_set_rows[i], RING_CX - SET_BTN_W / 2,
                       SET_ROW_Y0 + i * SET_ROW_DY);
        lv_obj_set_style_text_align(s_set_rows[i], LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_set_rows[i], COL_ROW, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_set_rows[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_set_rows[i], 8, LV_PART_MAIN);
        lv_obj_add_flag(s_set_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_set_rows[i], settings_row_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    settings_refresh();

    lv_obj_t *back = lv_label_create(s_set_scr);
    lv_label_set_text(back, "BACK");
    lv_obj_set_size(back, 120, 32);
    lv_obj_set_pos(back, RING_CX - 60, SET_BACK_Y);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, COL_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 8, LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, nav_back_cb, LV_EVENT_CLICKED, NULL);
}

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
        /* Talk (active) screen. */
        s_talk_scr = ring_ui_screen();
        build_talk();

        /* Chat screen. */
        s_chat_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_chat_scr, RING_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_chat_scr, LV_OPA_COVER, LV_PART_MAIN);
        build_chat();

        /* Settings screen. */
        s_set_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_set_scr, RING_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_set_scr, LV_OPA_COVER, LV_PART_MAIN);
        build_settings();

        /* 1 Hz wire/battery readout. */
        lv_timer_create(stats_timer_cb, 1000, NULL);

        lv_screen_load(s_talk_scr);
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
