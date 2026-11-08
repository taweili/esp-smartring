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
 * Memory discipline (this ran out of both heap and stack on the first cut):
 *  - No large stack arrays: SDP buffers are heap-allocated.
 *  - The peer callback runs on the peer thread that also runs
 *    esp_peer_main_loop — it must stay tiny and must not log big strings.
 *  - State changes are marshalled to a worker task, so the application's
 *    callback (which takes the LVGL lock) never runs on a peer thread.
 */
#include "ringkit/ring_net.h"
#include "ringkit/ring_wifi.h"
#include "ringkit/ring_ca.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_json.h"
#include "ringkit/ring_byte_ring.h"
#include "ringkit/ring_http.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "arpa/inet.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_dec_default.h"
#include "esp_opus_dec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define TAG "ring_net"

const char *ring_net_ca_pem(void)
{
    return ring_ca_pem();
}

size_t ring_net_ca_pem_len(void)
{
    return ring_ca_pem_len();
}

/* ------------------------------------------------------------- config */

#ifndef RING_NET_HOST
#define RING_NET_HOST "maas.qianwenaiapi.com"
#endif

/* The endpoint only accepts Opus on m=audio, so this is fixed at 48 kHz.
 * RING_NET_FRAME_SAMPLES (960 = 20 ms) comes from the public header. */
#define RING_NET_SAMPLE_RATE    48000
#define RING_NET_CHANNELS       1   /* PCM on the public API is mono */
/* esp_peer offers `opus/48000/2` for Opus no matter what audio_info.channel
 * says, so the encoder must produce stereo to match the negotiated SDP;
 * ring_net_send_audio() up-mixes the mono API to stereo. */
#define RING_NET_OPUS_CHANNELS  2
#define RING_NET_FRAME_BYTES    (RING_NET_FRAME_SAMPLES * 2)   /* 16-bit */
#define RING_NET_BITRATE        32000

/* Downlink PCM ring buffer: 4 s of 48 kHz / 16-bit / mono = 384 KB (PSRAM). */
#define RING_NET_DL_BYTES       (RING_NET_SAMPLE_RATE * 2 * 4)

/* Internal RAM is scarce (250 Hz IMU task + LVGL panel buffers live there),
 * so keep our tasks lean. TLS work happens in the esp-tls task, not here. */
/* esp_peer_main_loop also runs the DTLS handshake, which is stack-hungry
 * (mbedTLS). 16 KB is sized from the observed high-water mark. */
#define PEER_TASK_STACK        16384
#define PEER_TASK_PRIO          5
#define SIGNAL_TASK_STACK       4096
#define SIGNAL_TASK_PRIO        4
/* Runs the app callback, which takes the LVGL lock and renders labels with
 * the 48 px font; 3.5 KB was tight. */
#define EVENT_TASK_STACK        6144
#define EVENT_TASK_PRIO         4

#define SDP_MAX_BYTES           4096
#define HTTP_TIMEOUT_MS         10000
#define SNTP_TIMEOUT_MS         15000
#define SESSION_TIMEOUT_MS      20000

/* ------------------------------------------------------------- state */

