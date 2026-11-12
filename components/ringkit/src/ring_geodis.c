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
 *  - Application code is never called from the peer task. State transitions
 *    and transcript fragments are queued and delivered on `ring-geodis-event`,
 *    in order, one event per item — a burst of READY -> LISTENING -> SPEAKING
 *    is three events, not one overwritten cell.
 *  - The peer handle has one owner (`ring-geodis-peer`). Everybody else uses
 *    it under peer_lock and asks for teardown via peer_close_req, so stop()
 *    can never free the DTLS state out from under the main loop.
 *
 * This file owns the public API, the module state, the event queue and the
 * event task. The token + SDP exchange and peer bring-up live in
 * ring_geodis_signaling.c, the Opus codec in ring_geodis_codec.c and the
 * DataChannel JSON protocol in ring_geodis_protocol.c.
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Wrap-safe: the timestamp counter rolls over after 49.7 days, and
 * `now >= deadline` is then true forever. */
static bool deadline_passed(uint32_t deadline)
{
    return (int32_t)(now_ms() - deadline) >= 0;
}

/* ------------------------------------------------------------- event queue */

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

    if (ring_geodis_s.event_queue == NULL) {
        return;
    }
    ring_geodis_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = RING_GEODIS_EV_STATE;
    ev.state = st;
    if (xQueueSend(ring_geodis_s.event_queue, &ev, 0) != pdTRUE) {
        /* Only reachable if the app's callback is slower than a state storm;
         * the polled ring_geodis_state() is still correct. */
        ESP_LOGW(TAG, "event queue full, state %s not delivered",
                 ring_geodis_state_name(st));
    }
}

void ring_geodis_emit_text(const char *text, size_t len, bool is_user, bool final)
{
    if (ring_geodis_s.event_queue == NULL || text == NULL || len == 0) {
        return;
    }
    ring_geodis_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = RING_GEODIS_EV_TEXT;
    ev.is_user = is_user ? 1 : 0;
    ev.final = final ? 1 : 0;
    if (len > sizeof(ev.text) - 1) {
        len = sizeof(ev.text) - 1;
    }
    memcpy(ev.text, text, len);
    ev.text[len] = '\0';
    ev.len = (uint16_t)len;

    if (xQueueSend(ring_geodis_s.event_queue, &ev, 0) != pdTRUE) {
        /* Dropping a fragment costs a word on screen, never correctness: the
         * `…done` event that follows carries the whole line. */
        ESP_LOGW(TAG, "event queue full, transcript fragment dropped (%u B)",
                 (unsigned)len);
    }
}

/* ------------------------------------------------------------- tasks */

