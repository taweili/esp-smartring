/*
 * ringkit — ring_chat STT stage: 48 kHz PCM → 16 kHz WAV → HTTPS POST.
 *
 * Split out of ring_chat.c. The WAV and base64 scratch buffers live in PSRAM
 * and are allocated lazily on first use. See ring_chat_priv.h.
 */
#include "ring_chat_priv.h"

#include "ringkit/ring_http.h"
#include "ringkit/ring_resample.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ring_chat"

static char *s_wav_buf = NULL;  /* PSRAM */
static char *s_b64_buf = NULL;  /* PSRAM */

esp_err_t ring_chat_stt(const int16_t *pcm48k, size_t samples,
                        char *out, size_t out_cap)
{
    if (!ring_chat_s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "stt: %u samples @ 48 kHz = %u ms",
             (unsigned)samples, (unsigned)(samples * 1000 / 48000));

    /* Resample 48k → 16k mono. */
    int n_float = (int)samples;
    float *in_f = malloc(n_float * sizeof(float));
    if (!in_f) return ESP_ERR_NO_MEM;
    ring_chat_pcm16_to_float(pcm48k, in_f, samples);

    int n16 = samples / 3;
    float *out_f = malloc(n16 * sizeof(float));
    if (!out_f) { free(in_f); return ESP_ERR_NO_MEM; }

    /* Reset decimator state for each turn (no cross-turn history needed). */
    ring_resample_decim_t decim;
    ring_resample_decim_init(&decim);
    int got = ring_resample_decim_process(&decim, in_f, n_float, out_f);
    free(in_f);

    /* Build WAV at 16 kHz. */
    int data_bytes = got * 2;  /* 16-bit */
    int wav_len = 44 + data_bytes;
    if (!s_wav_buf) {
        s_wav_buf = heap_caps_malloc(RING_CHAT_WAV_CAP, MALLOC_CAP_SPIRAM);
        if (!s_wav_buf) { free(out_f); return ESP_ERR_NO_MEM; }
    }
    if (wav_len > RING_CHAT_WAV_CAP) { free(out_f); return ESP_ERR_INVALID_SIZE; }

    ring_chat_wav_header((uint8_t *)s_wav_buf, 16000, data_bytes);
    ring_chat_float_to_pcm16(out_f, (int16_t *)(s_wav_buf + 44), (size_t)got);
    free(out_f);

    /* Base64 the WAV. */
    if (!s_b64_buf) {
        s_b64_buf = heap_caps_malloc(RING_CHAT_B64_CAP, MALLOC_CAP_SPIRAM);
        if (!s_b64_buf) return ESP_ERR_NO_MEM;
    }
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)s_b64_buf, RING_CHAT_B64_CAP - 128,
                              &b64_len, (const unsigned char *)s_wav_buf,
                              (size_t)wav_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Build the Data URI prefix + JSON body. The base64 payload is the bulk
     * of the body, so size the buffer for it rather than a fixed JSON_CAP. */
    size_t body_cap = b64_len + 256;
    char *body = malloc(body_cap);
    if (!body) return ESP_ERR_NO_MEM;
    int body_len;
    if (ring_chat_s.cfg.stt_language != NULL) {
        body_len = snprintf(body, body_cap,
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
            "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
            "\"data:audio/wav;base64,%s\"}}]}],\"stream\":false,"
            "\"asr_options\":{\"language\":\"%s\"}}",
            ring_chat_s.cfg.stt_model, s_b64_buf, ring_chat_s.cfg.stt_language);
    } else {
        body_len = snprintf(body, body_cap,
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
            "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
            "\"data:audio/wav;base64,%s\"}}]}],\"stream\":false}",
            ring_chat_s.cfg.stt_model, s_b64_buf);
    }
    if (body_len <= 0 || (size_t)body_len >= body_cap) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    /* POST. */
    esp_http_client_handle_t c = ring_http_init(RING_CHAT_STT_URL, RING_CHAT_CA_PEM,
                                                RING_CHAT_CA_LEN, RING_CHAT_STT_TIMEOUT_MS,
                                                4096, 1024);
    if (!c) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    ring_http_set_bearer(c, ring_chat_s.api_key);
    esp_http_client_set_post_field(c, body, body_len);

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            char *resp = malloc(RING_CHAT_JSON_CAP);
            if (resp) {
                int rlen = 0;
                if (ring_http_read_body(c, resp, RING_CHAT_JSON_CAP, &rlen) == ESP_OK) {
                    if (!ring_chat_json_get_str(resp, "content", out, out_cap)) {
                        ESP_LOGW(TAG, "stt: could not extract content from: %.200s", resp);
                        err = ESP_ERR_INVALID_RESPONSE;
                    } else {
                        ESP_LOGI(TAG, "stt: \"%s\"", out);
                    }
                }
                free(resp);
            }
        } else {
            ESP_LOGW(TAG, "stt: HTTP %d", status);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "stt: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    free(body);
    return err;
}
