/*
 * ringkit — ring_net DataChannel JSON protocol and session callbacks.
 *
 * Split out of ring_net.c. The session is configured when the server sends
 * session.created; events arrive as JSON strings on the DataChannel and
 * replies must go back on the same SCTP stream. See ring_net_priv.h for the
 * shared state.
 */
#include "ring_net_priv.h"

#include "ringkit/ring_json.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "ring_net"

/* ------------------------------------------------------------- peer callbacks
 *
 * Everything here runs on the peer task, which also runs
 * esp_peer_main_loop: keep the stack small and never log big strings.
 */

int ring_net_on_state(esp_peer_state_t state, void *ctx)
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
        ring_net_state_set(RING_NET_ERROR);
        break;
    default:
        break;
    }
    return 0;
}

int ring_net_on_msg(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP || msg->data == NULL) {
        return 0;
    }
    if ((size_t)msg->size >= RING_NET_SDP_MAX_BYTES) {
        ESP_LOGE(TAG, "local SDP too big");
        return 0;
    }
    xSemaphoreTake(ring_net_s.sdp_mutex, portMAX_DELAY);
    memcpy(ring_net_s.local_sdp, msg->data, (size_t)msg->size);
    ring_net_s.local_sdp[msg->size] = '\0';
    xSemaphoreGive(ring_net_s.sdp_mutex);

    ESP_LOGI(TAG, "local SDP ready (%u bytes)", (unsigned)strlen(ring_net_s.local_sdp));
    /* Full dumps only at DEBUG: the console is USB-Serial-JTAG and its writes
     * block when the host is not reading, so keep the hot path small. */
    ESP_LOGD(TAG, "LOCAL SDP:\n%s", ring_net_s.local_sdp);
    if (ring_net_s.signal_task) {
        xTaskNotifyGive(ring_net_s.signal_task);
    }
    return 0;
}

int ring_net_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "remote audio: codec %d, %lu Hz, %u ch",
             info->codec, (unsigned long)info->sample_rate, info->channel);
    return 0;
}

int ring_net_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "data channel '%s' open (stream %u)", ch->label, ch->stream_id);
    /* Remember the server's channel: replies must go back on it. */
    ring_net_s.send_stream_id = ch->stream_id;
    /* The session is configured when the server sends session.created (see
     * on_data_frame): sending session.update from here races that event. */
    return 0;
}

/* DataChannel frames arrive as JSON strings. */
int ring_net_on_data_frame(esp_peer_data_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0) {
        return 0;
    }
    /* Remember which channel the event arrived on; replies go back on it. */
    if (frame->stream_id != 0) {
        ring_net_s.send_stream_id = frame->stream_id;
    }

    /* Bounded copy on the peer thread: no unbounded heap churn there. */
    char json[1024];
    int n = frame->size < (int)sizeof(json) - 1 ? frame->size : (int)sizeof(json) - 1;
    memcpy(json, frame->data, (size_t)n);
    json[n] = '\0';

    /* Bounded at INFO: this fires on every transcript delta, and the console
     * is USB-Serial-JTAG, whose writes block when the host stops reading. */
    ESP_LOGI(TAG, "dc: %.200s", json);

    if (ring_json_type_is(json, "session.created")) {
        ESP_LOGI(TAG, "session created -> configuring");
        esp_err_t e = ring_net_send_event(
            "{\"type\":\"session.update\",\"session\":{\"modalities\":[\"text\",\"audio\"],"
            "\"voice\":\"longanqian_v3.1\",\"turn_detection\":{\"type\":\"server_vad\","
            "\"threshold\":0.5,\"silence_duration_ms\":800}}}");
        ESP_LOGI(TAG, "session.update sent: %s", esp_err_to_name(e));
    } else if (ring_json_type_is(json, "session.updated")) {
        ESP_LOGI(TAG, "session configured");
        ring_net_state_set(RING_NET_READY);
    } else if (ring_json_type_is(json, "response.done")) {
        ring_net_s.speaking = false;
        if (ring_net_state() == RING_NET_SPEAKING) {
            ring_net_state_set(RING_NET_READY);
        }
    } else if (ring_json_type_is(json, "error")) {
        ESP_LOGE(TAG, "model error: %.200s", json);
    } else if (ring_json_type_is(json, "conversation.item.input_audio_transcription.completed")) {
        /* The server transcribes the user's utterance by default; surfacing it
         * is the clearest evidence that the uplink arrived intact. */
        char text[256];
        size_t len = 0;
        if (ring_net_s.text_cb != NULL &&
            ring_json_get_str(json, "transcript", text, sizeof(text), &len)) {
            ring_net_s.text_cb(text, len, true, ring_net_s.user);
        }
    } else if (ring_net_s.text_cb != NULL &&
               (ring_json_type_is(json, "response.text.delta") ||
                ring_json_type_is(json, "response.audio_transcript.delta"))) {
        char text[128];
        size_t len = 0;
        if (ring_json_get_str(json, "delta", text, sizeof(text), &len)) {
            ring_net_s.text_cb(text, len, false, ring_net_s.user);
        }
    }
    return 0;
}

/* ------------------------------------------------------------- send */

esp_err_t ring_net_send_event(const char *json)
{
    if (ring_net_s.peer == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = ring_net_s.send_stream_id,
        .data = (uint8_t *)json,
        .size = (int)strlen(json),
    };
    int r = esp_peer_send_data(ring_net_s.peer, &frame);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_send_data(stream %u) failed: %d",
                 ring_net_s.send_stream_id, r);
        return ESP_FAIL;
    }
    return ESP_OK;
}