static void event_task(void *arg)
{
    (void)arg;
    ring_geodis_event_t ev;
    for (;;) {
        if (xQueueReceive(ring_geodis_s.event_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ev.kind == RING_GEODIS_EV_STATE) {
            if (ring_geodis_s.state_cb != NULL) {
                ring_geodis_s.state_cb(ev.state, ring_geodis_s.user);
            }
        } else if (ev.kind == RING_GEODIS_EV_TEXT) {
            if (ring_geodis_s.text_cb != NULL) {
                ring_geodis_s.text_cb(ev.text, ev.len, ev.is_user != 0,
                                      ev.final != 0, ring_geodis_s.user);
            }
        }
    }
}

/* ------------------------------------------------------------- cleanup */

/* Free every resource ring_geodis_init() may have allocated. Safe to call on a
 * zero-initialised context and on partial init failure. Tasks are created last,
 * so nothing here runs while one of them could be using these objects. */
static void geodis_release(void)
{
    if (ring_geodis_s.event_queue) { vQueueDeleteWithCaps(ring_geodis_s.event_queue); ring_geodis_s.event_queue = NULL; }
    if (ring_geodis_s.tool_queue) { vQueueDelete(ring_geodis_s.tool_queue); ring_geodis_s.tool_queue = NULL; }
    if (ring_geodis_s.state_mutex) { vSemaphoreDelete(ring_geodis_s.state_mutex); ring_geodis_s.state_mutex = NULL; }
    if (ring_geodis_s.sdp_mutex) { vSemaphoreDelete(ring_geodis_s.sdp_mutex); ring_geodis_s.sdp_mutex = NULL; }
    if (ring_geodis_s.dec_mutex) { vSemaphoreDelete(ring_geodis_s.dec_mutex); ring_geodis_s.dec_mutex = NULL; }
    if (ring_geodis_s.peer_lock) { vSemaphoreDelete(ring_geodis_s.peer_lock); ring_geodis_s.peer_lock = NULL; }
    if (ring_geodis_s.peer_gone) { vSemaphoreDelete(ring_geodis_s.peer_gone); ring_geodis_s.peer_gone = NULL; }
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
    /* Re-initialising would leak the codec, the PSRAM buffers and the
     * semaphores, and orphan three tasks. It is a no-op instead. */
    if (ring_geodis_s.inited) {
        return ESP_OK;
    }

    memset(&ring_geodis_s, 0, sizeof(ring_geodis_s));
    ring_geodis_s.state = RING_GEODIS_OFF;
    ring_geodis_s.state_cb = state_cb;
    ring_geodis_s.text_cb = text_cb;
    ring_geodis_s.tool_cb = tool_cb;
    ring_geodis_s.user = user;

    ring_geodis_s.sdp_mutex = xSemaphoreCreateMutex();
    ring_geodis_s.state_mutex = xSemaphoreCreateMutex();
    ring_geodis_s.peer_lock = xSemaphoreCreateMutex();
    ring_geodis_s.peer_gone = xSemaphoreCreateBinary();
    ring_geodis_s.tool_queue = xQueueCreate(2, sizeof(ring_geodis_tool_job_t));
    /* ~5 KB of events: PSRAM, because internal RAM is where the panel buffers
     * and the IMU task's stack live. */
    ring_geodis_s.event_queue = xQueueCreateWithCaps(RING_GEODIS_EVENT_DEPTH,
                                                     sizeof(ring_geodis_event_t),
                                                     MALLOC_CAP_SPIRAM);
    if (ring_geodis_s.sdp_mutex == NULL || ring_geodis_s.state_mutex == NULL ||
        ring_geodis_s.peer_lock == NULL || ring_geodis_s.peer_gone == NULL ||
        ring_geodis_s.tool_queue == NULL || ring_geodis_s.event_queue == NULL) {
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

    BaseType_t ok = xTaskCreate(ring_geodis_signal_task, "ring-geodis-signal",
                                RING_GEODIS_SIGNAL_TASK_STACK, NULL,
                                RING_GEODIS_SIGNAL_TASK_PRIO,
                                &ring_geodis_s.signal_task);
    if (ok == pdPASS) {
        ok = xTaskCreate(event_task, "ring-geodis-event",
                         RING_GEODIS_EVENT_TASK_STACK, NULL,
                         RING_GEODIS_EVENT_TASK_PRIO, &ring_geodis_s.event_task);
    }
    if (ok == pdPASS) {
        ok = xTaskCreate(ring_geodis_tool_task, "ring-geodis-tool",
                         RING_GEODIS_TOOL_TASK_STACK, NULL,
                         RING_GEODIS_TOOL_TASK_PRIO, &ring_geodis_s.tool_task);
    }
    if (ok != pdPASS) {
        /* Roll back what did start before freeing the objects they block on:
         * deleting a queue under a blocked task is a use-after-free. */
        ESP_LOGE(TAG, "task creation failed");
        if (ring_geodis_s.signal_task) { vTaskDelete(ring_geodis_s.signal_task); ring_geodis_s.signal_task = NULL; }
        if (ring_geodis_s.event_task) { vTaskDelete(ring_geodis_s.event_task); ring_geodis_s.event_task = NULL; }
        if (ring_geodis_s.tool_task) { vTaskDelete(ring_geodis_s.tool_task); ring_geodis_s.tool_task = NULL; }
        geodis_release();
        return ESP_ERR_NO_MEM;
    }

    ring_geodis_s.inited = true;
    ESP_LOGI(TAG, "initialised");
    return ESP_OK;
}

esp_err_t ring_geodis_start(const ring_geodis_cfg_t *cfg)
{
    if (!ring_geodis_s.inited || cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NULL means "the documented default" for every pointer field, and -1 for
     * the two flags (0 = the caller explicitly turned it off). */
    snprintf(ring_geodis_s.base_url, sizeof(ring_geodis_s.base_url), "%s",
             (cfg->base_url != NULL) ? cfg->base_url : RING_GEODIS_DEFAULT_BASE_URL);
    snprintf(ring_geodis_s.sdp_url, sizeof(ring_geodis_s.sdp_url), "%s",
             (cfg->sdp_url != NULL) ? cfg->sdp_url : RING_GEODIS_DEFAULT_SDP_URL);
    snprintf(ring_geodis_s.voice, sizeof(ring_geodis_s.voice), "%s",
             (cfg->voice != NULL && cfg->voice[0] != '\0') ? cfg->voice : "coral");
    snprintf(ring_geodis_s.system_prompt, sizeof(ring_geodis_s.system_prompt), "%s",
             (cfg->system_prompt != NULL) ? cfg->system_prompt : "");
    ring_geodis_s.enable_stt = (cfg->enable_stt < 0) ? true : (cfg->enable_stt != 0);
    ring_geodis_s.enable_ptt = (cfg->enable_ptt < 0) ? true : (cfg->enable_ptt != 0);
    /* Log the *effective* config, not the struct: a caller that left the flags
     * zeroed gets PTT and STT off, and this line is where that becomes visible
     * instead of a conversation that answers without a transcript. */
    ESP_LOGI(TAG, "config: %s, voice %s, %s, %s", ring_geodis_s.base_url,
             ring_geodis_s.voice,
             ring_geodis_s.enable_ptt ? "push-to-talk" : "server VAD",
             ring_geodis_s.enable_stt ? "stt on" : "stt off");

    /* A fresh chat_session_id per session (transcript persistence key). */
    ring_rand_seed((uint32_t)esp_timer_get_time());
    snprintf(ring_geodis_s.chat_session_id, sizeof(ring_geodis_s.chat_session_id),
             "ring-%08x", (unsigned)ring_rand_u32());

    /* A retry must start from a clean peer: reusing a half-open connection
     * would send a second offer over an existing DTLS session. The peer task
     * owns the handle, so teardown goes through it — and if it will not let go,
     * opening a second peer would leak the first. */
    esp_err_t cerr = ring_geodis_peer_close();
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "previous peer would not close: %s", esp_err_to_name(cerr));
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return cerr;
    }
    /* Whatever the signal task is still working on is now stale. */
    ring_geodis_s.session_gen++;
    ring_geodis_s.last_err = ESP_OK;

    ring_geodis_s.speaking = false;
    ring_geodis_s.uplink_open = false;
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
        goto fail;
    }
    err = ring_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 2. Clock — required for TLS/DTLS certificate validation. */
    err = ring_time_sync(RING_GEODIS_SNTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto fail;
    }

    /* 3. Ephemeral token. The gateway resets ~1 in 12 TLS connections, so the
     * POST retries internally. */
    err = ring_geodis_fetch_token();
    if (err != ESP_OK) {
        goto fail;
    }

    /* 4. PeerConnection + signaling */
    err = ring_geodis_peer_open();
    if (err != ESP_OK) {
        goto fail;
    }
    int r = esp_peer_new_connection(ring_geodis_s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "new_connection failed: %d", r);
        err = ESP_FAIL;
        goto fail;
    }
    /* The offer has been produced; the main loop may now drive the session. */
    ring_geodis_s.peer_ready = true;

    /* 5. Wait for the DataChannel + session.updated. The deadline is a
     * backstop: the signal task reports its own failure by moving the state to
     * ERROR, so a slow gateway does not have to burn the whole window. */
    ESP_LOGI(TAG, "waiting for session...");
    uint32_t deadline = now_ms() + RING_GEODIS_SESSION_TIMEOUT_MS;
    for (;;) {
        if (ring_geodis_state() != RING_GEODIS_CONNECTING) {
            break;
        }
        if (deadline_passed(deadline)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        ring_geodis_state_t st = ring_geodis_state();
        if (st == RING_GEODIS_READY || st == RING_GEODIS_LISTENING ||
            st == RING_GEODIS_SPEAKING) {
            ESP_LOGI(TAG, "session ready");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "session not ready (%s)", ring_geodis_state_name(st));
        /* Leave a broken peer behind rather than a half-open DTLS session that
         * the next start() would have to reason about. */
        ring_geodis_peer_close();
        ring_geodis_state_set(RING_GEODIS_ERROR);
        return (ring_geodis_s.last_err != ESP_OK) ? ring_geodis_s.last_err
                                                 : ESP_ERR_TIMEOUT;
    }

fail:
    ring_geodis_s.last_err = err;
    ring_geodis_peer_close();
    ring_geodis_state_set(RING_GEODIS_ERROR);
    return err;
}

