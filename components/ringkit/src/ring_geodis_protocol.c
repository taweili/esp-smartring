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
 *
 * Everything in the `peer callbacks` section runs on `ring-geodis-peer`, which
 * also runs esp_peer_main_loop() on a 16 KB stack with DTLS on top. Those
 * callbacks therefore do string work and queue events; they never call
 * application code.
 */
#include "ring_geodis_priv.h"
#include "ring_geodis_payload.h"

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

/* DataChannel frames are read through this much stack first; anything bigger
 * (a long transcript, a fat tool call) gets a heap copy instead of silently
 * losing its tail. */
#define RING_GEODIS_FRAME_STACK 512

/* ------------------------------------------------------------- session.update */

/* Thin wrapper over the host-tested builder: it only supplies the session's
 * configuration. The wire rules live in ring_geodis_payload.c. */
static bool build_session_update(char *buf, size_t cap)
{
    return ring_geodis_payload_session_update(buf, cap, ring_geodis_s.voice,
                                              ring_geodis_s.enable_stt,
                                              ring_geodis_s.enable_ptt);
}

/* ------------------------------------------------------------- peer callbacks
 *
 * Runs on the peer task, which also runs esp_peer_main_loop: keep the stack
 * small, never log big strings, never call application code.
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
        ring_geodis_s.last_err = ESP_FAIL;
        ring_geodis_s.speaking = false;
        /* The uplink died with the session; the next button press must not
         * try to stream into a peer that is gone. */
        ring_geodis_s.uplink_open = false;
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

    ESP_LOGI(TAG, "local SDP ready (%u bytes)", (unsigned)msg->size);
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

/* response.function_call_arguments.done -> a queued job for the tool task.
 *
 * The job lives in a static, not on the peer stack: it is 1.1 KB and this task
 * is also running DTLS. Only the peer task ever writes it. */
static void queue_tool_job(const char *json)
{
    static ring_geodis_tool_job_t job;
    memset(&job, 0, sizeof(job));

    bool t_id = false, t_name = false, t_args = false;
    bool args_present = ring_json_has_key(json, "arguments");
    bool args_read = ring_json_get_str_ex(json, "arguments", job.args_json,
                                          sizeof(job.args_json), NULL, &t_args);
    ring_json_get_str_ex(json, "call_id", job.call_id, sizeof(job.call_id),
                         NULL, &t_id);
    ring_json_get_str_ex(json, "name", job.name, sizeof(job.name), NULL, &t_name);

    if (t_id || job.call_id[0] == '\0') {
        /* Without the id the output cannot be attached to the call; sending it
         * with a made-up id is worse than letting the turn fail. */
        ESP_LOGE(TAG, "tool call without a usable call_id — dropping");
        return;
    }
    if (job.name[0] == '\0' || t_name) {
        snprintf(job.name, sizeof(job.name), "rechercher_base_geodis");
    }

    /* Present but unreadable is a different failure from absent: the arguments
     * may carry the only copy of the question. Never substitute "{}" — that
     * looks up nothing and the model answers as if it had data. */
    if (args_present && (!args_read || t_args)) {
        job.bad_args = 1;
        ESP_LOGE(TAG, "tool '%s': arguments unreadable (%u B frame, %s)",
                 job.name, (unsigned)strlen(json),
                 t_args ? "truncated" : "parse failed");
    } else if (!args_present) {
        job.bad_args = 1;
        ESP_LOGE(TAG, "tool '%s': no arguments field", job.name);
    }

    if (ring_geodis_s.tool_queue != NULL &&
        xQueueSend(ring_geodis_s.tool_queue, &job, 0) == pdTRUE) {
        ESP_LOGI(TAG, "tool '%s' queued (%u B args)", job.name,
                 (unsigned)strlen(job.args_json));
    } else {
        ESP_LOGW(TAG, "tool queue unavailable — dropping '%s'", job.name);
    }
}

