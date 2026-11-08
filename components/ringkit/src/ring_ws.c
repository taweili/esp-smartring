/*
 * ringkit — WebSocket realtime voice (Qianwen AI platform). See ring_ws.h.
 *
 * Design notes:
 *  - Wi-Fi and SNTP come first: the wss:// TLS handshake validates the server
 *    certificate against the wall clock, so an unset clock fails the handshake.
 *  - The endpoint speaks JSON over the WS text channel. Audio is base64 PCM
 *    in input_audio_buffer.append (16 kHz uplink) and response.audio.delta
 *    (24 kHz downlink). We resample at the boundary so the public API stays
 *    48 kHz / mono, identical to ring_net.
 *  - The esp_websocket_client runs its own task and the WS event callback
 *    executes on that task. So the callback does ONLY fragment copies into a
 *    PSRAM reassembly buffer and pushes completed messages to a worker queue
 *    — no JSON, no base64, no LVGL, no flash. A dedicated ring-ws-worker
 *    task drains the queue and does the heavy work; state changes and
 *    transcripts are marshalled to a ring-ws-event task exactly like ring_net.
 *  - The session protocol is sequential: connect → session.created → send
 *    session.update → session.updated → READY. Turn mode, voice and audio
 *    format are IDLE-only, so the smart_turn_v2 → server_vad fallback must
 *    happen before any audio is sent.
 *
 * This file owns the public API, the module state and the event task. The
 * WebSocket lifecycle/reassembly and playback live in ring_ws_transport.c;
 * the session/turn JSON and the uplink live in ring_ws_protocol.c.
 */
#include "ring_ws_priv.h"

#include "ringkit/ring_wifi.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_ca.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define TAG "ring_ws"

const char *ring_ws_ca_pem(void)
{
    return ring_ca_pem();
}

size_t ring_ws_ca_pem_len(void)
{
    return ring_ca_pem_len();
}

/* ------------------------------------------------------------- state */

ring_ws_ctx_t ring_ws_s;

/* ------------------------------------------------------------- helpers */

const char *ring_ws_state_name(ring_ws_state_t st)
{
    switch (st) {
    case RING_WS_OFF:        return "off";
    case RING_WS_CONNECTING: return "connecting";
    case RING_WS_READY:      return "ready";
    case RING_WS_LISTENING:  return "listening";
    case RING_WS_SPEAKING:   return "speaking";
    case RING_WS_ERROR:      return "error";
    }
    return "?";
}

uint32_t ring_ws_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* State change with event-task notification (same pattern as ring_net). */
void ring_ws_state_set(ring_ws_state_t st)
{
    if (ring_ws_s.state_mutex != NULL) {
        xSemaphoreTake(ring_ws_s.state_mutex, portMAX_DELAY);
    }
    if (ring_ws_s.state == st) {
        if (ring_ws_s.state_mutex != NULL) {
            xSemaphoreGive(ring_ws_s.state_mutex);
        }
        return;
    }
    ring_ws_s.state = st;
    if (ring_ws_s.state_mutex != NULL) {
        xSemaphoreGive(ring_ws_s.state_mutex);
    }
    ESP_LOGI(TAG, "state -> %s", ring_ws_state_name(st));
    if (ring_ws_s.event_queue) {
        xQueueOverwrite(ring_ws_s.event_queue, &st);
        if (ring_ws_s.event_task) {
            xTaskNotifyGive(ring_ws_s.event_task);
        }
    }
}

/* ------------------------------------------------------------- event task */

static void event_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ring_ws_state_t st;
        if (xQueuePeek(ring_ws_s.event_queue, &st, 0) == pdTRUE && ring_ws_s.state_cb) {
            ring_ws_s.state_cb(st, ring_ws_s.user);
        }
    }
}

/* ------------------------------------------------------------- cleanup */

/* Free every resource ring_ws_init() may have allocated. Safe to call on a
 * zero-initialised context and on partial init failure. */
