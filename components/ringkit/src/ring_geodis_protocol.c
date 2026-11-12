/*
 * ringkit — ring_geodis DataChannel JSON protocol and session callbacks.
 *
 * Split out of ring_geodis.c. The session is configured when the server sends
 * session.created; events arrive as JSON strings on the DataChannel and
 * replies must go back on the same SCTP stream. See ring_geodis_priv.h for the
 * shared state.
 *
 * The backend does NOT execute the RAG tool call: on
 * response.function_call_arguments.done this module POSTs /tool-call itself
 * (on a dedicated task — the HTTPS round trip must not block the peer thread)
 * and writes the result back as a function_call_output.
 */
#include "ring_geodis_priv.h"

#include "ringkit/ring_json.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "ring_geodis"

/* The /tool-call response is ~10 KB raw / ~9 KB inner JSON. Allocate the
 * scratch buffers once on the tool task (PSRAM) rather than on its stack. */
#define RING_GEODIS_TOOL_RESP_CAP  32768
#define RING_GEODIS_TOOL_INNER_CAP 16384

/* ------------------------------------------------------------- session.update */

static void build_session_update(char *buf, size_t cap)
{
    int pos = 0;
    ring_json_append(buf, cap, &pos,
                     "{\"type\":\"session.update\",\"session\":{\"audio\":{");
    bool need_comma = false;
    if (ring_geodis_s.enable_stt || ring_geodis_s.enable_ptt) {
        ring_json_append(buf, cap, &pos, "\"input\":{");
        bool in_comma = false;
        if (ring_geodis_s.enable_stt) {
            ring_json_append(buf, cap, &pos,
                             "\"transcription\":{\"model\":\"whisper-1\"}");
            in_comma = true;
        }
        if (ring_geodis_s.enable_ptt) {
            if (in_comma) {
                ring_json_append(buf, cap, &pos, ",");
            }
            ring_json_append(buf, cap, &pos, "\"turn_detection\":null");
        }
        ring_json_append(buf, cap, &pos, "}");
        need_comma = true;
    }
    if (ring_geodis_s.voice[0] != '\0') {
        if (need_comma) {
            ring_json_append(buf, cap, &pos, ",");
        }
        ring_json_append(buf, cap, &pos, "\"output\":{\"voice\":");
        ring_json_append_string(buf, cap, &pos, ring_geodis_s.voice);
        ring_json_append(buf, cap, &pos, "}");
    }
    ring_json_append(buf, cap, &pos, "}}}");
}

/* ------------------------------------------------------------- peer callbacks
 *
 * Everything here runs on the peer task, which also runs
 * esp_peer_main_loop: keep the stack small and never log big strings.
 */

int ring_geodis_on_state(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case ESP_PEER_STATE_CONNECTED:
        ESP_LOGI(TAG, "peer connected");
        break;
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
        ESP_LOGI(TAG, "data channel open");
        break;
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_CONNECT_FAILED:
        ESP_LOGW(TAG, "peer lost (%d)", state);
        ring_geodis_state_set(RING_GEODIS_ERROR);
        break;
    default:
        break;
    }
    return 0;
}

int ring_geodis_on_msg(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP || msg->data == NULL) {
        return 0;
    }
    if ((size_t)msg->size >= RING_GEODIS_SDP_MAX_BYTES) {
        ESP_LOGE(TAG, "local SDP too big");
        return 0;
    }
    xSemaphoreTake(ring_geodis_s.sdp_mutex, portMAX_DELAY);
    memcpy(ring_geodis_s.local_sdp, msg->data, (size_t)msg->size);
    ring_geodis_s.local_sdp[msg->size] = '\0';
    xSemaphoreGive(ring_geodis_s.sdp_mutex);

    ESP_LOGI(TAG, "local SDP ready (%u bytes)",
             (unsigned)strlen(ring_geodis_s.local_sdp));
    if (ring_geodis_s.signal_task) {
        xTaskNotifyGive(ring_geodis_s.signal_task);
    }
    return 0;
}

int ring_geodis_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "remote audio: codec %d, %lu Hz, %u ch",
             info->codec, (unsigned long)info->sample_rate, info->channel);
    return 0;
}

int ring_geodis_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "data channel '%s' open (stream %u)", ch->label, ch->stream_id);
    ring_geodis_s.send_stream_id = ch->stream_id;
    return 0;
}