/* One DataChannel event. Pure string work + queueing; no app code. */
static void handle_event(const char *json)
{
    if (ring_json_type_is(json, "session.created")) {
        ESP_LOGI(TAG, "session created -> configuring");
        char update[512];
        if (!build_session_update(update, sizeof(update))) {
            ESP_LOGE(TAG, "session.update does not fit in %u bytes", (unsigned)sizeof(update));
            ring_geodis_state_set(RING_GEODIS_ERROR);
            return;
        }
        esp_err_t e = ring_geodis_send_event(update);
        if (e != ESP_OK) {
            /* Without session.update the session stays on its preset config
             * (server VAD, no transcription) — fail loudly rather than run a
             * conversation that behaves nothing like the requested one. */
            ESP_LOGE(TAG, "session.update failed: %s", esp_err_to_name(e));
            ring_geodis_s.last_err = e;
            ring_geodis_state_set(RING_GEODIS_ERROR);
            return;
        }
        ESP_LOGI(TAG, "session.update sent");
    } else if (ring_json_type_is(json, "session.updated")) {
        ESP_LOGI(TAG, "session configured");
        ring_geodis_state_set(RING_GEODIS_READY);
    } else if (ring_json_type_is(json, "response.created")) {
        /* Announce the turn before the first audio frame: a text-only reply or
         * a slow RAG lookup would otherwise leave the UI on "ready". */
        ring_geodis_s.speaking = true;
        ring_geodis_state_set(RING_GEODIS_SPEAKING);
    } else if (ring_json_type_is(json, "response.done")) {
        ring_geodis_s.speaking = false;
        if (ring_geodis_state() == RING_GEODIS_SPEAKING) {
            ring_geodis_state_set(RING_GEODIS_READY);
        }
    } else if (ring_json_type_is(json, "error")) {
        char code[64];
        char msg[160];
        ring_json_get_str(json, "code", code, sizeof(code), NULL);
        ring_json_get_str(json, "message", msg, sizeof(msg), NULL);
        ESP_LOGW(TAG, "model error: %s%s%s", code[0] ? code : "?",
                 msg[0] ? " — " : "", msg);
    } else if (ring_json_type_is(json, "conversation.item.input_audio_transcription.completed")) {
        char text[RING_GEODIS_TEXT_MAX];
        size_t len = 0;
        bool trunc = false;
        if (ring_json_get_str_ex(json, "transcript", text, sizeof(text),
                                 &len, &trunc)) {
            ring_geodis_emit_text(text, len, true, true, trunc);
        }
    } else if (ring_json_type_is(json, "response.output_audio_transcript.delta")) {
        char text[RING_GEODIS_TEXT_MAX];
        size_t len = 0;
        bool trunc = false;
        if (ring_json_get_str_ex(json, "delta", text, sizeof(text), &len, &trunc)) {
            ring_geodis_emit_text(text, len, false, false, trunc);
        }
    } else if (ring_json_type_is(json, "response.output_audio_transcript.done")) {
        char text[RING_GEODIS_TEXT_MAX];
        size_t len = 0;
        bool trunc = false;
        if (ring_json_get_str_ex(json, "transcript", text, sizeof(text), &len, &trunc)) {
            ring_geodis_emit_text(text, len, false, true, trunc);
            if (trunc) {
                ESP_LOGW(TAG, "assistant transcript longer than %d B, clipped",
                         RING_GEODIS_TEXT_MAX - 1);
            }
        }
    } else if (ring_json_type_is(json, "response.function_call_arguments.done")) {
        queue_tool_job(json);
    }
}

/* Entry point for the mock transport (and host tests): feed a synthetic
 * DataChannel frame through the real protocol handler. */
void ring_geodis_dispatch_data_frame(const char *json)
{
    if (json != NULL) {
        handle_event(json);
    }
}

/* DataChannel frames arrive as JSON strings. */
int ring_geodis_on_data_frame(esp_peer_data_frame_t *frame, void *ctx)
{    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0) {
        return 0;
    }
    /* Remember which channel the event arrived on; replies go back on it. */
    if (frame->stream_id != 0) {
        ring_geodis_s.send_stream_id = frame->stream_id;
    }

    char stackbuf[RING_GEODIS_FRAME_STACK];
    char *json = stackbuf;
    char *heap = NULL;
    if (frame->size >= (int)sizeof(stackbuf)) {
        heap = malloc((size_t)frame->size + 1);
        if (heap != NULL) {
            json = heap;
        }
    }
    size_t cap = (json == stackbuf) ? sizeof(stackbuf) : (size_t)frame->size + 1;
    int n = (frame->size < (int)cap - 1) ? frame->size : (int)cap - 1;
    memcpy(json, frame->data, (size_t)n);
    json[n] = '\0';

    /* DEBUG, not INFO: this is the user's spoken question and the assistant's
     * answer, and the console blocks when the host stops reading it. */
    ESP_LOGD(TAG, "dc: %.160s%s", json, (heap != NULL) ? " …" : "");

    handle_event(json);
    free(heap);
    return 0;
}