static void ws_release(void)
{
    if (ring_ws_s.msg_queue) { vQueueDelete(ring_ws_s.msg_queue); ring_ws_s.msg_queue = NULL; }
    if (ring_ws_s.event_queue) { vQueueDelete(ring_ws_s.event_queue); ring_ws_s.event_queue = NULL; }
    if (ring_ws_s.state_mutex) { vSemaphoreDelete(ring_ws_s.state_mutex); ring_ws_s.state_mutex = NULL; }
    if (ring_ws_s.reasm_ack) { vSemaphoreDelete(ring_ws_s.reasm_ack); ring_ws_s.reasm_ack = NULL; }
    if (ring_ws_s.worker_task) { vTaskDelete(ring_ws_s.worker_task); ring_ws_s.worker_task = NULL; }
    if (ring_ws_s.event_task) { vTaskDelete(ring_ws_s.event_task); ring_ws_s.event_task = NULL; }
    free(ring_ws_s.reasm); ring_ws_s.reasm = NULL;
    free(ring_ws_s.b64_text); ring_ws_s.b64_text = NULL;
    free(ring_ws_s.pcm24_scratch); ring_ws_s.pcm24_scratch = NULL;
    ring_byte_ring_deinit(&ring_ws_s.dl_ring);
}

/* ------------------------------------------------------------- public API */

esp_err_t ring_ws_init(ring_ws_state_cb_t state_cb,
                       ring_ws_transcript_cb_t text_cb,
                       ring_ws_tool_cb_t tool_cb,
                       void *user)
{
    memset(&ring_ws_s, 0, sizeof(ring_ws_s));
    ring_ws_s.state = RING_WS_OFF;
    ring_ws_s.state_cb = state_cb;
    ring_ws_s.text_cb = text_cb;
    ring_ws_s.tool_cb = tool_cb;
    ring_ws_s.user = user;

    ring_ws_s.msg_queue = xQueueCreate(16, sizeof(ring_ws_msg_t));
    ring_ws_s.event_queue = xQueueCreate(1, sizeof(ring_ws_state_t));
    ring_ws_s.state_mutex = xSemaphoreCreateMutex();
    ring_ws_s.reasm_ack = xSemaphoreCreateBinary();
    if (ring_ws_s.msg_queue == NULL || ring_ws_s.event_queue == NULL ||
        ring_ws_s.state_mutex == NULL || ring_ws_s.reasm_ack == NULL) {
        ws_release();
        return ESP_ERR_NO_MEM;
    }

    if (ring_byte_ring_init(&ring_ws_s.dl_ring, RING_WS_DL_BYTES, MALLOC_CAP_SPIRAM) != ESP_OK) {
        ESP_LOGW(TAG, "downlink ring alloc failed (%d bytes)", RING_WS_DL_BYTES);
        ws_release();
        return ESP_ERR_NO_MEM;
    }
    ring_ws_s.reasm = heap_caps_malloc(RING_WS_REASM_BYTES, MALLOC_CAP_SPIRAM);
    if (ring_ws_s.reasm == NULL) {
        ESP_LOGW(TAG, "reassembly alloc failed (%d bytes)", RING_WS_REASM_BYTES);
        ws_release();
        return ESP_ERR_NO_MEM;
    }
    ring_ws_s.reasm_cap = RING_WS_REASM_BYTES;

    ring_ws_s.b64_cap = RING_WS_B64_MAX;
    ring_ws_s.b64_text = heap_caps_malloc(ring_ws_s.b64_cap, MALLOC_CAP_SPIRAM);
    ring_ws_s.pcm24_cap = RING_WS_PCM24_MAX;
    ring_ws_s.pcm24_scratch = heap_caps_malloc(ring_ws_s.pcm24_cap, MALLOC_CAP_SPIRAM);
    if (ring_ws_s.b64_text == NULL || ring_ws_s.pcm24_scratch == NULL) {
        ESP_LOGW(TAG, "delta scratch alloc failed (%u + %u bytes)",
                 (unsigned)ring_ws_s.b64_cap, (unsigned)ring_ws_s.pcm24_cap);
        ws_release();
        return ESP_ERR_NO_MEM;
    }

    ring_resample_decim_init(&ring_ws_s.decim);
    ring_resample_interp_init(&ring_ws_s.interp);

    xTaskCreate(ring_ws_worker_task, "ring-ws-worker", RING_WS_WORKER_STACK, NULL,
                RING_WS_WORKER_PRIO, &ring_ws_s.worker_task);
    xTaskCreate(event_task, "ring-ws-event", RING_WS_EVENT_STACK, NULL,
                RING_WS_EVENT_PRIO, &ring_ws_s.event_task);

    ESP_LOGI(TAG, "initialised");
    return ESP_OK;
}