typedef struct {
    ring_net_state_t         state;
    ring_net_state_cb_t      state_cb;
    ring_net_transcript_cb_t text_cb;
    void                    *user;

    esp_peer_handle_t        peer;
    TaskHandle_t             peer_task;
    TaskHandle_t             signal_task;
    TaskHandle_t             event_task;

    char                     model[64];
    char                     api_key[128];

    /* SCTP stream the server's event channel ('txt') uses. The server pushes
     * session events on its own channel and expects replies on the same one,
     * so sends must carry this id (esp_peer defaults to stream 0, which is
     * not a channel the server opened and makes esp_peer_send_data fail). */
    uint16_t                 send_stream_id;

    /* Local Offer handed from the peer callback to the signalling task. */
    char                     local_sdp[SDP_MAX_BYTES];
    SemaphoreHandle_t        sdp_mutex;

    /* Opus codec. Encoder runs on the app's audio task, decoder on the peer
     * thread, hence separate handles and a lock around the decoder. */
    esp_audio_enc_handle_t   enc;
    int                      enc_in_size;
    int                      enc_out_size;
    uint8_t                 *enc_out;
    uint8_t                 *enc_in;      /* mono -> stereo up-mix buffer */

    esp_audio_dec_handle_t   dec;
    SemaphoreHandle_t        dec_mutex;
    uint8_t                 *dec_out;
    int                      dec_out_size;
    /* Stereo decode -> mono fold scratch (the ring's audio path is mono). */
    uint8_t                 *dec_mono;

    /* Downlink PCM ring buffer (48 kHz mono). */
    ring_byte_ring_t         dl_ring;

    QueueHandle_t            event_queue;
    SemaphoreHandle_t        state_mutex;
    volatile bool            speaking;

    /* RTP presentation timestamp for the current session (sample count). */
    uint32_t                 pts;
} ring_net_ctx_t;

static ring_net_ctx_t s;

/* FreeRTOS calls this on stack overflow. The stock hook aborts with an
 * unusable backtrace; name the offending task and its headroom instead. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_DRAM_LOGE("ring_net", "*** STACK OVERFLOW in task '%s' (hwm %u words)",
                  pcTaskName, (unsigned)uxTaskGetStackHighWaterMark(xTask));
    abort();
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

/* Updates the state and notifies the event task, which runs the app callback.
 * Safe to call from any task, including the peer thread. */