/* DataChannel frames arrive as JSON strings. */
int ring_geodis_on_data_frame(esp_peer_data_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0) {
        return 0;
    }
    /* Remember which channel the event arrived on; replies go back on it. */
    if (frame->stream_id != 0) {
        ring_geodis_s.send_stream_id = frame->stream_id;
    }

    /* Bounded copy on the peer thread: no unbounded heap churn there, and
     * keep the frame small — this runs on the 16 KB peer task that also runs
     * esp_peer_main_loop (DTLS/mbedTLS). */
    char json[1024];
    int n = frame->size < (int)sizeof(json) - 1 ? frame->size : (int)sizeof(json) - 1;
    memcpy(json, frame->data, (size_t)n);
    json[n] = '\0';

    /* Bounded at INFO: this fires on every transcript delta, and the console
     * is USB-Serial-JTAG, whose writes block when the host stops reading. */
    ESP_LOGI(TAG, "dc: %.200s", json);

    if (ring_json_type_is(json, "session.created")) {
        ESP_LOGI(TAG, "session created -> configuring");
        char update[512];
        build_session_update(update, sizeof(update));
        esp_err_t e = ring_geodis_send_event(update);
        ESP_LOGI(TAG, "session.update sent: %s", esp_err_to_name(e));
    } else if (ring_json_type_is(json, "session.updated")) {
        ESP_LOGI(TAG, "session configured");
        ring_geodis_state_set(RING_GEODIS_READY);
    } else if (ring_json_type_is(json, "response.done")) {
        ring_geodis_s.speaking = false;
        if (ring_geodis_state() == RING_GEODIS_SPEAKING) {
            ring_geodis_state_set(RING_GEODIS_READY);
        }
    } else if (ring_json_type_is(json, "error")) {
        ESP_LOGE(TAG, "model error: %.200s", json);
    } else if (ring_json_type_is(json, "conversation.item.input_audio_transcription.completed")) {
        char text[256];
        size_t len = 0;
        if (ring_geodis_s.text_cb != NULL &&
            ring_json_get_str(json, "transcript", text, sizeof(text), &len)) {
            ring_geodis_s.text_cb(text, len, true, ring_geodis_s.user);
        }
    } else if (ring_json_type_is(json, "response.output_audio_transcript.delta")) {
        char text[128];
        size_t len = 0;
        if (ring_geodis_s.text_cb != NULL &&
            ring_json_get_str(json, "delta", text, sizeof(text), &len)) {
            ring_geodis_s.text_cb(text, len, false, ring_geodis_s.user);
        }
    } else if (ring_json_type_is(json, "response.output_audio_transcript.done")) {
        char text[256];
        size_t len = 0;
        if (ring_geodis_s.text_cb != NULL &&
            ring_json_get_str(json, "transcript", text, sizeof(text), &len)) {
            ring_geodis_s.text_cb(text, len, false, ring_geodis_s.user);
        }
    } else if (ring_json_type_is(json, "response.function_call_arguments.done")) {
        ring_geodis_tool_job_t job;
        memset(&job, 0, sizeof(job));
        ring_json_get_str(json, "call_id", job.call_id, sizeof(job.call_id), NULL);
        ring_json_get_str(json, "name", job.name, sizeof(job.name), NULL);
        ring_json_get_str(json, "arguments", job.args_json, sizeof(job.args_json), NULL);
        if (job.call_id[0] == '\0') {
            snprintf(job.call_id, sizeof(job.call_id), "call");
        }
        if (job.name[0] == '\0') {
            snprintf(job.name, sizeof(job.name), "rechercher_base_geodis");
        }
        if (job.args_json[0] == '\0') {
            snprintf(job.args_json, sizeof(job.args_json), "{}");
        }
        /* App hook. It runs on the peer task, so it must not take the LVGL
         * lock; return true only when the app will send the output itself. */
        if (ring_geodis_s.tool_cb != NULL &&
            ring_geodis_s.tool_cb(job.name, job.args_json, ring_geodis_s.user)) {
            ESP_LOGI(TAG, "tool '%s' handled by the app", job.name);
            return 0;
        }
        if (ring_geodis_s.tool_queue != NULL &&
            xQueueSend(ring_geodis_s.tool_queue, &job, 0) == pdTRUE) {
            ESP_LOGI(TAG, "tool '%s' queued (%u B args)",
                     job.name, (unsigned)strlen(job.args_json));
        } else {
            ESP_LOGW(TAG, "tool queue unavailable — dropping '%s'", job.name);
        }
    }
    return 0;
}

