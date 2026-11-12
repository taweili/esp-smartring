/*
 * ringkit — GEODIS Voice Agent realtime voice. See ring_geodis.h.
 *
 * Design notes:
 *  - Wi-Fi and SNTP come first: the HTTPS token/SDP POSTs and the DTLS
 *    handshake both validate the server certificate against the wall clock.
 *  - Signalling is the GEODIS ephemeral-token flow: POST /token -> ek_ token,
 *    then POST the Azure OpenAI Realtime SDP exchange with Bearer auth.
 *  - The peer main loop and the SDP exchange run in separate tasks so a slow
 *    HTTPS round trip never stalls RTP.
 *  - Audio is Opus at 48 kHz stereo inside RTP. The SDP negotiates
 *    opus/48000/2 (verified by scripts/test_geodis_api.py).
 *
 * This file owns the public API, the module state and the event task. The
 * token + SDP exchange and peer bring-up live in ring_geodis_signaling.c, the
 * Opus codec in ring_geodis_codec.c and the DataChannel JSON protocol in
 * ring_geodis_protocol.c.
 */
#include "ring_geodis_priv.h"

#include "ringkit/ring_wifi.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_rand.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define TAG "ring_geodis"

ring_geodis_ctx_t ring_geodis_s;

/* ------------------------------------------------------------- helpers */

const char *ring_geodis_state_name(ring_geodis_state_t state)
{
    switch (state) {
    case RING_GEODIS_OFF:        return "off";
    case RING_GEODIS_CONNECTING: return "connecting";
    case RING_GEODIS_READY:      return "ready";
    case RING_GEODIS_LISTENING:  return "listening";
    case RING_GEODIS_SPEAKING:   return "speaking";
    case RING_GEODIS_ERROR:      return "error";
    }
    return "?";
}

/* Updates the state and notifies the event task, which runs the app callback.
 * Safe to call from any task, including the peer thread. */
void ring_geodis_state_set(ring_geodis_state_t st)
{
    if (ring_geodis_s.state_mutex != NULL) {
        xSemaphoreTake(ring_geodis_s.state_mutex, portMAX_DELAY);
    }
    if (ring_geodis_s.state == st) {
        if (ring_geodis_s.state_mutex != NULL) {
            xSemaphoreGive(ring_geodis_s.state_mutex);
        }
        return;
    }
    ring_geodis_s.state = st;
    if (ring_geodis_s.state_mutex != NULL) {
        xSemaphoreGive(ring_geodis_s.state_mutex);
    }
    ESP_LOGI(TAG, "state -> %s", ring_geodis_state_name(st));
    if (ring_geodis_s.event_queue) {
        xQueueOverwrite(ring_geodis_s.event_queue, &st);
        if (ring_geodis_s.event_task) {
            xTaskNotifyGive(ring_geodis_s.event_task);
        }
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------- downlink ring */

static size_t dl_pop(uint8_t *pcm, size_t len)
{
    return ring_byte_ring_pop(&ring_geodis_s.dl_ring, pcm, len);
}

/* ------------------------------------------------------------- tasks */

static void event_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ring_geodis_state_t st;
        if (xQueuePeek(ring_geodis_s.event_queue, &st, 0) == pdTRUE && ring_geodis_s.state_cb) {
            ring_geodis_s.state_cb(st, ring_geodis_s.user);
        }
    }
}

/* ------------------------------------------------------------- cleanup */

/* Free every resource ring_geodis_init() may have allocated. Safe to call on a
 * zero-initialised context and on partial init failure. */
static void geodis_release(void)
{
    if (ring_geodis_s.event_queue) { vQueueDelete(ring_geodis_s.event_queue); ring_geodis_s.event_queue = NULL; }
    if (ring_geodis_s.state_mutex) { vSemaphoreDelete(ring_geodis_s.state_mutex); ring_geodis_s.state_mutex = NULL; }
    if (ring_geodis_s.sdp_mutex) { vSemaphoreDelete(ring_geodis_s.sdp_mutex); ring_geodis_s.sdp_mutex = NULL; }
    if (ring_geodis_s.dec_mutex) { vSemaphoreDelete(ring_geodis_s.dec_mutex); ring_geodis_s.dec_mutex = NULL; }
    if (ring_geodis_s.tool_queue) { vQueueDelete(ring_geodis_s.tool_queue); ring_geodis_s.tool_queue = NULL; }
    if (ring_geodis_s.enc) { esp_audio_enc_close(ring_geodis_s.enc); ring_geodis_s.enc = NULL; }
    if (ring_geodis_s.dec) { esp_audio_dec_close(ring_geodis_s.dec); ring_geodis_s.dec = NULL; }
    free(ring_geodis_s.enc_out); ring_geodis_s.enc_out = NULL;
    free(ring_geodis_s.enc_in); ring_geodis_s.enc_in = NULL;
    free(ring_geodis_s.dec_out); ring_geodis_s.dec_out = NULL;
    free(ring_geodis_s.dec_mono); ring_geodis_s.dec_mono = NULL;
    ring_byte_ring_deinit(&ring_geodis_s.dl_ring);
}

