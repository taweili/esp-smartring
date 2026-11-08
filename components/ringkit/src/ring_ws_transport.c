/*
 * ringkit — ring_ws transport: WebSocket lifecycle, reassembly, base64, playback.
 *
 * Split out of ring_ws.c. The WS event handler runs on the esp_websocket_client
 * task and does only fragment copies + queue pushes; the worker task does the
 * JSON/base64 heavy lifting (see ring_ws_protocol.c). This file also owns the
 * audio transport in both directions: the 48k→16k uplink (send_audio) and the
 * 24k→48k playback path (read_audio), plus the downlink ring and the
 * half-duplex watchdog. See ring_ws_priv.h for the shared state.
 */
#include "ring_ws_priv.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "ring_ws"

/* ------------------------------------------------------------- base64 send/recv */

/* base64-encode `in` (n bytes) into a NUL-terminated string in `out`.
 * Returns ESP_OK or an mbedtls error. `out` must hold 4/3 * n + 1 bytes. */
esp_err_t ring_ws_b64_encode(const uint8_t *in, size_t n, char *out,
                             size_t out_cap, size_t *out_len)
{
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_cap, &olen, in, n);
    if (ret != 0) {
        return ESP_FAIL;
    }
    out[olen] = '\0';
    if (out_len) {
        *out_len = olen;
    }
    return ESP_OK;
}

/* base64-decode `in` (NUL-terminated) into `out`. Returns bytes written. */
int ring_ws_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    size_t olen = 0;
    int ret = mbedtls_base64_decode(out, out_cap, &olen,
                                    (const unsigned char *)in, in_len);
    if (ret != 0) {
        return -1;
    }
    return (int)olen;
}

/* ------------------------------------------------------------- worker task */