static void state_set(ring_net_state_t st)
{
    if (s.state_mutex != NULL) {
        xSemaphoreTake(s.state_mutex, portMAX_DELAY);
    }
    if (s.state == st) {
        if (s.state_mutex != NULL) {
            xSemaphoreGive(s.state_mutex);
        }
        return;
    }
    s.state = st;
    if (s.state_mutex != NULL) {
        xSemaphoreGive(s.state_mutex);
    }
    ESP_LOGI(TAG, "state -> %s", ring_net_state_name(st));
    if (s.event_queue) {
        xQueueOverwrite(s.event_queue, &st);
        if (s.event_task) {
            xTaskNotifyGive(s.event_task);
        }
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------- downlink ring */

static void dl_push(const uint8_t *pcm, size_t len)
{
    size_t dropped = 0;
    ring_byte_ring_push(&s.dl_ring, pcm, len, &dropped);
}

static size_t dl_pop(uint8_t *pcm, size_t len)
{
    return ring_byte_ring_pop(&s.dl_ring, pcm, len);
}

/* ------------------------------------------------------------- codec */

static esp_err_t opus_open(void)
{
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RING_NET_SAMPLE_RATE;
    cfg.channel = RING_NET_OPUS_CHANNELS;
    cfg.bits_per_sample = 16;
    cfg.bitrate = RING_NET_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 0;          /* lowest CPU: the ESP32-S3 is also running Wi-Fi */
    cfg.enable_fec = true;
    cfg.enable_vbr = false;

    esp_audio_enc_config_t ecfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    esp_audio_err_t e = esp_audio_enc_open(&ecfg, &s.enc);
    if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encoder open failed: %d", e);
        return ESP_FAIL;
    }
    esp_audio_enc_get_frame_size(s.enc, &s.enc_in_size, &s.enc_out_size);
    s.enc_out = malloc(s.enc_out_size > 0 ? s.enc_out_size : 1024);
    s.enc_in = malloc(RING_NET_FRAME_SAMPLES * RING_NET_OPUS_CHANNELS * 2);
    if (s.enc_out == NULL || s.enc_in == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "opus encoder: in %d B out %d B", s.enc_in_size, s.enc_out_size);

    esp_opus_dec_cfg_t dcfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dcfg.sample_rate = RING_NET_SAMPLE_RATE;
    /* The endpoint negotiates stereo Opus (a=rtpmap:opus/48000/2), so the
     * decoder must be opened for two channels; on_audio_data folds to mono. */
    dcfg.channel = ESP_AUDIO_DUAL;
    dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;

    esp_audio_dec_cfg_t adcfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &dcfg,
        .cfg_sz = sizeof(dcfg),
    };
    e = esp_audio_dec_open(&adcfg, &s.dec);
    if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus decoder open failed: %d", e);
        return ESP_FAIL;
    }
    /* Worst case output for one 20 ms frame: stereo 16-bit PCM. */
    s.dec_out_size = RING_NET_FRAME_BYTES * 2;
    s.dec_out = malloc(s.dec_out_size);
    s.dec_mono = malloc(RING_NET_FRAME_BYTES);
    if (s.dec_out == NULL || s.dec_mono == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s.dec_mutex = xSemaphoreCreateMutex();
    return s.dec_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------- network gate */

esp_err_t ring_net_sync_clock(void)
{
    return ring_time_sync(SNTP_TIMEOUT_MS);
}

int ring_net_probe(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return -1;
    }
    esp_http_client_config_t http = {
        .url = url,
        .method = HTTP_METHOD_GET,
        /* Generic public host: use the Mozilla bundle, not the Qianwen CA. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&http);
    if (c == NULL) {
        ESP_LOGW(TAG, "probe %s: init failed", url);
        return -1;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "probe %s failed: %s (HTTP %d)", url,
                 esp_err_to_name(err), status);
        return -1;
    }
    ESP_LOGI(TAG, "probe %s -> HTTP %d", url, status);
    return status;
}

/* ------------------------------------------------------------- SDP exchange */

/* esp_peer emits SDP with bare LF; WebRTC wants CRLF. */
static void normalise_sdp_crlf(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_len; i++) {
        if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r')) {
            out[j++] = '\r';
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

static void log_http_error(esp_http_client_handle_t c)
{
    char buf[257];
    int n = esp_http_client_read_response(c, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        ESP_LOGW(TAG, "HTTP %d: %s", esp_http_client_get_status_code(c), buf);
    } else {
        ESP_LOGW(TAG, "HTTP %d (no body)", esp_http_client_get_status_code(c));
    }
}

/**
 * POST the Offer to the Qianwen WebRTC endpoint and return the Answer SDP.
 * All buffers are on the heap so this can run on a small task stack.
 */
static esp_err_t sdp_exchange(const char *offer, char *answer, size_t answer_len)
{
    char url[192];
    snprintf(url, sizeof(url), "https://%s/api/v1/webrtc/realtime?model=%s",
             RING_NET_HOST, s.model);

    char *offer_crlf = malloc(SDP_MAX_BYTES);
    if (offer_crlf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    normalise_sdp_crlf(offer, offer_crlf, SDP_MAX_BYTES);

    /* The bundled cert chain for this endpoint (assets/qianwen_ca.pem):
     * the server does not send its intermediate CA. */
    esp_http_client_handle_t c = ring_http_init(url, ring_ca_pem(),
                                                ring_ca_pem_len(),
                                                HTTP_TIMEOUT_MS,
                                                SDP_MAX_BYTES, 1024);
    if (c == NULL) {
        free(offer_crlf);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/sdp");
    ring_http_set_bearer(c, s.api_key);

    /* Stream instead of perform(): perform() swallows the response body, and
     * the Answer SDP *is* the body. */
    esp_err_t err = esp_http_client_open(c, (int)strlen(offer_crlf));
    if (err == ESP_OK) {
        if (esp_http_client_write(c, offer_crlf, (int)strlen(offer_crlf)) < 0) {
            err = ESP_FAIL;
        }
    }
    free(offer_crlf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDP POST open/write failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    int total = esp_http_client_get_content_length(c);

    if (status != 200) {
        ESP_LOGE(TAG, "SDP POST returned HTTP %d", status);
        log_http_error(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Read the whole Answer (chunked responses report length -1). */
    int n = 0;
    esp_err_t rerr = ring_http_read_body(c, answer, answer_len, &n);
    esp_http_client_cleanup(c);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "SDP answer empty (content-length %d)", total);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "SDP answer: %d bytes", n);
    return ESP_OK;
}

/* ------------------------------------------------------------- peer callbacks
 *
 * Everything here runs on the peer task, which also runs
 * esp_peer_main_loop: keep the stack small and never log big strings.
 */

static int on_state(esp_peer_state_t state, void *ctx)
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
        state_set(RING_NET_ERROR);
        break;
    default:
        break;
    }
    return 0;
}

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP || msg->data == NULL) {
        return 0;
    }
    if ((size_t)msg->size >= SDP_MAX_BYTES) {
        ESP_LOGE(TAG, "local SDP too big");
        return 0;
    }
    xSemaphoreTake(s.sdp_mutex, portMAX_DELAY);
    memcpy(s.local_sdp, msg->data, (size_t)msg->size);
    s.local_sdp[msg->size] = '\0';
    xSemaphoreGive(s.sdp_mutex);

    ESP_LOGI(TAG, "local SDP ready (%u bytes)", (unsigned)strlen(s.local_sdp));
    /* Full dumps only at DEBUG: the console is USB-Serial-JTAG and its writes
     * block when the host is not reading, so keep the hot path small. */
    ESP_LOGD(TAG, "LOCAL SDP:\n%s", s.local_sdp);
    if (s.signal_task) {
        xTaskNotifyGive(s.signal_task);
    }
    return 0;
}

static int on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "remote audio: codec %d, %lu Hz, %u ch",
             info->codec, (unsigned long)info->sample_rate, info->channel);
    return 0;
}