/* ------------------------------------------------------------- public API */

esp_err_t ring_geodis_init(ring_geodis_state_cb_t state_cb,
                           ring_geodis_transcript_cb_t text_cb,
                           ring_geodis_tool_cb_t tool_cb, void *user)
{
    memset(&ring_geodis_s, 0, sizeof(ring_geodis_s));
    ring_geodis_s.state = RING_GEODIS_OFF;
    ring_geodis_s.state_cb = state_cb;
    ring_geodis_s.text_cb = text_cb;
    ring_geodis_s.tool_cb = tool_cb;
    ring_geodis_s.user = user;

    ring_geodis_s.sdp_mutex = xSemaphoreCreateMutex();
    ring_geodis_s.state_mutex = xSemaphoreCreateMutex();
    ring_geodis_s.event_queue = xQueueCreate(1, sizeof(ring_geodis_state_t));
    ring_geodis_s.tool_queue = xQueueCreate(2, sizeof(ring_geodis_tool_job_t));
    if (ring_geodis_s.sdp_mutex == NULL || ring_geodis_s.state_mutex == NULL ||
        ring_geodis_s.event_queue == NULL || ring_geodis_s.tool_queue == NULL) {
        geodis_release();
        return ESP_ERR_NO_MEM;
    }

    /* The downlink buffer is 384 KB: it must live in PSRAM, not internal RAM. */
    if (ring_byte_ring_init(&ring_geodis_s.dl_ring, RING_GEODIS_DL_BYTES, MALLOC_CAP_SPIRAM) != ESP_OK) {
        ESP_LOGW(TAG, "downlink ring allocation failed (%d bytes)",
                 RING_GEODIS_DL_BYTES);
    }

    esp_err_t err = ring_geodis_opus_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "opus init failed: %s", esp_err_to_name(err));
        geodis_release();
        return err;
    }

    xTaskCreate(ring_geodis_signal_task, "ring-geodis-signal",
                RING_GEODIS_SIGNAL_TASK_STACK, NULL,
                RING_GEODIS_SIGNAL_TASK_PRIO, &ring_geodis_s.signal_task);
    xTaskCreate(event_task, "ring-geodis-event", RING_GEODIS_EVENT_TASK_STACK,
                NULL, RING_GEODIS_EVENT_TASK_PRIO, &ring_geodis_s.event_task);
    xTaskCreate(ring_geodis_tool_task, "ring-geodis-tool",
                RING_GEODIS_TOOL_TASK_STACK, NULL,
                RING_GEODIS_TOOL_TASK_PRIO, &ring_geodis_s.tool_task);

    ESP_LOGI(TAG, "initialised");
    return ESP_OK;
}