void ring_ws_worker_task(void *arg)
{
    (void)arg;
    ring_ws_msg_t msg;
    for (;;) {
        if (xQueueReceive(ring_ws_s.msg_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.kind == RING_WS_MSG_STOP) {
            break;
        }
        if (msg.kind == RING_WS_MSG_TEXT) {
            /* The data is in ring_ws_s.reasm (shared buffer with WS task). The WS
             * task is blocked in reasm_ack until we give it back. */
            ring_ws_worker_handle_text((const char *)ring_ws_s.reasm, msg.len);
            xSemaphoreGive(ring_ws_s.reasm_ack);
        } else if (msg.kind == RING_WS_MSG_LIFECYCLE) {
            ring_ws_worker_handle_lifecycle(&msg);
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- WS event handler
 *
 * Runs on the esp_websocket_client task. Must stay minimal: copy fragments,
 * push completed messages, post lifecycle events. No JSON, no base64, no LVGL.
 */
void ring_ws_event_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        ring_ws_s.lifecycle_msg.kind = RING_WS_MSG_LIFECYCLE;
        ring_ws_s.lifecycle_msg.err_type = 0;
        xQueueSend(ring_ws_s.msg_queue, &ring_ws_s.lifecycle_msg, portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        ring_ws_s.lifecycle_msg.kind = RING_WS_MSG_LIFECYCLE;
        ring_ws_s.lifecycle_msg.err_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
        xQueueSend(ring_ws_s.msg_queue, &ring_ws_s.lifecycle_msg, portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_ERROR type=%d",
                 data ? data->error_handle.error_type : -1);
        ring_ws_s.lifecycle_msg.kind = RING_WS_MSG_LIFECYCLE;
        ring_ws_s.lifecycle_msg.err_type = data ? data->error_handle.error_type
                                       : WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
        xQueueSend(ring_ws_s.msg_queue, &ring_ws_s.lifecycle_msg, portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CLOSED");
        ring_ws_s.lifecycle_msg.kind = RING_WS_MSG_LIFECYCLE;
        ring_ws_s.lifecycle_msg.err_type = WEBSOCKET_ERROR_TYPE_SERVER_CLOSE;
        xQueueSend(ring_ws_s.msg_queue, &ring_ws_s.lifecycle_msg, portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (data == NULL || data->data_len <= 0) {
            break;
        }
        /* Only reassemble text frames (op_code 1). Pong/close are op 0xA/0x8. */
        if (data->op_code != 1) {
            break;
        }
        /* Accumulate into the reassembly buffer at the current offset. */
        int offset = data->payload_offset;
        if (offset + data->data_len > ring_ws_s.reasm_cap) {
            ESP_LOGW(TAG, "fragment too big: %d+%d > %d",
                     offset, data->data_len, ring_ws_s.reasm_cap);
            break;
        }
        memcpy(ring_ws_s.reasm + offset, data->data_ptr, data->data_len);
        if (data->fin && (offset + data->data_len) >= data->payload_len) {
            int total = offset + data->data_len;
            if (total >= ring_ws_s.reasm_cap) {
                ESP_LOGW(TAG, "message too big (%d >= %d) - dropped",
                         total, ring_ws_s.reasm_cap);
                break;
            }
            ring_ws_s.reasm[total] = '\0';
            ring_ws_msg_t m = { .kind = RING_WS_MSG_TEXT, .len = total };
            xQueueSend(ring_ws_s.msg_queue, &m, portMAX_DELAY);
            /* Hand ownership of ring_ws_s.reasm to the worker until it has parsed
             * the message: the next fragment lands at offset 0 and would
             * otherwise overwrite the JSON mid-parse. */
            if (xSemaphoreTake(ring_ws_s.reasm_ack, pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGW(TAG, "worker did not release the reassembly buffer");
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------- downlink ring (24 kHz) */

void ring_ws_dl_push(const uint8_t *pcm, size_t len)
{
    size_t dropped = 0;
    ring_byte_ring_push(&ring_ws_s.dl_ring, pcm, len, &dropped);
    ring_ws_s.dropped_down += (uint32_t)dropped;

    size_t cur = ring_byte_ring_count(&ring_ws_s.dl_ring) / 2;   /* samples (24 kHz) */
    if (cur > ring_ws_s.ring_high_samples) {
        ring_ws_s.ring_high_samples = (uint32_t)cur;
    }
}

size_t ring_ws_dl_pop(uint8_t *pcm, size_t len)
{
    return ring_byte_ring_pop(&ring_ws_s.dl_ring, pcm, len);
}

size_t ring_ws_dl_count_bytes(void)
{
    return ring_byte_ring_count(&ring_ws_s.dl_ring);
}

uint32_t ring_ws_buffered_ms(void)
{
    /* 24 kHz mono, 2 bytes/sample -> ms = bytes / (2 * 24) */
    return (uint32_t)(ring_ws_dl_count_bytes() / (2 * (RING_WS_OUT_RATE / 1000)));
}

bool ring_ws_playback_pending(void)
{
    if (ring_ws_is_speaking()) {
        return true;
    }
    return ring_ws_buffered_ms() > 0;
}

/* A lost response.done would keep the half-duplex gate closed forever. */
void ring_ws_response_watchdog(void)
{
    if (ring_ws_s.responding &&
        (ring_ws_now_ms() - ring_ws_s.response_start_ms) > RING_WS_RESPONSE_WATCHDOG_MS) {
        ESP_LOGW(TAG, "no response.done after %u ms - cancelling",
                 (unsigned)(ring_ws_now_ms() - ring_ws_s.response_start_ms));
        ring_ws_s.responding = false;
        ring_ws_s.speaking = false;
        if (ring_ws_state() == RING_WS_SPEAKING) {
            ring_ws_state_set(RING_WS_READY);
        }
    }
}

/* ------------------------------------------------------------- uplink */

esp_err_t ring_ws_send_audio(const int16_t *pcm48k, size_t samples)
{
    if (ring_ws_s.client == NULL || pcm48k == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    ring_ws_response_watchdog();
    if (ring_ws_state() != RING_WS_LISTENING && ring_ws_state() != RING_WS_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Half-duplex gate: drop the uplink while the model is replying or
     * recent playback is still in the echo tail. */
    uint32_t now = ring_ws_now_ms();
    if (ring_ws_s.responding || ring_ws_buffered_ms() > 0 ||
        (ring_ws_s.last_playback_ms && (now - ring_ws_s.last_playback_ms) < RING_WS_ECHO_TAIL_MS)) {
        ring_ws_s.dropped_up++;
        return ESP_ERR_INVALID_STATE;
    }

    /* Resample 48k -> 16k. We only process full 20 ms blocks here. */
    if (samples != RING_WS_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    /* int16 -> float */
    float in_f[RING_WS_FRAME_SAMPLES];
    for (size_t i = 0; i < samples; i++) {
        in_f[i] = (float)pcm48k[i];
    }

    int n16 = ring_resample_decim_process(&ring_ws_s.decim, in_f, (int)samples, ring_ws_s.decim_out);
    if (n16 <= 0 || n16 > RING_WS_UP16_SAMPLES) {
        return ESP_FAIL;
    }

    /* float (-32768..32767) -> int16. The filter output is a float; sending
     * the raw floats (as this code first did) ships IEEE-754 bit patterns as
     * "PCM" and the model only ever hears noise. */
    for (int i = 0; i < n16; i++) {
        float v = ring_ws_s.decim_out[i];
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        ring_ws_s.up16[i] = (int16_t)v;
    }

    /* base64-encode the 16-bit PCM (no heap churn per 20 ms frame). */
    size_t pcm_bytes = (size_t)n16 * 2;
    size_t b64_len = 0;
    if (ring_ws_b64_encode((const uint8_t *)ring_ws_s.up16, pcm_bytes, ring_ws_s.up_b64,
                           sizeof(ring_ws_s.up_b64), &b64_len) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Wrap in JSON. */
    int n = snprintf(ring_ws_s.up_json, sizeof(ring_ws_s.up_json),
                     "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                     ring_ws_s.up_b64);
    if (n <= 0 || (size_t)n >= sizeof(ring_ws_s.up_json)) {
        return ESP_FAIL;
    }

    int w = esp_websocket_client_send_text(ring_ws_s.client, ring_ws_s.up_json, n,
                                           pdMS_TO_TICKS(RING_WS_SEND_TIMEOUT_MS));
    if (w < 0) {
        ring_ws_s.dropped_up++;
        return ESP_FAIL;
    }
    ring_ws_s.bytes_up += (uint32_t)w;
    return ESP_OK;
}

/* ------------------------------------------------------------- playback */

esp_err_t ring_ws_read_audio(int16_t *pcm48k, size_t samples,
                             size_t *out, uint32_t timeout_ms)
{
    if (pcm48k == NULL || samples == 0 ||
        samples > RING_WS_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out) {
        *out = 0;
    }
    ring_ws_response_watchdog();

    /* Jitter cushion: wait for RING_WS_CUSHION_MS of downlink only when
     * playback is *starting* (ring empty). response.audio.delta arrives in
     * bursts, so re-waiting for the cushion before every 20 ms pop would put
     * every frame behind a fresh refill and stutter. */
    if (ring_ws_dl_count_bytes() == 0) {
        uint32_t deadline = ring_ws_now_ms() + timeout_ms;
        while (ring_ws_buffered_ms() < ring_ws_s.cushion_ms && ring_ws_now_ms() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (ring_ws_dl_count_bytes() == 0) {
            return ESP_ERR_TIMEOUT;
        }
    }

    /* Pop samples at 24 kHz, resample to 48 kHz.
     * We need `samples` 48k samples = `samples/2` 24k samples. */
    size_t want24 = samples / 2;
    if (want24 == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pcm24[(want24) * 2];
    size_t got = ring_ws_dl_pop(pcm24, want24 * 2) / 2;   /* samples */
    if (got == 0) {
        return ESP_ERR_TIMEOUT;
    }

    float in_f[got];
    for (size_t i = 0; i < got; i++) {
        in_f[i] = (float)((int16_t)(pcm24[2 * i] | (pcm24[2 * i + 1] << 8)));
    }
    int n48 = ring_resample_interp_process(&ring_ws_s.interp, in_f, (int)got, ring_ws_s.interp_out);

    size_t copy = (size_t)n48 < samples ? (size_t)n48 : samples;
    for (size_t i = 0; i < copy; i++) {
        float v = ring_ws_s.interp_out[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm48k[i] = (int16_t)v;
    }

    if (out) {
        *out = copy;
    }
    ring_ws_s.last_playback_ms = ring_ws_now_ms();
    uint32_t bms = ring_ws_buffered_ms();
    if (bms > ring_ws_s.buffered_high_ms) {
        ring_ws_s.buffered_high_ms = bms;
    }
    return ESP_OK;
}