/* ------------------------------------------------------------- send */

esp_err_t ring_geodis_send_event(const char *json)
{
    if (ring_geodis_s.peer == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = ring_geodis_s.send_stream_id,
        .data = (uint8_t *)json,
        .size = (int)strlen(json),
    };
    int r = esp_peer_send_data(ring_geodis_s.peer, &frame);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_send_data(stream %u) failed: %d",
                 ring_geodis_s.send_stream_id, r);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ring_geodis_commit(void)
{
    esp_err_t e1 = ring_geodis_send_event("{\"type\":\"input_audio_buffer.commit\"}");
    esp_err_t e2 = ring_geodis_send_event("{\"type\":\"response.create\"}");
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t ring_geodis_cancel(void)
{
    return ring_geodis_send_event("{\"type\":\"response.cancel\"}");
}

esp_err_t ring_geodis_keepalive(void)
{
    if (ring_geodis_state() != RING_GEODIS_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    char json[512];
    build_session_update(json, sizeof(json));
    return ring_geodis_send_event(json);
}

/* ------------------------------------------------------------- tool task */

static void tool_exec(const ring_geodis_tool_job_t *job, char *resp, char *inner,
                      size_t cap)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/tool-call", ring_geodis_s.base_url);

    /* `arguments` must be a JSON string, not an object. The event's
     * `arguments` field is already a string; embed it verbatim. The escape
     * can roughly double it, so leave room. */
    char body[3072];
    int pos = 0;
    ring_json_append(body, sizeof(body), &pos, "{\"name\":");
    ring_json_append_string(body, sizeof(body), &pos, job->name);
    ring_json_append(body, sizeof(body), &pos, ",\"arguments\":");
    ring_json_append_string(body, sizeof(body), &pos, job->args_json);
    ring_json_append(body, sizeof(body), &pos, ",\"session_id\":");
    ring_json_append_string(body, sizeof(body), &pos, ring_geodis_s.session_id);
    ring_json_append(body, sizeof(body), &pos, "}");

    int status = 0;
    esp_err_t err = ring_geodis_post_json(url, body, 200, resp, cap, NULL, &status);

    const char *output = NULL;
    size_t inner_len = 0;
    if (err == ESP_OK &&
        ring_json_get_str(resp, "text", inner, RING_GEODIS_TOOL_INNER_CAP,
                          &inner_len) &&
        inner_len > 0 && inner[0] == '{') {
        output = inner;
        ESP_LOGI(TAG, "tool '%s': HTTP %d, %u B inner JSON",
                 job->name, status, (unsigned)inner_len);
    } else {
        snprintf(inner, RING_GEODIS_TOOL_INNER_CAP,
                 "{\"error\":\"tool-call failed: HTTP %d\"}", status);
        output = inner;
        ESP_LOGW(TAG, "tool '%s' failed: HTTP %d", job->name, status);
    }

    /* The raw response is no longer needed: build the function_call_output
     * item into the same buffer. `output` points at the separate inner buffer,
     * so the append cannot clobber it. */
    pos = 0;
    ring_json_append(resp, cap, &pos,
                     "{\"type\":\"conversation.item.create\",\"item\":"
                     "{\"type\":\"function_call_output\",\"call_id\":");
    ring_json_append_string(resp, cap, &pos, job->call_id);
    ring_json_append(resp, cap, &pos, ",\"output\":");
    ring_json_append_string(resp, cap, &pos, output);
    ring_json_append(resp, cap, &pos, "}}");
    ring_geodis_send_event(resp);
    ring_geodis_send_event("{\"type\":\"response.create\"}");
}

void ring_geodis_tool_task(void *arg)
{
    (void)arg;
    char *resp = heap_caps_malloc(RING_GEODIS_TOOL_RESP_CAP, MALLOC_CAP_SPIRAM);
    char *inner = heap_caps_malloc(RING_GEODIS_TOOL_INNER_CAP, MALLOC_CAP_SPIRAM);
    if (resp == NULL || inner == NULL) {
        ESP_LOGE(TAG, "tool task: no scratch buffers");
        free(resp);
        free(inner);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "tool task started (hwm %u words)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        ring_geodis_tool_job_t job;
        if (xQueueReceive(ring_geodis_s.tool_queue, &job, portMAX_DELAY) == pdTRUE) {
            tool_exec(&job, resp, inner, RING_GEODIS_TOOL_RESP_CAP);
        }
    }
}
