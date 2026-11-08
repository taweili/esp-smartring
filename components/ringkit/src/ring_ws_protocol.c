/*
 * ringkit — ring_ws session/turn JSON protocol and uplink.
 *
 * Split out of ring_ws.c. The worker task parses completed text messages
 * here: session lifecycle, turn events, transcripts, function calls, errors
 * (with the smart_turn_v2 → server_vad fallback) and the base64 audio deltas.
 * The uplink audio path lives in ring_ws_transport.c.
 * See ring_ws_priv.h for the shared state.
 */
#include "ring_ws_priv.h"

#include "ringkit/ring_json.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ring_ws"

/* ------------------------------------------------------------- protocol send */

esp_err_t ring_ws_send_json(const char *json)
{
    if (ring_ws_s.client == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(json);
    int w = esp_websocket_client_send_text(ring_ws_s.client, json, len,
                                           pdMS_TO_TICKS(RING_WS_SEND_TIMEOUT_MS));
    if (w < 0) {
        return ESP_FAIL;
    }
    ring_ws_s.bytes_up += (uint32_t)w;
    return ESP_OK;
}

/* Append formatted text to a fixed JSON buffer; returns false on overflow.
 * `snprintf` reports the would-be length, so a plain `n += snprintf(...)`
 * would underflow `sizeof(json) - n` on the next call and write out of
 * bounds. */
static bool json_append(char *buf, size_t cap, int *pos, const char *fmt, ...)
{
    if (*pos < 0 || (size_t)*pos >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *pos, cap - (size_t)*pos, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - (size_t)*pos) {
        *pos = (int)cap;
        return false;
    }
    *pos += w;
    return true;
}

/* Build and send a session.update event from the stored cfg.
 * If `turn_detection` is non-NULL it overrides cfg.turn_detection. */
esp_err_t ring_ws_send_session_update(const char *turn_detection_override)
{
    char json[1024];
    int n = 0;
    const ring_ws_session_cfg_t *c = &ring_ws_s.cfg;
    const char *td = turn_detection_override ? turn_detection_override : c->turn_detection;

    if (!json_append(json, sizeof(json), &n,
                     "{\"type\":\"session.update\",\"session\":{"
                     "\"modalities\":[\"text\",\"audio\"]")) {
        goto overflow;
    }
    if (c->voice &&
        !json_append(json, sizeof(json), &n, ",\"voice\":\"%s\"", c->voice)) {
        goto overflow;
    }
    if (c->instructions) {
        if (!json_append(json, sizeof(json), &n, ",\"instructions\":")) {
            goto overflow;
        }
        if (!ring_json_append_string(json, sizeof(json), &n, c->instructions)) {
            goto overflow;
        }
    }
    if (c->speech_rate > 0.0f &&
        !json_append(json, sizeof(json), &n, ",\"speech_rate\":%.2f", c->speech_rate)) {
        goto overflow;
    }
    if (c->max_history_turns > 0 &&
        !json_append(json, sizeof(json), &n, ",\"max_history_turns\":%d",
                     c->max_history_turns)) {
        goto overflow;
    }
    if (c->enable_search &&
        !json_append(json, sizeof(json), &n, ",\"enable_search\":true")) {
        goto overflow;
    }
    if (c->input_language &&
        !json_append(json, sizeof(json), &n,
                     ",\"input_audio_transcription\":{\"language\":\"%s\"}",
                     c->input_language)) {
        goto overflow;
    }
    if (c->output_language &&
        !json_append(json, sizeof(json), &n,
                     ",\"output_audio\":{\"language\":\"%s\"}",
                     c->output_language)) {
        goto overflow;
    }
    if (td == NULL) {
        if (!json_append(json, sizeof(json), &n, ",\"turn_detection\":null")) {
            goto overflow;
        }
    } else if (strcmp(td, "server_vad") == 0) {
        if (!json_append(json, sizeof(json), &n,
                         ",\"turn_detection\":{\"type\":\"server_vad\","
                         "\"threshold\":0.5,\"silence_duration_ms\":800}")) {
            goto overflow;
        }
    } else {
        /* smart_turn / smart_turn_v2: silence_duration_ms is ignored. */
        if (!json_append(json, sizeof(json), &n,
                         ",\"turn_detection\":{\"type\":\"%s\",\"threshold\":0.5}",
                         td)) {
            goto overflow;
        }
    }
    if (c->tools_json &&
        !json_append(json, sizeof(json), &n, ",\"tools\":%s", c->tools_json)) {
        goto overflow;
    }
    if (!json_append(json, sizeof(json), &n, "}}")) {
        goto overflow;
    }

    ESP_LOGI(TAG, "session.update: turn_detection=%s",
             td ? td : "null");
    return ring_ws_send_json(json);

overflow:
    ESP_LOGE(TAG, "session.update too long");
    return ESP_FAIL;
}

/* ------------------------------------------------------------- protocol receive (worker) */

/* Decode a response.audio.delta's base64 PCM (24 kHz mono) and push to ring. */
void ring_ws_handle_audio_delta(const char *json)
{
    /* Find the "delta" string value. The scratch buffer is PSRAM and as big
     * as the reassembly buffer, so a completed message always fits. */
    size_t blen = 0;
    if (!ring_json_get_str(json, "delta", ring_ws_s.b64_text, ring_ws_s.b64_cap, &blen)) {
        return;
    }
    if (blen == 0) {
        return;
    }
    if (blen + 1 >= ring_ws_s.b64_cap) {
        ESP_LOGW(TAG, "audio delta too big (%u B) - dropped", (unsigned)blen);
        return;
    }

    int n = ring_ws_b64_decode(ring_ws_s.b64_text, blen, ring_ws_s.pcm24_scratch, ring_ws_s.pcm24_cap);
    if (n <= 0) {
        ESP_LOGW(TAG, "audio delta: base64 decode failed (%u B)",
                 (unsigned)blen);
        ring_ws_s.dropped_down += (uint32_t)blen;
        return;
    }
    ring_ws_s.bytes_down += (uint32_t)n;
    ring_ws_dl_push(ring_ws_s.pcm24_scratch, (size_t)n);
}

/* Process a complete JSON message from the worker queue. */
void ring_ws_worker_handle_text(const char *json, int len)
{
    /* DEBUG only: an audio delta is a ~6 KB JSON message and there are dozens
     * per reply. Logging them at INFO floods the USB-Serial-JTAG console,
     * which blocks the WS worker whenever no monitor is attached. */
    ESP_LOGD(TAG, "ws: %.*s", len > 200 ? 200 : len, json);

    if (ring_json_type_is(json, "session.created")) {
        ESP_LOGI(TAG, "session created -> configuring");
        ring_ws_send_session_update(NULL);
        return;
    }
    if (ring_json_type_is(json, "session.updated")) {
        ESP_LOGI(TAG, "session configured");
        ring_ws_state_set(RING_WS_READY);
        return;
    }
    if (ring_json_type_is(json, "response.created")) {
        ring_ws_s.responding = true;
        ring_ws_s.speaking = true;
        ring_ws_s.response_start_ms = ring_ws_now_ms();
        ring_ws_state_t st = ring_ws_state();
        if (st == RING_WS_READY || st == RING_WS_LISTENING) {
            ring_ws_state_set(RING_WS_SPEAKING);
        }
        return;
    }
    if (ring_json_type_is(json, "response.done")) {
        ring_ws_s.responding = false;
        ring_ws_s.speaking = false;
        ring_ws_s.turns++;
        if (ring_ws_state() == RING_WS_SPEAKING) {
            ring_ws_state_set(RING_WS_READY);
        }
        return;
    }
    if (ring_json_type_is(json, "input_audio_buffer.speech_started")) {
        /* In hands-free modes this means the server heard speech while the
         * uplink was open — barge-in. Cancel the current response and flush
         * the downlink. (In PTT mode this event doesn't fire.) */
        if (ring_ws_s.responding) {
            ESP_LOGI(TAG, "barge-in: speech_started");
            ring_ws_cancel();
        }
        return;
    }
    if (ring_json_type_is(json, "input_audio_buffer.speech_stopped") ||
        ring_json_type_is(json, "input_audio_buffer.committed")) {
        return;
    }
    if (ring_json_type_is(json, "response.audio.delta")) {
        ring_ws_handle_audio_delta(json);
        return;
    }
    if (ring_json_type_is(json, "response.audio_transcript.delta") ||
        ring_json_type_is(json, "response.text.delta")) {
        if (ring_ws_s.text_cb) {
            char text[256];
            size_t tlen = 0;
            if (ring_json_get_str(json, "delta", text, sizeof(text), &tlen)) {
                ring_ws_s.text_cb(text, tlen, false, ring_ws_s.user);
            }
        }
        return;
    }
    if (ring_json_type_is(json, "response.audio_transcript.done")) {
        char text[256];
        size_t tlen = 0;
        /* The Qianwen/OpenAI shape calls the finished string "transcript";
         * "text" is accepted as a fallback. */
        if (ring_json_get_str(json, "transcript", text, sizeof(text), &tlen) ||
            ring_json_get_str(json, "text", text, sizeof(text), &tlen)) {
            ESP_LOGI(TAG, "reply: %s", text);
            if (ring_ws_s.text_cb) {
                ring_ws_s.text_cb(text, tlen, false, ring_ws_s.user);
            }
        }
        return;
    }
    if (ring_json_type_is(json, "conversation.item.input_audio_transcription.completed")) {
        char text[256];
        size_t tlen = 0;
        if (ring_json_get_str(json, "transcript", text, sizeof(text), &tlen)) {
            ESP_LOGI(TAG, "heard: %s", text);
            if (ring_ws_s.text_cb) {
                ring_ws_s.text_cb(text, tlen, true, ring_ws_s.user);
            }
        }
        return;
    }
    if (ring_json_type_is(json, "response.function_call_arguments.done")) {
        if (ring_ws_s.tool_cb) {
            char name[64] = {0};
            char args[512] = {0};
            size_t nl = 0, al = 0;
            ring_json_get_str(json, "name", name, sizeof(name), &nl);
            ring_json_get_str(json, "arguments", args, sizeof(args), &al);
            if (nl > 0) {
                ring_ws_s.tool_cb(name, args, ring_ws_s.user);
            }
        }
        return;
    }
    if (ring_json_type_is(json, "error")) {
        char etype[64] = {0};
        char eparam[128] = {0};
        size_t el = 0;
        ring_json_get_str(json, "type", etype, sizeof(etype), &el);  /* error.type is nested */
        /* The nested error block: re-search from after "error" */
        const char *ep = strstr(json, "\"error\"");
        if (ep) {
            ring_json_get_str(ep, "type", etype, sizeof(etype), &el);
            ring_json_get_str(ep, "param", eparam, sizeof(eparam), &el);
        }
        ESP_LOGW(TAG, "error: type=%s param=%s", etype, eparam);
        /* smart_turn_v2 -> server_vad fallback on a turn_detection error,
         * only before any audio was sent (still IDLE/CONNECTING/READY). */
        ring_ws_state_t st = ring_ws_state();
        if (strstr(eparam, "turn_detection") &&
            st != RING_WS_LISTENING && st != RING_WS_SPEAKING &&
            ring_ws_s.cfg.turn_detection != NULL &&
            strcmp(ring_ws_s.cfg.turn_detection, "server_vad") != 0) {
            ESP_LOGI(TAG, "falling back to server_vad");
            snprintf(ring_ws_s.turn_mode, sizeof(ring_ws_s.turn_mode), "server_vad");
            ring_ws_send_session_update("server_vad");
        } else if (strcmp(etype, "server_error") == 0) {
            ring_ws_state_set(RING_WS_ERROR);
        }
        return;
    }
}

void ring_ws_worker_handle_lifecycle(ring_ws_msg_t *m)
{
    switch (m->err_type) {
    case 0:  /* CONNECTED */
        ESP_LOGI(TAG, "ws connected");
        break;
    case WEBSOCKET_ERROR_TYPE_HANDSHAKE:
        ESP_LOGE(TAG, "ws handshake failed (auth?)");
        ring_ws_state_set(RING_WS_ERROR);
        break;
    case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT:
    case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:
    case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE:
        ESP_LOGW(TAG, "ws disconnected (err=%d)", m->err_type);
        if (ring_ws_state() != RING_WS_OFF) {
            ring_ws_state_set(RING_WS_ERROR);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- turn control */

esp_err_t ring_ws_commit(void)
{
    if (ring_ws_s.client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e1 = ring_ws_send_json("{\"type\":\"input_audio_buffer.commit\"}");
    esp_err_t e2 = ring_ws_response_create();
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t ring_ws_response_create(void)
{
    return ring_ws_send_json("{\"type\":\"response.create\"}");
}

esp_err_t ring_ws_cancel(void)
{
    if (!ring_ws_s.responding) {
        return ESP_OK;   /* B-7: no-op unless speaking */
    }
    esp_err_t e = ring_ws_send_json("{\"type\":\"response.cancel\"}");
    /* Flush downlink so playback stops immediately. */
    ring_byte_ring_reset(&ring_ws_s.dl_ring);
    ring_ws_s.responding = false;
    ring_ws_s.speaking = false;
    if (ring_ws_state() == RING_WS_SPEAKING) {
        ring_ws_state_set(RING_WS_READY);
    }
    return e;
}
