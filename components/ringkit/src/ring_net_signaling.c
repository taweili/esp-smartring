/*
 * ringkit — ring_net signaling: SDP exchange over HTTPS and peer bring-up.
 *
 * Split out of ring_net.c. The HTTPS round trip runs on its own task so a
 * slow SDP POST never stalls RTP; the peer main loop runs on another task.
 * See ring_net_priv.h for the shared state and ring_net.h for the public API.
 */
#include "ring_net_priv.h"

#include "ringkit/ring_ca.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_http.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ring_net"

/* ------------------------------------------------------------- network gate */

esp_err_t ring_net_sync_clock(void)
{
    return ring_time_sync(RING_NET_SNTP_TIMEOUT_MS);
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
        .timeout_ms = RING_NET_HTTP_TIMEOUT_MS,
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
             RING_NET_HOST, ring_net_s.model);

    char *offer_crlf = malloc(RING_NET_SDP_MAX_BYTES);
    if (offer_crlf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    normalise_sdp_crlf(offer, offer_crlf, RING_NET_SDP_MAX_BYTES);

    /* The bundled cert chain for this endpoint (assets/qianwen_ca.pem):
     * the server does not send its intermediate CA. */
    esp_http_client_handle_t c = ring_http_init(url, ring_ca_pem(),
                                                ring_ca_pem_len(),
                                                RING_NET_HTTP_TIMEOUT_MS,
                                                RING_NET_SDP_MAX_BYTES, 1024);
    if (c == NULL) {
        free(offer_crlf);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/sdp");
    ring_http_set_bearer(c, ring_net_s.api_key);

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

/* ------------------------------------------------------------- tasks */

void ring_net_peer_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    ESP_LOGI(TAG, "peer task started (hwm %u words)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        if (ring_net_s.peer) {
            esp_peer_main_loop(ring_net_s.peer);
        }
        if ((ticks++ % 1000) == 0) {
            ESP_LOGI(TAG, "peer task alive (hwm %u words)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ring_net_signal_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char *offer = malloc(RING_NET_SDP_MAX_BYTES);
        char *answer = malloc(RING_NET_SDP_MAX_BYTES);
        if (offer == NULL || answer == NULL) {
            free(offer);
            free(answer);
            continue;
        }

        xSemaphoreTake(ring_net_s.sdp_mutex, portMAX_DELAY);
        strncpy(offer, ring_net_s.local_sdp, RING_NET_SDP_MAX_BYTES - 1);
        offer[RING_NET_SDP_MAX_BYTES - 1] = '\0';
        xSemaphoreGive(ring_net_s.sdp_mutex);

        if (offer[0] != '\0' &&
            sdp_exchange(offer, answer, RING_NET_SDP_MAX_BYTES) == ESP_OK) {
            esp_peer_msg_t m = {
                .type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)answer,
                .size = (int)strlen(answer),
            };
            int r = esp_peer_send_msg(ring_net_s.peer, &m);
            if (r != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "esp_peer_send_msg failed: %d", r);
                ring_net_state_set(RING_NET_ERROR);
            } else {
                ESP_LOGI(TAG, "remote SDP installed (%u bytes)",
                         (unsigned)strlen(answer));
                ESP_LOGD(TAG, "ANSWER SDP:\n%s", answer);
            }
        } else {
            ring_net_state_set(RING_NET_ERROR);
        }

        free(offer);
        free(answer);
    }
}

/* ------------------------------------------------------------- session */

esp_err_t ring_net_peer_open(void)
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
        .on_state = ring_net_on_state,
        .on_msg = ring_net_on_msg,
        .on_audio_info = ring_net_on_audio_info,
        .on_audio_data = ring_net_on_audio_data,
        .on_channel_open = ring_net_on_channel_open,
        .on_data = ring_net_on_data_frame,
        .ctx = NULL,
    };

    int r = esp_peer_open(&cfg, esp_peer_get_default_impl(), &ring_net_s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", r);
        return ESP_FAIL;
    }
    if (ring_net_s.peer_task == NULL) {
        xTaskCreate(ring_net_peer_task, "ring-net-peer", RING_NET_PEER_TASK_STACK,
                    NULL, RING_NET_PEER_TASK_PRIO, &ring_net_s.peer_task);
    }
    return ESP_OK;
}