static int on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0 || s.dec == NULL) {
        return 0;
    }

    xSemaphoreTake(s.dec_mutex, portMAX_DELAY);
    esp_audio_dec_in_raw_t raw = {
        .buffer = frame->data,
        .len = (uint32_t)frame->size,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = s.dec_out,
        .len = (uint32_t)s.dec_out_size,
    };
    esp_audio_err_t e = esp_audio_dec_process(s.dec, &raw, &out);
    xSemaphoreGive(s.dec_mutex);

    if (e == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
        /* The endpoint sends stereo; the ring's audio path is mono. Fold
         * L/R here (the decoder is opened for two channels, so the buffer
         * holds interleaved stereo and decoded_size counts both channels). */
        const int16_t *stereo = (const int16_t *)s.dec_out;
        size_t frames = out.decoded_size / (2 * sizeof(int16_t));
        if (frames > RING_NET_FRAME_SAMPLES) {
            frames = RING_NET_FRAME_SAMPLES;
        }
        int16_t *mono = (int16_t *)s.dec_mono;
        for (size_t i = 0; i < frames; i++) {
            mono[i] = (int16_t)(((int32_t)stereo[2 * i] +
                                 (int32_t)stereo[2 * i + 1]) / 2);
        }
        dl_push(s.dec_mono, frames * sizeof(int16_t));
    } else if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "opus decode failed: %d", e);
    }

    s.speaking = true;
    /* The reply can start after the button was released (state READY), so
     * accept both pre-turn states here. */
    ring_net_state_t st = ring_net_state();
    if (st == RING_NET_LISTENING || st == RING_NET_READY) {
        state_set(RING_NET_SPEAKING);
    }
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "data channel '%s' open (stream %u)", ch->label, ch->stream_id);
    /* Remember the server's channel: replies must go back on it. */
    s.send_stream_id = ch->stream_id;
    /* The session is configured when the server sends session.created (see
     * on_data_frame): sending session.update from here races that event. */
    return 0;
}

