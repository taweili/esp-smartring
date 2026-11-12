/*
 * ringkit — ring_geodis signaling: ephemeral token + SDP exchange over HTTPS,
 * and peer bring-up.
 *
 * Split out of ring_geodis.c. The HTTPS round trips run on their own task so a
 * slow POST never stalls RTP; the peer main loop runs on another task. See
 * ring_geodis_priv.h for the shared state and ring_geodis.h for the public API.
 */
#include "ring_geodis_priv.h"

#include "ringkit/ring_time.h"
#include "ringkit/ring_http.h"
#include "ringkit/ring_json.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ring_geodis"

/* ------------------------------------------------------------- HTTPS */

/* Low-level POST with retries.
 *
 * The GEODIS gateway intermittently resets TLS connections (~1 in 12 from the
 * bench), so transport failures are retried. A 4xx is a real rejection and is
 * never retried; a 5xx is retried. Returns ESP_OK only when the status equals
 * @p expect_status.
 *
 * The response body is read for both success and failure so the caller can log
 * it; @p resp may be NULL when the body is not needed.
 */
static esp_err_t http_post_raw(const char *url, const char *auth,
                               const char *content_type, const char *body,
                               int body_len, int expect_status,
                               char *resp, size_t resp_cap, int *resp_len,
                               int *status, int retries)
{
    if (resp != NULL && resp_cap > 0) {
        resp[0] = '\0';
    }
    if (resp_len != NULL) {
        *resp_len = 0;
    }
    if (status != NULL) {
        *status = 0;
    }

    int attempts = retries > 0 ? retries : 1;
    for (int i = 0; i < attempts; i++) {
        /* Both the GEODIS gateway and the Azure SDP host use public
         * certificates, so the Mozilla bundle (esp_crt_bundle_attach) is the
         * right trust anchor — no pinned PEM is needed. */
        esp_http_client_handle_t c = ring_http_init_bundle(
            url, RING_GEODIS_HTTP_TIMEOUT_MS, 2048, 1024);
        if (c == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", content_type);
        if (auth != NULL && auth[0] != '\0') {
            char bearer[160];
            snprintf(bearer, sizeof(bearer), "Bearer %s", auth);
            esp_http_client_set_header(c, "Authorization", bearer);
        }

        esp_err_t err = esp_http_client_open(c, body_len);
        if (err == ESP_OK && body_len > 0) {
            if (esp_http_client_write(c, body, body_len) < 0) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(c);
            int st = esp_http_client_get_status_code(c);
            if (status != NULL) {
                *status = st;
            }
            if (st == expect_status) {
                int n = 0;
                if (resp != NULL && resp_cap > 0) {
                    ring_http_read_body(c, resp, resp_cap, &n);
                }
                if (resp_len != NULL) {
                    *resp_len = n;
                }
                esp_http_client_cleanup(c);
                return ESP_OK;
            }
            char errbuf[256];
            int n = 0;
            ring_http_read_body(c, errbuf, sizeof(errbuf), &n);
            ESP_LOGW(TAG, "POST %s -> HTTP %d (want %d): %.120s",
                     url, st, expect_status, errbuf);
            esp_http_client_cleanup(c);
            if (st >= 400 && st < 500) {
                return ESP_FAIL;
            }
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "POST %s transport: %s", url, esp_err_to_name(err));
            esp_http_client_cleanup(c);
        }

        if (i + 1 < attempts) {
            vTaskDelay(pdMS_TO_TICKS(200 * (i + 1)));
        }
    }
    return ESP_FAIL;
}

esp_err_t ring_geodis_post_json(const char *url, const char *body,
                                int expect_status, char *resp, size_t resp_cap,
                                int *resp_len, int *status)
{
    return http_post_raw(url, NULL, "application/json", body,
                         (int)strlen(body), expect_status, resp, resp_cap,
                         resp_len, status, 3);
}

esp_err_t ring_geodis_post_sdp(const char *url, const char *token,
                               const char *offer, char *answer, size_t answer_cap)
{
    return http_post_raw(url, token, "application/sdp", offer,
                         (int)strlen(offer), 201, answer, answer_cap, NULL,
                         NULL, 3);
}

/* ------------------------------------------------------------- token */