esp_err_t ring_ws_start(const char *model, const char *api_key,
                        const ring_ws_session_cfg_t *cfg)
{
    if (model == NULL || api_key == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(ring_ws_s.model, sizeof(ring_ws_s.model), "%s", model);
    snprintf(ring_ws_s.api_key, sizeof(ring_ws_s.api_key), "%s", api_key);
    ring_ws_s.cfg = *cfg;

    /* Negotiated mode starts as the requested mode (or "ptt"). */
    if (cfg->turn_detection == NULL) {
        snprintf(ring_ws_s.turn_mode, sizeof(ring_ws_s.turn_mode), "ptt");
    } else {
        snprintf(ring_ws_s.turn_mode, sizeof(ring_ws_s.turn_mode), "%s", cfg->turn_detection);
    }

    /* Reset stats. */
    ring_ws_s.bytes_up = ring_ws_s.bytes_down = ring_ws_s.turns = 0;
    ring_ws_s.dropped_up = ring_ws_s.dropped_down = 0;
    ring_ws_s.ring_high_samples = 0;
    ring_ws_s.buffered_high_ms = 0;
    ring_ws_s.responding = false;
    ring_ws_s.speaking = false;
    ring_ws_s.last_playback_ms = 0;
    ring_ws_s.cushion_ms = RING_WS_CUSHION_MS;
    /* Reset the resampler history and the downlink ring: stale samples from
     * a previous session would otherwise leak into the first frame/turn. */
    ring_resample_decim_init(&ring_ws_s.decim);
    ring_resample_interp_init(&ring_ws_s.interp);
    ring_byte_ring_reset(&ring_ws_s.dl_ring);

    /* Tear down any previous client first (B-3). */
    if (ring_ws_s.client != NULL) {
        esp_websocket_client_stop(ring_ws_s.client);
        esp_websocket_client_destroy(ring_ws_s.client);
        ring_ws_s.client = NULL;
    }

    ring_ws_state_set(RING_WS_CONNECTING);

    /* 1. Wi-Fi (idempotent, like ring_net). */
    esp_err_t err = ring_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        ring_ws_state_set(RING_WS_ERROR);
        return err;
    }
    err = ring_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP: %s", esp_err_to_name(err));
        ring_ws_state_set(RING_WS_ERROR);
        return err;
    }

    /* 2. Clock (TLS needs it). */
    err = ring_time_sync(RING_WS_SNTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        ring_ws_state_set(RING_WS_ERROR);
        return err;
    }

    /* 3. WebSocket connect. */
    char uri[256];
    snprintf(uri, sizeof(uri), "wss://%s/api-ws/v1/realtime?model=%s",
             RING_WS_HOST, ring_ws_s.model);

    char auth_hdr[192];
    snprintf(auth_hdr, sizeof(auth_hdr),
             "Authorization: Bearer %s\r\n", ring_ws_s.api_key);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .cert_pem = ring_ws_ca_pem(),
        .cert_len = ring_ws_ca_pem_len(),
        .headers = auth_hdr,
        .buffer_size = 4096,
        .disable_auto_reconnect = true,
        .task_stack = 8192,
        .task_prio = 5,
        .ping_interval_sec = 15,
        .network_timeout_ms = 10000,
    };
    ring_ws_s.client = esp_websocket_client_init(&ws_cfg);
    if (ring_ws_s.client == NULL) {
        ESP_LOGE(TAG, "ws init failed");
        ring_ws_state_set(RING_WS_ERROR);
        return ESP_FAIL;
    }
    esp_websocket_register_events(ring_ws_s.client, WEBSOCKET_EVENT_ANY,
                                  ring_ws_event_handler, NULL);
    esp_err_t wserr = esp_websocket_client_start(ring_ws_s.client);
    if (wserr != ESP_OK) {
        ESP_LOGE(TAG, "ws start: %s", esp_err_to_name(wserr));
        esp_websocket_client_destroy(ring_ws_s.client);
        ring_ws_s.client = NULL;
        ring_ws_state_set(RING_WS_ERROR);
        return wserr;
    }

    /* 4. Wait for session.updated. */
    ESP_LOGI(TAG, "waiting for session...");
    uint32_t deadline = ring_ws_now_ms() + RING_WS_SESSION_TIMEOUT_MS;
    for (;;) {
        ring_ws_state_t st = ring_ws_state();
        if (st != RING_WS_CONNECTING && st != RING_WS_OFF) {
            break;
        }
        if (ring_ws_now_ms() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ring_ws_state_t st = ring_ws_state();
    if (st != RING_WS_READY && st != RING_WS_LISTENING &&
        st != RING_WS_SPEAKING) {
        ESP_LOGW(TAG, "session not ready (%s)", ring_ws_state_name(st));
        /* Don't leave a live client behind: the caller retries start(), which
         * would otherwise tear it down only after the retry began. */
        esp_websocket_client_stop(ring_ws_s.client);
        esp_websocket_client_destroy(ring_ws_s.client);
        ring_ws_s.client = NULL;
        ring_ws_state_set(RING_WS_ERROR);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "session ready (model %s, mode %s)", ring_ws_s.model, ring_ws_s.turn_mode);
    return ESP_OK;
}

esp_err_t ring_ws_stop(void)
{
    if (ring_ws_s.client != NULL) {
        esp_websocket_client_stop(ring_ws_s.client);
        esp_websocket_client_destroy(ring_ws_s.client);
        ring_ws_s.client = NULL;
    }
    ring_ws_s.responding = false;
    ring_ws_s.speaking = false;
    ring_byte_ring_reset(&ring_ws_s.dl_ring);
    ring_resample_decim_init(&ring_ws_s.decim);
    ring_resample_interp_init(&ring_ws_s.interp);
    ring_ws_state_set(RING_WS_OFF);
    return ESP_OK;
}

ring_ws_state_t ring_ws_state(void)
{
    if (ring_ws_s.state_mutex != NULL) {
        xSemaphoreTake(ring_ws_s.state_mutex, portMAX_DELAY);
    }
    ring_ws_state_t st = ring_ws_s.state;
    if (ring_ws_s.state_mutex != NULL) {
        xSemaphoreGive(ring_ws_s.state_mutex);
    }
    return st;
}

const char *ring_ws_turn_mode(void)
{
    return ring_ws_s.turn_mode;
}

esp_err_t ring_ws_get_stats(ring_ws_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->bytes_up = ring_ws_s.bytes_up;
    out->bytes_down = ring_ws_s.bytes_down;
    out->turns = ring_ws_s.turns;
    out->dropped_up = ring_ws_s.dropped_up;
    out->dropped_down = ring_ws_s.dropped_down;
    out->ring_high_ms = ring_ws_s.buffered_high_ms;
    out->turn_mode = ring_ws_s.turn_mode;
    return ESP_OK;
}

esp_err_t ring_ws_set_listening(bool on)
{
    if (on) {
        if (ring_ws_state() != RING_WS_READY) {
            return ESP_ERR_INVALID_STATE;
        }
        ring_ws_state_set(RING_WS_LISTENING);
    } else if (ring_ws_state() == RING_WS_LISTENING) {
        ring_ws_state_set(RING_WS_READY);
    }
    return ESP_OK;
}

bool ring_ws_is_speaking(void)
{
    return ring_ws_s.speaking;
}
