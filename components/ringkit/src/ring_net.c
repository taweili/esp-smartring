/*
 * ringkit — WebRTC realtime voice (Qianwen AI platform). See ring_net.h.
 *
 * Design notes:
 *  - Wi-Fi and SNTP come first: the HTTPS SDP POST and the DTLS handshake both
 *    validate the server certificate against the wall clock, so an unset clock
 *    fails every connection with a certificate error.
 *  - Signaling is the Qianwen SDP exchange over HTTPS; esp_peer produces the
 *    Offer (with gathered candidates) and consumes the Answer.
 *  - The peer main loop and the SDP exchange run in separate tasks so a slow
 *    HTTPS round trip never stalls RTP.
 *  - Audio is Opus at 48 kHz mono inside RTP. The endpoint accepts only Opus
 *    on m=audio (PCMA/PCMU offers are rejected with "SDP format and content
 *    error"), and esp_peer carries encoded frames — it has no codec of its own,
 *    so esp_audio_codec does the Opus work here.
 *
 * This file owns the public API, the module state and the event task. The SDP
 * exchange and peer bring-up live in ring_net_signaling.c, the Opus codec in
 * ring_net_codec.c and the DataChannel JSON protocol in ring_net_protocol.c.
 *
 * Threading: the peer task owns the esp_peer handle; everybody else uses it
 * under peer_lock and asks for teardown via peer_close_req (see
 * ring_net_signaling.c). Application callbacks run on `ring-net-event`.
 */
#include "ring_net_priv.h"

#include "ringkit/ring_wifi.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_ca.h"

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

#define TAG "ring_net"

ring_net_ctx_t ring_net_s;

const char *ring_net_ca_pem(void)
{
    return ring_ca_pem();
}

size_t ring_net_ca_pem_len(void)
{
    return ring_ca_pem_len();
}

/* ------------------------------------------------------------- helpers */

const char *ring_net_state_name(ring_net_state_t state)
{
    switch (state) {
    case RING_NET_OFF:        return "off";
    case RING_NET_CONNECTING: return "connecting";
    case RING_NET_READY:      return "ready";
    case RING_NET_LISTENING:  return "listening";
    case RING_NET_SPEAKING:   return "speaking";
    case RING_NET_ERROR:      return "error";
    }
    return "?";
}

/* Updates the state and queues a transition for the event task. Safe to call
 * from any task, including the peer thread. */
void ring_net_state_set(ring_net_state_t st)
{
    if (ring_net_s.state_mutex != NULL) {
        xSemaphoreTake(ring_net_s.state_mutex, portMAX_DELAY);
    }
    if (ring_net_s.state == st) {
        if (ring_net_s.state_mutex != NULL) {
            xSemaphoreGive(ring_net_s.state_mutex);
        }
        return;
    }
    ring_net_s.state = st;
    if (ring_net_s.state_mutex != NULL) {
        xSemaphoreGive(ring_net_s.state_mutex);
    }
    ESP_LOGI(TAG, "state -> %s", ring_net_state_name(st));

    if (ring_net_s.event_queue == NULL) {
        return;
    }
    ring_net_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = RING_NET_EV_STATE;
    ev.state = st;
    if (xQueueSend(ring_net_s.event_queue, &ev, 0) != pdTRUE) {
        /* Only reachable if the app's callback is slower than a state storm;
         * the polled ring_net_state() is still correct. */
        ESP_LOGW(TAG, "event queue full, state %s not delivered",
                 ring_net_state_name(st));
    }
}