esp_err_t ring_geodis_fetch_token(void)
{
    char body[768];
    int pos = 0;
    ring_json_append(body, sizeof(body), &pos, "{\"chat_session_id\":");
    ring_json_append_string(body, sizeof(body), &pos, ring_geodis_s.chat_session_id);
    if (ring_geodis_s.system_prompt[0] != '\0') {
        ring_json_append(body, sizeof(body), &pos, ",\"system_prompt\":");
        ring_json_append_string(body, sizeof(body), &pos, ring_geodis_s.system_prompt);
    }
    ring_json_append(body, sizeof(body), &pos, "}");

    char url[256];
    snprintf(url, sizeof(url), "%s/token", ring_geodis_s.base_url);

    char resp[512];
    int resp_len = 0;
    int status = 0;
    esp_err_t err = ring_geodis_post_json(url, body, 200, resp, sizeof(resp),
                                          &resp_len, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "token: HTTP %d", status);
        return err;
    }

    char token[RING_GEODIS_TOKEN_MAX];
    char session_id[RING_GEODIS_SESSION_ID_MAX];
    if (!ring_json_get_str(resp, "token", token, sizeof(token), NULL) ||
        !ring_json_get_str(resp, "session_id", session_id, sizeof(session_id), NULL)) {
        ESP_LOGE(TAG, "token: parse failed: %.120s", resp);
        return ESP_FAIL;
    }
    snprintf(ring_geodis_s.token, sizeof(ring_geodis_s.token), "%s", token);
    snprintf(ring_geodis_s.session_id, sizeof(ring_geodis_s.session_id), "%s", session_id);
    ESP_LOGI(TAG, "token: session %s", session_id);
    return ESP_OK;
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

/**
 * POST the Offer to the Azure OpenAI Realtime SDP exchange and return the
 * Answer. All buffers are on the heap so this can run on a small task stack.
 */
static esp_err_t sdp_exchange(const char *offer, char *answer, size_t answer_len)
{
    char *offer_crlf = malloc(RING_GEODIS_SDP_MAX_BYTES);
    if (offer_crlf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    normalise_sdp_crlf(offer, offer_crlf, RING_GEODIS_SDP_MAX_BYTES);

    esp_err_t err = ring_geodis_post_sdp(ring_geodis_s.sdp_url, ring_geodis_s.token,
                                         offer_crlf, answer, answer_len);
    free(offer_crlf);
    return err;
}

/* ------------------------------------------------------------- tasks */

void ring_geodis_peer_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    ESP_LOGI(TAG, "peer task started (hwm %u words)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        if (ring_geodis_s.peer) {
            esp_peer_main_loop(ring_geodis_s.peer);
        }
        if ((ticks++ % 1000) == 0) {
            ESP_LOGI(TAG, "peer task alive (hwm %u words)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ring_geodis_signal_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char *offer = malloc(RING_GEODIS_SDP_MAX_BYTES);
        char *answer = malloc(RING_GEODIS_SDP_MAX_BYTES);
        if (offer == NULL || answer == NULL) {
            free(offer);
            free(answer);
            continue;
        }

        xSemaphoreTake(ring_geodis_s.sdp_mutex, portMAX_DELAY);
        strncpy(offer, ring_geodis_s.local_sdp, RING_GEODIS_SDP_MAX_BYTES - 1);
        offer[RING_GEODIS_SDP_MAX_BYTES - 1] = '\0';
        xSemaphoreGive(ring_geodis_s.sdp_mutex);

        if (offer[0] != '\0' &&
            sdp_exchange(offer, answer, RING_GEODIS_SDP_MAX_BYTES) == ESP_OK) {
            esp_peer_msg_t m = {
                .type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)answer,
                .size = (int)strlen(answer),
            };
            int r = esp_peer_send_msg(ring_geodis_s.peer, &m);
            if (r != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "esp_peer_send_msg failed: %d", r);
                ring_geodis_state_set(RING_GEODIS_ERROR);
            } else {
                ESP_LOGI(TAG, "remote SDP installed (%u bytes)",
                         (unsigned)strlen(answer));
                ESP_LOGD(TAG, "ANSWER SDP:\n%s", answer);
            }
        } else {
            ring_geodis_state_set(RING_GEODIS_ERROR);
        }

        free(offer);
        free(answer);
    }
}

/* ------------------------------------------------------------- session */

esp_err_t ring_geodis_peer_open(void)
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
            .sample_rate = RING_GEODIS_SAMPLE_RATE,
            .channel = RING_GEODIS_OPUS_CHANNELS,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .no_auto_reconnect = true,
        .extra_cfg = &dcfg,
        .extra_size = sizeof(dcfg),
        .on_state = ring_geodis_on_state,
        .on_msg = ring_geodis_on_msg,
        .on_audio_info = ring_geodis_on_audio_info,
        .on_audio_data = ring_geodis_on_audio_data,
        .on_channel_open = ring_geodis_on_channel_open,
        .on_data = ring_geodis_on_data_frame,
        .ctx = NULL,
    };

    int r = esp_peer_open(&cfg, esp_peer_get_default_impl(), &ring_geodis_s.peer);
    if (r != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", r);
        return ESP_FAIL;
    }
    if (ring_geodis_s.peer_task == NULL) {
        xTaskCreate(ring_geodis_peer_task, "ring-geodis-peer",
                    RING_GEODIS_PEER_TASK_STACK, NULL,
                    RING_GEODIS_PEER_TASK_PRIO, &ring_geodis_s.peer_task);
    }
    return ESP_OK;
}