/* ------------------------------------------------------------- send */

#ifndef CONFIG_RINGKIT_GEODIS_MOCK
/* A real session needs no observation hook: the peer sends the event and the
 * server answers. The mock compiles its own definition. */
void ring_geodis_transport_on_event(const char *json, esp_err_t sent)
{
    (void)json;
    (void)sent;
}
#endif

esp_err_t ring_geodis_send_event(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_RINGKIT_GEODIS_MOCK
    /* No peer on the wire. The mock's transport_on_event() is the observer,
     * exactly as esp_peer_send_data() would be in a real session, so commit /
     * cancel / session.update reach it and report success. Without this the
     * mock's s.peer is NULL and every send_event() fails, so commit() never
     * starts a turn. */
    esp_err_t ret = ESP_OK;
#else
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ring_geodis_s.peer_lock, portMAX_DELAY);
    if (ring_geodis_s.peer != NULL) {
        esp_peer_data_frame_t frame = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .stream_id = ring_geodis_s.send_stream_id,
            .data = (uint8_t *)json,
            .size = (int)strlen(json),
        };
        int r = esp_peer_send_data(ring_geodis_s.peer, &frame);
        if (r == ESP_PEER_ERR_NONE) {
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "esp_peer_send_data(stream %u) failed: %d",
                     ring_geodis_s.send_stream_id, r);
            ret = ESP_FAIL;
        }
    }
    xSemaphoreGive(ring_geodis_s.peer_lock);
#endif /* CONFIG_RINGKIT_GEODIS_MOCK */
    /* The mock transport has no wire to observe, so give it the same view the
     * server would get. A no-op in a real build. */
    ring_geodis_transport_on_event(json, ret);
    return ret;
}