/* Queue a transcript fragment for the event task. Called on the peer task. */
void ring_net_emit_text(const char *text, size_t len, bool is_user, bool final,
                        bool clipped)
{
    if (ring_net_s.event_queue == NULL || text == NULL || len == 0) {
        return;
    }
    ring_net_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = RING_NET_EV_TEXT;
    ev.is_user = is_user ? 1 : 0;
    ev.final = final ? 1 : 0;
    ev.clipped = clipped ? 1 : 0;
    if (len > sizeof(ev.text) - 1) {
        len = sizeof(ev.text) - 1;
        ev.clipped = 1;
    }
    memcpy(ev.text, text, len);
    ev.text[len] = '\0';
    ev.len = (uint16_t)len;

    if (xQueueSend(ring_net_s.event_queue, &ev, 0) != pdTRUE) {
        /* Dropping a fragment costs a word on screen, never correctness: the
         * `…done` event that follows carries the whole line. */
        ESP_LOGW(TAG, "event queue full, transcript fragment dropped (%u B)",
                 (unsigned)len);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------- downlink ring */

static size_t dl_pop(uint8_t *pcm, size_t len)
{
    return ring_byte_ring_pop(&ring_net_s.dl_ring, pcm, len);
}

/* ------------------------------------------------------------- tasks */

static void event_task(void *arg)
{
    (void)arg;
    ring_net_event_t ev;
    for (;;) {
        if (xQueueReceive(ring_net_s.event_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ev.kind == RING_NET_EV_STATE) {
            if (ring_net_s.state_cb != NULL) {
                ring_net_s.state_cb(ev.state, ring_net_s.user);
            }
        } else if (ev.kind == RING_NET_EV_TEXT) {
            if (ring_net_s.text_cb != NULL) {
                ring_net_s.text_cb(ev.text, ev.len, ev.is_user != 0,
                                   ev.final != 0, ev.clipped != 0,
                                   ring_net_s.user);
            }
        }
    }
}

/* ------------------------------------------------------------- cleanup */

/* Free every resource ring_net_init() may have allocated. Safe to call on a
 * zero-initialised context and on partial init failure. */
static void net_release(void)
{
    if (ring_net_s.event_queue) { vQueueDeleteWithCaps(ring_net_s.event_queue); ring_net_s.event_queue = NULL; }
    if (ring_net_s.state_mutex) { vSemaphoreDelete(ring_net_s.state_mutex); ring_net_s.state_mutex = NULL; }
    if (ring_net_s.sdp_mutex) { vSemaphoreDelete(ring_net_s.sdp_mutex); ring_net_s.sdp_mutex = NULL; }
    if (ring_net_s.dec_mutex) { vSemaphoreDelete(ring_net_s.dec_mutex); ring_net_s.dec_mutex = NULL; }
    if (ring_net_s.peer_lock) { vSemaphoreDelete(ring_net_s.peer_lock); ring_net_s.peer_lock = NULL; }
    if (ring_net_s.peer_gone) { vSemaphoreDelete(ring_net_s.peer_gone); ring_net_s.peer_gone = NULL; }
    if (ring_net_s.enc) { esp_audio_enc_close(ring_net_s.enc); ring_net_s.enc = NULL; }
    if (ring_net_s.dec) { esp_audio_dec_close(ring_net_s.dec); ring_net_s.dec = NULL; }
    free(ring_net_s.enc_out); ring_net_s.enc_out = NULL;
    free(ring_net_s.enc_in); ring_net_s.enc_in = NULL;
    free(ring_net_s.dec_out); ring_net_s.dec_out = NULL;
    free(ring_net_s.dec_mono); ring_net_s.dec_mono = NULL;
    ring_byte_ring_deinit(&ring_net_s.dl_ring);
}

/* ------------------------------------------------------------- public API */

esp_err_t ring_net_init(ring_net_state_cb_t state_cb,
                        ring_net_transcript_cb_t text_cb,
                        void *user)
{
    memset(&ring_net_s, 0, sizeof(ring_net_s));
    ring_net_s.state = RING_NET_OFF;
    ring_net_s.state_cb = state_cb;
    ring_net_s.text_cb = text_cb;
    ring_net_s.user = user;

    ring_net_s.sdp_mutex = xSemaphoreCreateMutex();
    ring_net_s.state_mutex = xSemaphoreCreateMutex();
    ring_net_s.peer_lock = xSemaphoreCreateMutex();
    ring_net_s.peer_gone = xSemaphoreCreateBinary();
    /* ~6 KB of events: PSRAM, because internal RAM is where the panel buffers
     * and the IMU task's stack live. */
    ring_net_s.event_queue = xQueueCreateWithCaps(RING_NET_EVENT_DEPTH,
                                                  sizeof(ring_net_event_t),
                                                  MALLOC_CAP_SPIRAM);
    if (ring_net_s.sdp_mutex == NULL || ring_net_s.state_mutex == NULL ||
        ring_net_s.peer_lock == NULL || ring_net_s.peer_gone == NULL ||
        ring_net_s.event_queue == NULL) {
        net_release();
        return ESP_ERR_NO_MEM;
    }

    /* The downlink buffer is 384 KB: it must live in PSRAM, not internal RAM. */
    if (ring_byte_ring_init(&ring_net_s.dl_ring, RING_NET_DL_BYTES, MALLOC_CAP_SPIRAM) != ESP_OK) {
        ESP_LOGE(TAG, "downlink ring allocation failed (%d bytes)",
                 RING_NET_DL_BYTES);
        net_release();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ring_net_opus_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "opus init failed: %s", esp_err_to_name(err));
        net_release();
        return err;
    }

    BaseType_t ok = xTaskCreate(ring_net_signal_task, "ring-net-signal",
                                RING_NET_SIGNAL_TASK_STACK, NULL,
                                RING_NET_SIGNAL_TASK_PRIO,
                                &ring_net_s.signal_task);
    if (ok == pdPASS) {
        ok = xTaskCreate(ring_net_peer_task, "ring-net-peer",
                         RING_NET_PEER_TASK_STACK, NULL,
                         RING_NET_PEER_TASK_PRIO, &ring_net_s.peer_task);
    }
    if (ok == pdPASS) {
        ok = xTaskCreate(event_task, "ring-net-event", RING_NET_EVENT_TASK_STACK,
                         NULL, RING_NET_EVENT_TASK_PRIO, &ring_net_s.event_task);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        if (ring_net_s.signal_task) { vTaskDelete(ring_net_s.signal_task); ring_net_s.signal_task = NULL; }
        if (ring_net_s.peer_task) { vTaskDelete(ring_net_s.peer_task); ring_net_s.peer_task = NULL; }
        if (ring_net_s.event_task) { vTaskDelete(ring_net_s.event_task); ring_net_s.event_task = NULL; }
        net_release();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialised");
    return ESP_OK;
}

esp_err_t ring_net_start(const char *model, const char *api_key)
{
    if (model == NULL || api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(ring_net_s.model, sizeof(ring_net_s.model), "%s", model);
    snprintf(ring_net_s.api_key, sizeof(ring_net_s.api_key), "%s", api_key);

    /* A retry must start from a clean peer: reusing a half-open connection
     * would send a second offer over an existing DTLS session. The peer task
     * owns the handle, so teardown goes through it. */
    esp_err_t err = ring_net_peer_close();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "previous peer would not close: %s", esp_err_to_name(err));
        ring_net_s.last_err = err;
        ring_net_state_set(RING_NET_ERROR);
        return err;
    }
    /* Whatever the signal task is still working on is now stale. */
    ring_net_s.session_gen++;
    ring_net_s.last_err = ESP_OK;

    ring_net_s.speaking = false;
    ring_net_s.pts = 0;
    /* A retry must not replay stale downlink audio or reuse a stale SCTP
     * stream id from the previous session. */
    ring_byte_ring_reset(&ring_net_s.dl_ring);
    ring_net_s.send_stream_id = 0;
    ring_net_state_set(RING_NET_CONNECTING);

    /* 1. Wi-Fi */
    err = ring_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        goto fail;
    }
    err = ring_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 2. Clock — required for TLS/DTLS certificate validation. */
    err = ring_time_sync(RING_NET_SNTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto fail;
    }

    /* 3. PeerConnection + signaling */
    err = ring_net_peer_open();
    if (err != ESP_OK) {
        goto fail;
    }
    err = ring_net_peer_connect();
    if (err != ESP_OK) {
        goto fail;
    }
    ring_net_s.peer_ready = true;

    /* 4. Wait for the DataChannel + session.updated. */
    ESP_LOGI(TAG, "waiting for session...");
    uint32_t deadline = now_ms() + RING_NET_SESSION_TIMEOUT_MS;
    for (;;) {
        ring_net_state_t st = ring_net_state();
        if (st != RING_NET_CONNECTING && st != RING_NET_OFF) {
            break;
        }
        if (now_ms() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* READY is the state the wait loop looks for, but the app may have
     * already opened the uplink (LISTENING) or the reply may have started
     * (SPEAKING) by the time we re-check. Both imply the session is up. */
    ring_net_state_t st = ring_net_state();
    if (st != RING_NET_READY && st != RING_NET_LISTENING &&
        st != RING_NET_SPEAKING) {
        ESP_LOGW(TAG, "session not ready (%s)", ring_net_state_name(st));
        err = (ring_net_s.last_err != ESP_OK) ? ring_net_s.last_err
                                              : ESP_ERR_TIMEOUT;
        goto fail;
    }
    ESP_LOGI(TAG, "session ready (model %s)", ring_net_s.model);
    return ESP_OK;

fail:
    ring_net_s.last_err = err;
    /* Tear the peer down (through its owning task) so a slow SDP POST cannot
     * be answered into a handle that no longer exists. */
    ring_net_peer_disconnect();
    if (ring_net_peer_close() != ESP_OK) {
        ESP_LOGW(TAG, "peer close after failure timed out");
    }
    ring_net_state_set(RING_NET_ERROR);
    return err;
}

esp_err_t ring_net_stop(void)
{
    ring_net_peer_disconnect();
    if (ring_net_peer_close() != ESP_OK) {
        ESP_LOGW(TAG, "peer close timed out");
    }
    ring_net_state_set(RING_NET_OFF);
    return ESP_OK;
}

ring_net_state_t ring_net_state(void)
{
    if (ring_net_s.state_mutex != NULL) {
        xSemaphoreTake(ring_net_s.state_mutex, portMAX_DELAY);
    }
    ring_net_state_t st = ring_net_s.state;
    if (ring_net_s.state_mutex != NULL) {
        xSemaphoreGive(ring_net_s.state_mutex);
    }
    return st;
}

esp_err_t ring_net_set_listening(bool on)
{
    if (on) {
        if (ring_net_state() != RING_NET_READY) {
            return ESP_ERR_INVALID_STATE;
        }
        ring_net_s.speaking = false;
        ring_net_state_set(RING_NET_LISTENING);
    } else if (ring_net_state() == RING_NET_LISTENING) {
        /* Releasing while the model is already speaking must not cut the
         * reply short; the reply finishes and response.done returns us to
         * READY. */
        ring_net_state_set(RING_NET_READY);
    }
    return ESP_OK;
}

esp_err_t ring_net_send_audio(const int16_t *pcm, size_t samples)
{
    if (ring_net_s.enc == NULL || pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples != RING_NET_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ring_net_state() != RING_NET_LISTENING) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Encode exactly one Opus frame per call (the caller feeds
     * RING_NET_FRAME_SAMPLES at a time). The public API is mono while the
     * encoder is stereo, so duplicate the samples across L/R. */
    int16_t *stereo = (int16_t *)ring_net_s.enc_in;
    for (size_t i = 0; i < samples; i++) {
        stereo[2 * i] = pcm[i];
        stereo[2 * i + 1] = pcm[i];
    }
    esp_audio_enc_in_frame_t in = {
        .buffer = ring_net_s.enc_in,
        .len = (uint32_t)(samples * RING_NET_OPUS_CHANNELS * 2),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = ring_net_s.enc_out,
        .len = (uint32_t)ring_net_s.enc_out_size,
    };
    esp_audio_err_t e = esp_audio_enc_process(ring_net_s.enc, &in, &out);
    if (e != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        return ESP_FAIL;
    }

    /* RTP presentation timestamp: the sample count, not the wall clock.
     * The peer library advances it by the frame size; a ms timestamp here
     * makes the model reject the stream. Reset per session (see
     * ring_net_start) so a reconnect starts from a known offset. */
    esp_peer_audio_frame_t frame = {
        .data = ring_net_s.enc_out,
        .size = (int)out.encoded_bytes,
        .pts = ring_net_s.pts,
    };

    /* The peer handle belongs to the peer task; send under its lock. */
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ring_net_s.peer_lock, portMAX_DELAY);
    if (ring_net_s.peer != NULL) {
        ring_net_s.pts += samples;
        int r = esp_peer_send_audio(ring_net_s.peer, &frame);
        ret = (r == ESP_PEER_ERR_NONE || r == ESP_PEER_ERR_WOULD_BLOCK) ? ESP_OK
                                                                        : ESP_FAIL;
    }
    xSemaphoreGive(ring_net_s.peer_lock);
    return ret;
}

esp_err_t ring_net_read_audio(int16_t *pcm, size_t samples, size_t *out_samples,
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

bool ring_net_is_speaking(void)
{
    return ring_net_s.speaking;
}