/* DataChannel frames arrive as JSON strings. */
static int on_data_frame(esp_peer_data_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0) {
        return 0;
    }
    /* Remember which channel the event arrived on; replies go back on it. */
    if (frame->stream_id != 0) {
        s.send_stream_id = frame->stream_id;
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
        state_set(RING_NET_READY);
    } else if (ring_json_type_is(json, "response.done")) {
        s.speaking = false;
        if (ring_net_state() == RING_NET_SPEAKING) {
            state_set(RING_NET_READY);
        }
    } else if (ring_json_type_is(json, "error")) {
        ESP_LOGE(TAG, "model error: %.200s", json);
    } else if (ring_json_type_is(json, "conversation.item.input_audio_transcription.completed")) {
        /* The server transcribes the user's utterance by default; surfacing it
         * is the clearest evidence that the uplink arrived intact. */
        char text[256];
        size_t len = 0;
        if (s.text_cb != NULL &&
            ring_json_get_str(json, "transcript", text, sizeof(text), &len)) {
            s.text_cb(text, len, true, s.user);
        }
    } else if (s.text_cb != NULL &&
               (ring_json_type_is(json, "response.text.delta") ||
                ring_json_type_is(json, "response.audio_transcript.delta"))) {
        char text[128];
        size_t len = 0;
        if (ring_json_get_str(json, "delta", text, sizeof(text), &len)) {
            s.text_cb(text, len, false, s.user);
        }
    }
    return 0;
}

/* ------------------------------------------------------------- tasks */

static void event_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ring_net_state_t st;
        if (xQueuePeek(s.event_queue, &st, 0) == pdTRUE && s.state_cb) {
            s.state_cb(st, s.user);
        }
    }
}

static void peer_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    ESP_LOGI(TAG, "peer task started (hwm %u words)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        if (s.peer) {
            esp_peer_main_loop(s.peer);
        }
        if ((ticks++ % 1000) == 0) {
            ESP_LOGI(TAG, "peer task alive (hwm %u words)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void signal_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char *offer = malloc(SDP_MAX_BYTES);
        char *answer = malloc(SDP_MAX_BYTES);
        if (offer == NULL || answer == NULL) {
            free(offer);
            free(answer);
            continue;
        }

        xSemaphoreTake(s.sdp_mutex, portMAX_DELAY);
        strncpy(offer, s.local_sdp, SDP_MAX_BYTES - 1);
        offer[SDP_MAX_BYTES - 1] = '\0';
        xSemaphoreGive(s.sdp_mutex);

        if (offer[0] != '\0' &&
            sdp_exchange(offer, answer, SDP_MAX_BYTES) == ESP_OK) {
            esp_peer_msg_t m = {
                .type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)answer,
                .size = (int)strlen(answer),
            };
            int r = esp_peer_send_msg(s.peer, &m);
            if (r != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "esp_peer_send_msg failed: %d", r);
                state_set(RING_NET_ERROR);
            } else {
                ESP_LOGI(TAG, "remote SDP installed (%u bytes)",
                         (unsigned)strlen(answer));
                ESP_LOGD(TAG, "ANSWER SDP:\n%s", answer);
            }
        } else {
            state_set(RING_NET_ERROR);
        }

        free(offer);
        free(answer);
    }
}

/* ------------------------------------------------------------- cleanup */

/* Free every resource ring_net_init() may have allocated. Safe to call on a
 * zero-initialised context and on partial init failure. */
static void net_release(void)
{
    if (s.event_queue) { vQueueDelete(s.event_queue); s.event_queue = NULL; }
    if (s.state_mutex) { vSemaphoreDelete(s.state_mutex); s.state_mutex = NULL; }
    if (s.sdp_mutex) { vSemaphoreDelete(s.sdp_mutex); s.sdp_mutex = NULL; }
    if (s.dec_mutex) { vSemaphoreDelete(s.dec_mutex); s.dec_mutex = NULL; }
    if (s.enc) { esp_audio_enc_close(s.enc); s.enc = NULL; }
    if (s.dec) { esp_audio_dec_close(s.dec); s.dec = NULL; }
    free(s.enc_out); s.enc_out = NULL;
    free(s.enc_in); s.enc_in = NULL;
    free(s.dec_out); s.dec_out = NULL;
    free(s.dec_mono); s.dec_mono = NULL;
    ring_byte_ring_deinit(&s.dl_ring);
}

/* ------------------------------------------------------------- session */