esp_err_t ring_geodis_stop(void)
{
    ring_geodis_s.uplink_open = false;
    ring_geodis_s.speaking = false;
    /* Nothing may be delivered for a session that is going away. */
    ring_geodis_s.session_gen++;
    ring_geodis_peer_close();
    ring_byte_ring_reset(&ring_geodis_s.dl_ring);
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
    if (!on) {
        ring_geodis_s.uplink_open = false;
        if (ring_geodis_state() == RING_GEODIS_LISTENING) {
            /* Releasing while the model is already speaking must not cut the
             * reply short; the reply finishes and response.done returns us to
             * READY. */
            ring_geodis_state_set(RING_GEODIS_READY);
        }
        return ESP_OK;
    }

    ring_geodis_state_t st = ring_geodis_state();
    if (st != RING_GEODIS_READY && st != RING_GEODIS_SPEAKING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ring_geodis_s.speaking || st == RING_GEODIS_SPEAKING) {
        /* Barge-in: cancel the reply and drop the audio already decoded into
         * the playback buffer, or the ring keeps talking into its own mic. */
        ring_geodis_cancel();
    }
    ring_geodis_s.uplink_open = true;
    ring_geodis_state_set(RING_GEODIS_LISTENING);
    return ESP_OK;
}

esp_err_t ring_geodis_send_audio(const int16_t *pcm, size_t samples)
{
    if (pcm == NULL || samples != RING_GEODIS_FRAME_SAMPLES ||
        ring_geodis_s.enc == NULL || !ring_geodis_s.uplink_open) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Encode outside the lock: only the peer handle needs protecting, and the
     * encoder belongs to the caller's audio task alone. */
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

    esp_err_t ret = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ring_geodis_s.peer_lock, portMAX_DELAY);
    if (ring_geodis_s.peer != NULL) {
        ring_geodis_s.pts += samples;
        int r = esp_peer_send_audio(ring_geodis_s.peer, &frame);
        ret = (r == ESP_PEER_ERR_NONE || r == ESP_PEER_ERR_WOULD_BLOCK) ? ESP_OK
                                                                      : ESP_FAIL;
    }
    xSemaphoreGive(ring_geodis_s.peer_lock);
    return ret;
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
        got += ring_byte_ring_pop(&ring_geodis_s.dl_ring, (uint8_t *)pcm + got,
                                  want - got);
        if (got >= want || deadline_passed(deadline)) {
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
