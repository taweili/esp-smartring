/*
 * ringkit — ring_chat TTS stage: HTTPS POST → SSE stream → base64 PCM chunks.
 *
 * Split out of ring_chat.c. All scratch is on the heap: this runs on the
 * caller's task, which may have a small stack. See ring_chat_priv.h.
 */
#include "ring_chat_priv.h"

#include "ringkit/ring_http.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ring_chat"

/* SSE TTS: line-buffered parser, decodes base64 PCM to int16_t,
 * then calls the app's audio callback. */
typedef struct {
    ring_chat_audio_cb_t cb;
    void               *user;
    int16_t            *pcm;       /* PSRAM decode buffer */
    int                 pcm_cap;   /* samples */
    char               *b64;       /* base64 scratch (heap) */
    size_t              b64_cap;
    int                 sentences; /* diagnostic counter */
    int                 chunks;    /* per-sentence chunk counter */
} tts_ctx_t;

static bool tts_handle_event(tts_ctx_t *ctx, const char *json)
{
    /* Extract type. */
    char type[64];
    if (!ring_chat_json_get_str(json, "type", type, sizeof(type))) return false;

    if (strcmp(type, "sentence-begin") == 0) {
        ctx->sentences++;
        ctx->chunks = 0;
        return true;
    }

    if (strcmp(type, "sentence-synthesis") == 0) {
        ctx->chunks++;
        /* Extract the base64 audio data. */
        if (!ring_chat_json_get_str(json, "data", ctx->b64, ctx->b64_cap)) {
            return false;
        }

        size_t b64_len = strlen(ctx->b64);
        size_t pcm_bytes = (b64_len / 4) * 3;  /* upper bound */
        if (pcm_bytes / 2 > (size_t)ctx->pcm_cap) {
            ESP_LOGW(TAG, "tts: pcm chunk too big (%u bytes)", (unsigned)pcm_bytes);
            return false;
        }

        size_t decoded = 0;
        if (mbedtls_base64_decode((unsigned char *)ctx->pcm, pcm_bytes,
                                  &decoded,
                                  (const unsigned char *)ctx->b64,
                                  b64_len) != 0) {
            ESP_LOGW(TAG, "tts: base64 decode failed");
            return false;
        }

        size_t samples = decoded / 2;
        if (samples > 0) {
            ctx->cb(ctx->pcm, samples, ctx->user);
        }
        return true;
    }

    /* sentence-end — no audio, just the finish reason. We can log it. */
    if (strcmp(type, "sentence-end") == 0) {
        return true;
    }

    /* Ignore the trailing URL-only summary event. */
    return true;
}

esp_err_t ring_chat_tts(const char *text, ring_chat_audio_cb_t cb, void *user)
{
    if (!ring_chat_s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "tts: %u chars", (unsigned)strlen(text));

    /* Build the TTS request body. Heap, not stack: the caller's task may be
     * as small as 16 KB and the body can be several KB. */
    size_t text_len = strlen(text);
    size_t body_cap = text_len * 2 + 256;
    char *escaped = malloc(text_len * 2 + 1);
    char *body = malloc(body_cap);
    if (!escaped || !body) {
        free(escaped);
        free(body);
        return ESP_ERR_NO_MEM;
    }
    int esc_pos = 0;
    for (size_t i = 0; i < text_len; i++) {
        char ch = text[i];
        if (ch == '"' || ch == '\\') {
            escaped[esc_pos++] = '\\';
        }
        escaped[esc_pos++] = ch;
    }
    escaped[esc_pos] = '\0';

    int body_len = snprintf(body, body_cap,
        "{\"model\":\"%s\",\"input\":{"
        "\"text\":\"%s\",\"voice\":\"%s\",\"format\":\"pcm\",\"sample_rate\":48000"
        "}}",
        ring_chat_s.cfg.tts_model, escaped, ring_chat_s.cfg.voice);
    free(escaped);

    if (body_len <= 0 || (size_t)body_len >= body_cap) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Open HTTP client. */
    esp_http_client_handle_t c = ring_http_init(RING_CHAT_TTS_URL, RING_CHAT_CA_PEM,
                                               RING_CHAT_CA_LEN, RING_CHAT_TTS_TIMEOUT_MS,
                                               4096, 1024);
    if (!c) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "X-DashScope-SSE", "enable");
    ring_http_set_bearer(c, ring_chat_s.api_key);
    esp_http_client_set_post_field(c, body, body_len);

    /* Streaming post — open, write, then read the SSE stream. */
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK) {
        int written = esp_http_client_write(c, body, body_len);
        if (written < 0) {
            free(body);
            esp_http_client_cleanup(c);
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "tts: HTTP %d", status);
            free(body);
            esp_http_client_cleanup(c);
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* SSE read loop. All scratch on the heap: this runs on the caller's
         * task, which may have a small stack. */
        tts_ctx_t ctx;
        ctx.cb = cb;
        ctx.user = user;
        ctx.sentences = 0;
        ctx.chunks = 0;
        ctx.pcm_cap = RING_CHAT_TTS_PCM_SAMPLES;
        ctx.pcm = heap_caps_malloc(ctx.pcm_cap * 2, MALLOC_CAP_SPIRAM);
        ctx.b64_cap = RING_CHAT_TTS_B64_CAP;
        ctx.b64 = heap_caps_malloc(ctx.b64_cap, MALLOC_CAP_SPIRAM);

        ring_http_sse_t sse;
        bool sse_ok = ring_http_sse_init(&sse, c);
        if (!ctx.pcm || !ctx.b64 || !sse_ok) {
            free(ctx.pcm);
            free(ctx.b64);
            ring_http_sse_deinit(&sse);
            free(body);
            esp_http_client_cleanup(c);
            return ESP_ERR_NO_MEM;
        }

        bool done = false;
        while (!done) {
            bool truncated = false;
            const char *line = ring_http_sse_next(&sse, &truncated);
            if (line == NULL) {
                if (truncated) {
                    ESP_LOGW(TAG, "tts sse: line too long — aborted");
                }
                break;
            }

            /* Skip empty lines, :keepalive, id:, event: markers. */
            if (line[0] == '\0' || line[0] == ':' ||
                line[0] == 'i' || line[0] == 'e') {
                continue;
            }
            if (strncmp(line, "data:", 5) == 0) {
                const char *json = line + 5;
                while (*json == ' ') json++;
                if (json[0] == '\0') continue;

                if (!tts_handle_event(&ctx, json)) {
                    /* Skip malformed/unknown events; one bad event must not
                     * truncate the audio stream. */
                    ESP_LOGW(TAG, "tts sse: event parse failed: %.200s", json);
                    continue;
                }

                /* Check for finish_reason (sentence-end marks stop). */
                char fr[16];
                if (ring_chat_json_get_str(json, "finish_reason", fr, sizeof(fr))) {
                    if (strcmp(fr, "stop") == 0) {
                        done = true;
                    }
                }
            }
        }

        ESP_LOGI(TAG, "tts: %d sentences, %d chunks total%s",
                 ctx.sentences, ctx.chunks, sse.eof ? " (eof)" : "");
        ring_http_sse_deinit(&sse);
        free(ctx.pcm);
        free(ctx.b64);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "tts: open failed: %s", esp_err_to_name(err));
    }

    free(body);
    esp_http_client_cleanup(c);
    return err;
}