static esp_err_t peer_open(void)
{
    esp_peer_pre_generate_cert();

    esp_peer_default_cfg_t dcfg = {
        .agent_recv_timeout = 200,
        .data_ch_cfg = {
            .send_cache_size = 8192,
            .recv_cache_size = 8192,
        },
        .rtp_cfg = {
            .audio_recv_jitter = {
                .cache_size = 16384,
                .cache_timeout = 200,
            },
            .send_pool_size = 16384,
            .send_queue_num = 32,
        },
        .max_candidates = 4,
    };

    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLING,
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .audio_info = {
            .codec = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate = RING_NET_SAMPLE_RATE,
            .channel = RING_NET_OPUS_CHANNELS,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .no_auto_reconnect = true,
        .extra_cfg = &dcfg,
        .extra_size = sizeof(dcfg),
        .on_state = on_state,
        .on_msg = on_msg,
        .on_audio_info = on_audio_info,
        .on_audio_data = on_audio_data,
        .on_channel_open = on_channel_open,
        .on_data = on_data_frame,
        .ctx = NULL,
    };

    int r = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", r);
        return ESP_FAIL;
    }
    if (s.peer_task == NULL) {
        xTaskCreate(peer_task, "ring-net-peer", PEER_TASK_STACK, NULL,
                    PEER_TASK_PRIO, &s.peer_task);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------- public API */

esp_err_t ring_net_init(ring_net_state_cb_t state_cb,
                        ring_net_transcript_cb_t text_cb,
                        void *user)
{
    memset(&s, 0, sizeof(s));
    s.state = RING_NET_OFF;
    s.state_cb = state_cb;
    s.text_cb = text_cb;
    s.user = user;

    s.sdp_mutex = xSemaphoreCreateMutex();
    s.state_mutex = xSemaphoreCreateMutex();
    s.event_queue = xQueueCreate(1, sizeof(ring_net_state_t));
    if (s.sdp_mutex == NULL || s.state_mutex == NULL || s.event_queue == NULL) {
        net_release();
        return ESP_ERR_NO_MEM;
    }

    /* The downlink buffer is 384 KB: it must live in PSRAM, not internal RAM. */
    if (ring_byte_ring_init(&s.dl_ring, RING_NET_DL_BYTES, MALLOC_CAP_SPIRAM) != ESP_OK) {
        ESP_LOGW(TAG, "downlink ring allocation failed (%d bytes)",
                 RING_NET_DL_BYTES);
    }

    esp_err_t err = opus_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "opus init failed: %s", esp_err_to_name(err));
        net_release();
        return err;
    }

    xTaskCreate(signal_task, "ring-net-signal", SIGNAL_TASK_STACK, NULL,
                SIGNAL_TASK_PRIO, &s.signal_task);
    xTaskCreate(event_task, "ring-net-event", EVENT_TASK_STACK, NULL,
                EVENT_TASK_PRIO, &s.event_task);

    ESP_LOGI(TAG, "initialised");
    return ESP_OK;
}

esp_err_t ring_net_start(const char *model, const char *api_key)
{
    if (model == NULL || api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s.model, sizeof(s.model), "%s", model);
    snprintf(s.api_key, sizeof(s.api_key), "%s", api_key);

    /* A retry must start from a clean peer: reusing a half-open connection
     * would send a second offer over an existing DTLS session. */
    if (s.peer) {
        esp_peer_disconnect(s.peer);
        esp_peer_close(s.peer);
        s.peer = NULL;
    }
    s.speaking = false;
    s.pts = 0;
    /* A retry must not replay stale downlink audio or reuse a stale SCTP
     * stream id from the previous session. */
    ring_byte_ring_reset(&s.dl_ring);
    s.send_stream_id = 0;
    state_set(RING_NET_CONNECTING);

    /* 1. Wi-Fi */
    esp_err_t err = ring_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        state_set(RING_NET_ERROR);
        return err;
    }
    err = ring_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP: %s", esp_err_to_name(err));
        state_set(RING_NET_ERROR);
        return err;
    }

    /* 2. Clock — required for TLS/DTLS certificate validation. */
    err = ring_time_sync(SNTP_TIMEOUT_MS);
    if (err != ESP_OK) {
        state_set(RING_NET_ERROR);
        return err;
    }

    /* 3. PeerConnection + signaling */
    err = peer_open();
    if (err != ESP_OK) {
        state_set(RING_NET_ERROR);
        return err;
    }
    int r = esp_peer_new_connection(s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "new_connection failed: %d", r);
        state_set(RING_NET_ERROR);
        return ESP_FAIL;
    }

    /* 4. Wait for the DataChannel + session.updated. */
    ESP_LOGI(TAG, "waiting for session...");
    uint32_t deadline = now_ms() + SESSION_TIMEOUT_MS;
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
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "session ready (model %s)", s.model);
    return ESP_OK;
}