esp_err_t ring_geodis_start(const ring_geodis_cfg_t *cfg)
{
    if (cfg == NULL || cfg->base_url == NULL || cfg->sdp_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(ring_geodis_s.base_url, sizeof(ring_geodis_s.base_url), "%s", cfg->base_url);
    snprintf(ring_geodis_s.sdp_url, sizeof(ring_geodis_s.sdp_url), "%s", cfg->sdp_url);
    snprintf(ring_geodis_s.voice, sizeof(ring_geodis_s.voice), "%s",
             (cfg->voice != NULL) ? cfg->voice : "coral");
    snprintf(ring_geodis_s.system_prompt, sizeof(ring_geodis_s.system_prompt), "%s",
             (cfg->system_prompt != NULL) ? cfg->system_prompt : "");
    ring_geodis_s.enable_stt = cfg->enable_stt;
    ring_geodis_s.enable_ptt = cfg->enable_ptt;

    /* A fresh chat_session_id per session (transcript persistence key). */
    ring_rand_seed((uint32_t)esp_timer_get_time());
    snprintf(ring_geodis_s.chat_session_id, sizeof(ring_geodis_s.chat_session_id),
             "ring-%08x", (unsigned)ring_rand_u32());

    /* A retry must start from a clean peer: reusing a half-open connection
     * would send a second offer over an existing DTLS session. */
    if (ring_geodis_s.peer) {
        esp_peer_disconnect(ring_geodis_s.peer);
        esp_peer_close(ring_geodis_s.peer);
        ring_geodis_s.peer = NULL;
    }
    ring_geodis_s.speaking = false;
    ring_geodis_s.pts = 0;
    /* A retry must not replay stale downlink audio or reuse a stale SCTP
     * stream id from the previous session. */
    ring_byte_ring_reset(&ring_geodis_s.dl_ring);
    ring_geodis_s.send_stream_id = 0;
    ring_geodis_state_set(RING_GEODIS_CONNECTING);

    /* 1. Wi-Fi */
    esp_err_t err = ring_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return err;
    }
    err = ring_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP: %s", esp_err_to_name(err));
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return err;
    }

    /* 2. Clock — required for TLS/DTLS certificate validation. */
    err = ring_time_sync(RING_GEODIS_SNTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return err;
    }

    /* 3. Ephemeral token. The gateway resets ~1 in 12 TLS connections, so the
     * POST retries internally. */
    err = ring_geodis_fetch_token();
    if (err != ESP_OK) {
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return err;
    }

    /* 4. PeerConnection + signaling */
    err = ring_geodis_peer_open();
    if (err != ESP_OK) {
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return err;
    }
    int r = esp_peer_new_connection(ring_geodis_s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "new_connection failed: %d", r);
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return ESP_FAIL;
    }

    /* 5. Wait for the DataChannel + session.updated. */
    ESP_LOGI(TAG, "waiting for session...");
    uint32_t deadline = now_ms() + RING_GEODIS_SESSION_TIMEOUT_MS;
    for (;;) {
        ring_geodis_state_t st = ring_geodis_state();
        if (st != RING_GEODIS_CONNECTING && st != RING_GEODIS_OFF) {
            break;
        }
        if (now_ms() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ring_geodis_state_t st = ring_geodis_state();
    if (st != RING_GEODIS_READY && st != RING_GEODIS_LISTENING &&
        st != RING_GEODIS_SPEAKING) {
        ESP_LOGW(TAG, "session not ready (%s)", ring_geodis_state_name(st));
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "session ready");
    return ESP_OK;
}

esp_err_t ring_geodis_stop(void)
{
    if (ring_geodis_s.peer) {
        esp_peer_disconnect(ring_geodis_s.peer);
        esp_peer_close(ring_geodis_s.peer);
        ring_geodis_s.peer = NULL;
    }
    ring_geodis_state_set(RING_GEODIS_OFF);
    return ESP_OK;
}

ring_geodis_state_t ring_geodis_state(void)
{
    if (ring_geodis_s.state_mutex != NULL) {
        xSemaphoreTake(ring_geodis_s.state_mutex, portMAX_DELAY);
    }
    ring_geodis_state_t st = ring_geodis_s.state;
    if (ring_geodis_s.state_mutex != NULL) {
        xSemaphoreGive(ring_geodis_s.state_mutex);
    }
    return st;
}

esp_err_t ring_geodis_set_listening(bool on)
{
    if (on) {
        if (ring_geodis_state() != RING_GEODIS_READY) {
            return ESP_ERR_INVALID_STATE;
        }
        ring_geodis_s.speaking = false;
        ring_geodis_state_set(RING_GEODIS_LISTENING);
    } else if (ring_geodis_state() == RING_GEODIS_LISTENING) {
        /* Releasing while the model is already speaking must not cut the
         * reply short; the reply finishes and response.done returns us to
         * READY. */
        ring_geodis_state_set(RING_GEODIS_READY);
    }
    return ESP_OK;
}

esp_err_t ring_geodis_send_audio(const int16_t *pcm, size_t samples)
{
    if (ring_geodis_s.peer == NULL || pcm == NULL || samples == 0 || ring_geodis_s.enc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples != RING_GEODIS_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ring_geodis_state() != RING_GEODIS_LISTENING) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Encode exactly one Opus frame per call (the caller feeds
     * RING_GEODIS_FRAME_SAMPLES at a time). The public API is mono while the
     * encoder is stereo, so duplicate the samples across L/R. */
    int16_t *stereo = (int16_t *)ring_geodis_s.enc_in;
    for (size_t i = 0; i < samples; i++) {
        stereo[2 * i] = pcm[i];
        stereo[2 * i + 1] = pcm[i];
    }
    esp_audio_enc_in_frame_t in = {
        .buffer = ring_geodis_s.enc_in,
        .len = (uint32_t)(samples * RING_GEODIS_OPUS_CHANNELS * 2),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = ring_geodis_s.enc_out,
        .len = (uint32_t)ring_geodis_s.enc_out_size,
    };
    esp_audio_err_t e = esp_audio_enc_process(ring_geodis_s.enc, &in, &out);
    if (e != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        return ESP_FAIL;
    }

    /* RTP presentation timestamp: the sample count, not the wall clock. */
    esp_peer_audio_frame_t frame = {
        .data = ring_geodis_s.enc_out,
        .size = (int)out.encoded_bytes,
        .pts = ring_geodis_s.pts,
    };
    ring_geodis_s.pts += samples;
    int r = esp_peer_send_audio(ring_geodis_s.peer, &frame);
    if (r != ESP_PEER_ERR_NONE && r != ESP_PEER_ERR_WOULD_BLOCK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ring_geodis_read_audio(int16_t *pcm, size_t samples, size_t *out_samples,
                                 uint32_t timeout_ms)
{
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t want = samples * 2;
    uint32_t deadline = now_ms() + timeout_ms;
    size_t got = 0;
    while (got < want) {
        got += dl_pop((uint8_t *)pcm + got, want - got);
        if (got >= want || now_ms() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (out_samples) {
        *out_samples = got / 2;
    }
    return got > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool ring_geodis_is_speaking(void)
{
    return ring_geodis_s.speaking;
}