esp_err_t ring_geodis_commit(void)
{
    ring_geodis_s.commit_ms = ring_geodis_now_ms();
    ring_geodis_s.commit_pending = true;
    esp_err_t e1 = ring_geodis_send_event("{\"type\":\"input_audio_buffer.commit\"}");
    esp_err_t e2 = ring_geodis_send_event("{\"type\":\"response.create\"}");
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t ring_geodis_cancel(void)
{
    esp_err_t ret = ESP_OK;
    if (ring_geodis_s.speaking) {
        ret = ring_geodis_send_event("{\"type\":\"response.cancel\"}");
    }
    ring_geodis_s.speaking = false;
    /* Drop the audio already decoded into the playback buffer, or the ring
     * keeps talking after it was cancelled (and feeds its own microphone). */
    ring_byte_ring_reset(&ring_geodis_s.dl_ring);
    /* Back to READY even if the cancel send failed or response.done is lost:
     * the alternative is a session stuck in SPEAKING with the uplink closed. */
    if (ring_geodis_state() == RING_GEODIS_SPEAKING) {
        ring_geodis_state_set(RING_GEODIS_READY);
    }
    return ret;
}

esp_err_t ring_geodis_keepalive(void)
{
    if (ring_geodis_state() != RING_GEODIS_READY || ring_geodis_s.speaking) {
        return ESP_ERR_INVALID_STATE;
    }
    char json[512];
    if (!build_session_update(json, sizeof(json))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ring_geodis_send_event(json);
}

/* ------------------------------------------------------------- tool task */

/* Build the function_call_output item into `out` and send it. */
static void send_tool_output(const char *call_id, const char *output, char *out,
                             size_t cap)
{
    if (!ring_geodis_payload_tool_output(out, cap, call_id, output)) {
        ESP_LOGE(TAG, "tool output item does not fit in %u bytes — dropping",
                 (unsigned)cap);
        return;
    }
    ring_geodis_send_event(out);
    ring_geodis_send_event("{\"type\":\"response.create\"}");
}

/* A failed lookup must be actionable for the model: the old text ("tool-call
 * failed: HTTP 200") was paraphrased into a confident denial. */
static void tool_error_output(char *inner, size_t cap, const char *detail)
{
    snprintf(inner, cap,
             "{\"error\":\"lookup_unavailable\",\"detail\":\"%s\","
             "\"instruction\":\"tell the user the lookup failed and to try again\"}",
             detail);
}

/* Report the lookup result to the app. Runs on this task, never the peer task,
 * so the callback may take the LVGL lock. */
static void tool_done_notify(const ring_geodis_tool_job_t *job, bool ok,
                             int status, int chunks)
{
    if (ring_geodis_s.tool_done_cb != NULL) {
        ring_geodis_s.tool_done_cb(job->name, ok, status, chunks,
                                   ring_geodis_s.user);
    }
}

static void tool_exec(const ring_geodis_tool_job_t *job, char *resp, char *inner,
                      size_t cap)
{
    const char *output;
    bool ok = false;
    int status = 0;
    int chunks = -1;

    if (job->bad_args) {
        tool_error_output(inner, RING_GEODIS_TOOL_INNER_CAP, "arguments_unreadable");
        send_tool_output(job->call_id, inner, resp, cap);
        tool_done_notify(job, false, 0, -1);
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/tool-call", ring_geodis_s.base_url);

    /* `arguments` must be a JSON string, not an object. The event's
     * `arguments` field is already a string; embed it verbatim. The escape
     * can roughly double it, so leave room and check the result. */
    char body[RING_GEODIS_TOOL_ARGS_MAX * 2 + 512];
    if (!ring_geodis_payload_tool_body(body, sizeof(body), job->name,
                                       job->args_json, ring_geodis_s.session_id)) {
        ESP_LOGE(TAG, "tool '%s': request body overflow", job->name);
        tool_error_output(inner, RING_GEODIS_TOOL_INNER_CAP, "request_too_large");
        send_tool_output(job->call_id, inner, resp, cap);
        tool_done_notify(job, false, 0, -1);
        return;
    }

    /* Two attempts, not three: somebody is listening to silence while this
     * runs, and a second timeout is worse than an honest failure. */
    uint32_t t0 = ring_geodis_now_ms();
    esp_err_t err = ring_geodis_post_json_ex(url, body, 200, resp, cap, NULL,
                                             &status, 2);
    ring_geodis_s.stats.tool_ms = ring_geodis_now_ms() - t0;

    bool trunc = false;
    size_t inner_len = 0;
    bool have_text = (err == ESP_OK) &&
                     ring_json_get_str_ex(resp, "text", inner,
                                          RING_GEODIS_TOOL_INNER_CAP,
                                          &inner_len, &trunc);
    if (have_text && !trunc && inner_len > 0 && inner[0] == '{') {
        output = inner;
        ok = true;
        ring_json_count_array(inner, "retrievedChunks", &chunks);
        ESP_LOGI(TAG, "tool '%s': HTTP %d, %u B inner JSON, %d chunks",
                 job->name, status, (unsigned)inner_len, chunks);
    } else {
        const char *detail = (err != ESP_OK) ? "http_error"
                            : trunc           ? "response_too_large"
                                              : "unparseable_response";
        tool_error_output(inner, RING_GEODIS_TOOL_INNER_CAP, detail);
        output = inner;
        ESP_LOGW(TAG, "tool '%s': %s (HTTP %d)", job->name, detail, status);
    }

    send_tool_output(job->call_id, output, resp, cap);
    tool_done_notify(job, ok, status, chunks);
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
        if (xQueueReceive(ring_geodis_s.tool_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ring_geodis_s.stats.tool_calls++;
        /* The app hook runs here, on this task, never on the peer task: the
         * contract says a callback may take the LVGL lock, and this is the only
         * place where blocking is harmless (one call at a time, and the RAG
         * lookup would block the same turn anyway). */
        if (ring_geodis_s.tool_cb != NULL &&
            ring_geodis_s.tool_cb(job.name, job.args_json, ring_geodis_s.user)) {
            ESP_LOGI(TAG, "tool '%s' handled by the app", job.name);
            continue;
        }
        tool_exec(&job, resp, inner, RING_GEODIS_TOOL_RESP_CAP);
    }
}