esp_err_t ring_net_stop(void)
{
    if (s.peer) {
        esp_peer_disconnect(s.peer);
        esp_peer_close(s.peer);
        s.peer = NULL;
    }
    state_set(RING_NET_OFF);
    return ESP_OK;
}

ring_net_state_t ring_net_state(void)
{
    if (s.state_mutex != NULL) {
        xSemaphoreTake(s.state_mutex, portMAX_DELAY);
    }
    ring_net_state_t st = s.state;
    if (s.state_mutex != NULL) {
        xSemaphoreGive(s.state_mutex);
    }
    return st;
}

esp_err_t ring_net_set_listening(bool on)
{
    if (on) {
        if (ring_net_state() != RING_NET_READY) {
            return ESP_ERR_INVALID_STATE;
        }
        s.speaking = false;
        state_set(RING_NET_LISTENING);
    } else if (ring_net_state() == RING_NET_LISTENING) {
        /* Releasing while the model is already speaking must not cut the
         * reply short; the reply finishes and response.done returns us to
         * READY. */
        state_set(RING_NET_READY);
    }
    return ESP_OK;
}

esp_err_t ring_net_send_audio(const int16_t *pcm, size_t samples)
{
    if (s.peer == NULL || pcm == NULL || samples == 0 || s.enc == NULL) {
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
    int16_t *stereo = (int16_t *)s.enc_in;
    for (size_t i = 0; i < samples; i++) {
        stereo[2 * i] = pcm[i];
        stereo[2 * i + 1] = pcm[i];
    }
    esp_audio_enc_in_frame_t in = {
        .buffer = s.enc_in,
        .len = (uint32_t)(samples * RING_NET_OPUS_CHANNELS * 2),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = s.enc_out,
        .len = (uint32_t)s.enc_out_size,
    };
    esp_audio_err_t e = esp_audio_enc_process(s.enc, &in, &out);
    static int s_hwm;                    /* TEMP DIAG */
    if (s_hwm < 3) {
        ESP_LOGI(TAG, "enc: %d B in, %d B out, ret %d, stack hwm %u words",
                 (int)in.len, (int)out.encoded_bytes, e,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        s_hwm++;
    }
    if (e != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        return ESP_FAIL;
    }

    /* RTP presentation timestamp: the sample count, not the wall clock.
     * The peer library advances it by the frame size; a ms timestamp here
     * makes the model reject the stream. Reset per session (see
     * ring_net_start) so a reconnect starts from a known offset. */
    esp_peer_audio_frame_t frame = {
        .data = s.enc_out,
        .size = (int)out.encoded_bytes,
        .pts = s.pts,
    };
    s.pts += samples;
    int r = esp_peer_send_audio(s.peer, &frame);
    if (r != ESP_PEER_ERR_NONE && r != ESP_PEER_ERR_WOULD_BLOCK) {
        return ESP_FAIL;
    }
    return ESP_OK;
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
    return s.speaking;
}

esp_err_t ring_net_send_event(const char *json)
{
    if (s.peer == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = s.send_stream_id,
        .data = (uint8_t *)json,
        .size = (int)strlen(json),
    };
    int r = esp_peer_send_data(s.peer, &frame);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_send_data(stream %u) failed: %d",
                 s.send_stream_id, r);
        return ESP_FAIL;
    }
    return ESP_OK;
}
