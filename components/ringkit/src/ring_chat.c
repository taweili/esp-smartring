/*
 * ringkit — HTTPS REST cascade: STT → LLM → TTS.
 *
 * Three sequential HTTPS POSTs to maas.qianwenaiapi.com. All blocking;
 * call from the app's worker task. The TTS SSE audio callback runs on
 * that same task and may take the LVGL lock briefly.
 */
#include "ringkit/ring_chat.h"
#include "ringkit/ring_resample.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ring_chat"

/* ------------------------------------------------------------- config */

#define HOST        "maas.qianwenaiapi.com"
#define STT_URL     "https://" HOST "/compatible-mode/v1/chat/completions"
#define LLM_URL     STT_URL
#define TTS_URL     "https://" HOST "/api/v1/services/audio/tts/SpeechSynthesizer"

/* The pinned CA (the server sends only its leaf cert). */
extern const uint8_t qianwen_ca_pem_start[] asm("_binary_qianwen_ca_pem_start");
extern const uint8_t qianwen_ca_pem_end[]   asm("_binary_qianwen_ca_pem_end");

#define MAX_HISTORY_TURNS  20     /* compile-time cap for safety */
#define WAV_CAP            (512 * 1024)   /* ~16 s @ 16 kHz mono: 512 KB */
#define B64_CAP            ((WAV_CAP + 2) / 3 * 4 + 256) /* base64 + Data URI prefix */
#define SSE_LINE_CAP       8192
#define LLM_REPLY_CAP      8192
#define JSON_CAP           8192

#define STT_TIMEOUT_MS     15000
#define LLM_TIMEOUT_MS     20000
#define TTS_TIMEOUT_MS     40000

/* ------------------------------------------------------------- state */

typedef struct ring_chat_history_t {
    char **user_msgs;
    char **assistant_msgs;
    int    count;
    int    cap;
} ring_chat_history_t;

static struct {
    ring_chat_cfg_t    cfg;
    char               api_key[160];
    ring_chat_history_t hist;
    bool               init;
} s;

/* ------------------------------------------------------------- JSON helpers
 *
 * Minimalist strstr-based extraction — mirrors ring_net.c's approach.
 * We extract these into ring_json.[ch] separately (standalone refactor).
 */

static bool json_get_str(const char *json, const char *key,
                         char *out, size_t cap)
{
    if (json == NULL || key == NULL || out == NULL || cap == 0) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    size_t len = 0;
    while (*p && *p != '"' && len < cap - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[len++] = *p++;
    }
    out[len] = '\0';
    return true;
}

/* Read until \n (or buffer full), appending to dst at *pos.
 * Skips CR. Returns false only on overflow. */
static bool sse_read_line(esp_http_client_handle_t c,
                          char *dst, int *pos, int cap)
{
    char ch;
    while (*pos < cap - 1) {
        int r = esp_http_client_read(c, &ch, 1);
        if (r <= 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        dst[(*pos)++] = ch;
    }
    dst[*pos] = '\0';
    return *pos < cap - 1;
}

/* ------------------------------------------------------------- audio helpers */

static void pcm16_to_float(const int16_t *in, float *out, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (float)in[i];
}

static void float_to_pcm16(const float *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (v < -32768.0f) v = -32768.0f;
        if (v > 32767.0f) v = 32767.0f;
        out[i] = (int16_t)v;
    }
}

/* Write a canonical 44-byte WAV header into dst (PCM 16-bit mono). */
static void wav_header(uint8_t *dst, int sample_rate, int data_bytes)
{
    int bits = 16, channels = 1;
    int byte_rate = sample_rate * channels * bits / 8;
    int block_align = channels * bits / 8;
    int chunk_size = 36 + data_bytes;
    /* RIFF */
    dst[0] = 'R'; dst[1] = 'I'; dst[2] = 'F'; dst[3] = 'F';
    dst[4] = (uint8_t)(chunk_size & 0xff);
    dst[5] = (uint8_t)((chunk_size >> 8) & 0xff);
    dst[6] = (uint8_t)((chunk_size >> 16) & 0xff);
    dst[7] = (uint8_t)((chunk_size >> 24) & 0xff);
    dst[8] = 'W'; dst[9] = 'A'; dst[10] = 'V'; dst[11] = 'E';
    /* fmt  */
    dst[12] = 'f'; dst[13] = 'm'; dst[14] = 't'; dst[15] = ' ';
    dst[16] = 16; dst[17] = 0; dst[18] = 0; dst[19] = 0; /* chunk size */
    dst[20] = 1; dst[21] = 0;  /* PCM */
    dst[22] = (uint8_t)channels; dst[23] = 0;
    dst[24] = (uint8_t)(sample_rate & 0xff);
    dst[25] = (uint8_t)((sample_rate >> 8) & 0xff);
    dst[26] = (uint8_t)((sample_rate >> 16) & 0xff);
    dst[27] = (uint8_t)((sample_rate >> 24) & 0xff);
    dst[28] = (uint8_t)(byte_rate & 0xff);
    dst[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    dst[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    dst[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    dst[32] = (uint8_t)block_align; dst[33] = 0;
    dst[34] = bits; dst[35] = 0;
    /* data */
    dst[36] = 'd'; dst[37] = 'a'; dst[38] = 't'; dst[39] = 'a';
    dst[40] = (uint8_t)(data_bytes & 0xff);
    dst[41] = (uint8_t)((data_bytes >> 8) & 0xff);
    dst[42] = (uint8_t)((data_bytes >> 16) & 0xff);
    dst[43] = (uint8_t)((data_bytes >> 24) & 0xff);
}

/* ------------------------------------------------------------- HTTP helpers */

static esp_http_client_handle_t http_init(void *user)
{
    (void)user;
    esp_http_client_config_t http = {
        .cert_pem = (const char *)qianwen_ca_pem_start,
        .cert_len = (size_t)(qianwen_ca_pem_end - qianwen_ca_pem_start),
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .timeout_ms = LLM_TIMEOUT_MS,
        .skip_cert_common_name_check = true,
    };
    return esp_http_client_init(&http);
}

static void http_set_common_headers(esp_http_client_handle_t c)
{
    char auth[200];
    snprintf(auth, sizeof(auth), "Bearer %s", s.api_key);
    esp_http_client_set_header(c, "Authorization", auth);
}

/* Read the full HTTP response body (chunked-safe). Returns ESP_OK on success. */
static esp_err_t http_read_body(esp_http_client_handle_t c,
                                 char *buf, size_t cap, int *out_len)
{
    int total = 0;
    while (total < (int)cap - 1) {
        int r = esp_http_client_read(c, buf + total, (int)cap - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return total > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ------------------------------------------------------------- STT */

static char *s_wav_buf = NULL;  /* PSRAM */
static char *s_b64_buf = NULL;  /* PSRAM */

esp_err_t ring_chat_stt(const int16_t *pcm48k, size_t samples,
                        char *out, size_t out_cap)
{
    if (!s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "stt: %u samples @ 48 kHz = %u ms",
             (unsigned)samples, (unsigned)(samples * 1000 / 48000));

    /* Resample 48k → 16k mono. */
    int n_float = (int)samples;
    float *in_f = malloc(n_float * sizeof(float));
    if (!in_f) return ESP_ERR_NO_MEM;
    pcm16_to_float(pcm48k, in_f, samples);

    int n16 = samples / 3;
    float *out_f = malloc(n16 * sizeof(float));
    if (!out_f) { free(in_f); return ESP_ERR_NO_MEM; }

    /* Reset decimator state for each turn (no cross-turn history needed). */
    ring_resample_decim_state_t decim;
    ring_resample_decim_init(&decim);
    int got = ring_resample_decim_process(&decim, in_f, n_float, out_f);
    free(in_f);

    /* Build WAV at 16 kHz. */
    int data_bytes = got * 2;  /* 16-bit */
    int wav_len = 44 + data_bytes;
    if (!s_wav_buf) {
        s_wav_buf = heap_caps_malloc(WAV_CAP, MALLOC_CAP_SPIRAM);
        if (!s_wav_buf) { free(out_f); return ESP_ERR_NO_MEM; }
    }
    if (wav_len > WAV_CAP) { free(out_f); return ESP_ERR_INVALID_SIZE; }

    wav_header((uint8_t *)s_wav_buf, 16000, data_bytes);
    float_to_pcm16(out_f, (int16_t *)(s_wav_buf + 44), (size_t)got);
    free(out_f);

    /* Base64 the WAV. */
    if (!s_b64_buf) {
        s_b64_buf = heap_caps_malloc(B64_CAP, MALLOC_CAP_SPIRAM);
        if (!s_b64_buf) return ESP_ERR_NO_MEM;
    }
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)s_b64_buf, B64_CAP - 128,
                              &b64_len, (const unsigned char *)s_wav_buf,
                              (size_t)wav_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Build the Data URI prefix + JSON body (separate buffer to avoid overlap). */
    char *body = malloc(JSON_CAP);
    if (!body) return ESP_ERR_NO_MEM;
    int body_len = snprintf(body, JSON_CAP,
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
        "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
        "\"data:audio/wav;base64,%s\"}}]}],\"stream\":false}",
        s.cfg.stt_model, s_b64_buf);
    if (body_len <= 0 || (size_t)body_len >= JSON_CAP) return ESP_ERR_INVALID_SIZE;

    /* POST. */
    esp_http_client_handle_t c = http_init(NULL);
    if (!c) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_set_url(c, STT_URL);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    http_set_common_headers(c);
    esp_http_client_set_post_field(c, body, body_len);
    esp_http_client_set_timeout_ms(c, STT_TIMEOUT_MS);

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            char *resp = malloc(JSON_CAP);
            if (resp) {
                int rlen = 0;
                if (http_read_body(c, resp, JSON_CAP, &rlen) == ESP_OK) {
                    if (!json_get_str(resp, "content", out, out_cap)) {
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

/* ------------------------------------------------------------- LLM */

esp_err_t ring_chat_add_user(const char *text)
{
    if (s.hist.count >= s.hist.cap) return ESP_ERR_NO_MEM;
    s.hist.user_msgs[s.hist.count] = strdup(text);
    if (!s.hist.user_msgs[s.hist.count]) return ESP_ERR_NO_MEM;
    s.hist.count++;
    return ESP_OK;
}

esp_err_t ring_chat_add_assistant(const char *text)
{
    if (s.hist.count >= s.hist.cap) return ESP_ERR_NO_MEM;
    s.hist.assistant_msgs[s.hist.count] = strdup(text);
    if (!s.hist.assistant_msgs[s.hist.count]) return ESP_ERR_NO_MEM;
    s.hist.count++;
    return ESP_OK;
}

void ring_chat_reset_history(void)
{
    for (int i = 0; i < s.hist.count; i++) {
        free(s.hist.user_msgs[i]);    s.hist.user_msgs[i] = NULL;
        free(s.hist.assistant_msgs[i]); s.hist.assistant_msgs[i] = NULL;
    }
    s.hist.count = 0;
}

/* Build a messages JSON array from history, respecting the cap. */
static int build_messages(char *buf, size_t cap)
{
    int pos = 0;
    int cap_val = s.cfg.max_history_turns > 0 ? s.cfg.max_history_turns
                  : MAX_HISTORY_TURNS;
    int start = s.hist.count > cap_val * 2 ? s.hist.count - cap_val * 2 : 0;
    /* Ensure system prompt alignment — if cap implies removing the first turn,
     * remove pairs so user+assistant stays intact. */
    if (start > 0 && (s.hist.count - start) % 2 != 0 && start > 0) start--;

    pos += snprintf(buf + pos, cap - pos, "[");
    /* System prompt, if any. */
    if (s.cfg.system_prompt) {
        pos += snprintf(buf + pos, cap - pos,
                        "{\"role\":\"system\",\"content\":\"%s\"},",
                        s.cfg.system_prompt);
    }
    for (int i = start; i < s.hist.count; i++) {
        if (s.hist.user_msgs[i]) {
            /* Escape quotes in the text. */
            pos += snprintf(buf + pos, cap - pos,
                            "{\"role\":\"user\",\"content\":\"%s\"},",
                            s.hist.user_msgs[i]);
        }
        if (s.hist.assistant_msgs[i]) {
            pos += snprintf(buf + pos, cap - pos,
                            "{\"role\":\"assistant\",\"content\":\"%s\"},",
                            s.hist.assistant_msgs[i]);
        }
    }
    /* Replace trailing comma with ] */
    if (pos > 1 && buf[pos-1] == ',') pos--;
    pos += snprintf(buf + pos, cap - pos, "]");
    return pos;
}

esp_err_t ring_chat_llm_complete(char *reply, size_t cap)
{
    if (!s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "llm: %d history turns", s.hist.count);

    char *msgs = malloc(JSON_CAP);
    if (!msgs) return ESP_ERR_NO_MEM;
    build_messages(msgs, JSON_CAP);

    /* Trim trailing comma (build_messages already handles this, but be safe). */
    size_t msgs_len = strlen(msgs);

    char *body = malloc(msgs_len + 256);
    if (!body) { free(msgs); return ESP_ERR_NO_MEM; }
    int body_len = snprintf(body, msgs_len + 256,
        "{\"model\":\"%s\",\"messages\":%.*s,\"stream\":false,"
        "\"extra_body\":{\"enable_thinking\":false}}",
        s.cfg.llm_model, (int)msgs_len, msgs);
    free(msgs);

    esp_http_client_handle_t c = http_init(NULL);
    if (!c) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_set_url(c, LLM_URL);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    http_set_common_headers(c);
    esp_http_client_set_post_field(c, body, body_len);
    esp_http_client_set_timeout_ms(c, LLM_TIMEOUT_MS);

    esp_err_t err = esp_http_client_perform(c);
    free(body);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            char *resp = malloc(JSON_CAP);
            if (!resp) { esp_http_client_cleanup(c); return ESP_ERR_NO_MEM; }
            int rlen = 0;
            if (http_read_body(c, resp, JSON_CAP, &rlen) == ESP_OK) {
                if (!json_get_str(resp, "content", reply, cap)) {
                    ESP_LOGW(TAG, "llm: could not extract content from: %.200s", resp);
                    err = ESP_ERR_INVALID_RESPONSE;
                } else {
                    ESP_LOGI(TAG, "llm: \"%s\"", reply);
                    ring_chat_add_assistant(reply);
                }
            }
            free(resp);
        } else {
            ESP_LOGW(TAG, "llm: HTTP %d", status);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "llm: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    return err;
}

/* ------------------------------------------------------------- TTS (SSE) */

esp_err_t ring_chat_llm_stream(ring_chat_text_cb_t text_cb, void *user)
{
    /* Placeholder — P4 implements streaming LLM with sentence bridge. */
    (void)text_cb; (void)user;
    return ESP_ERR_NOT_SUPPORTED;
}

/* SSE TTS: line-buffered parser, decodes base64 PCM to int16_t,
 * then calls the app's audio callback. */
typedef struct {
    ring_chat_audio_cb_t cb;
    void               *user;
    int16_t            *pcm;       /* PSRAM decode buffer */
    int                  pcm_cap;  /* samples */
    int                  sentences;/* diagnostic counter */
    int                  chunks;   /* per-sentence chunk counter */
} tts_ctx_t;

static bool tts_handle_event(tts_ctx_t *ctx, const char *json)
{
    /* Extract type. */
    char type[64];
    if (!json_get_str(json, "type", type, sizeof(type))) return false;

    if (strcmp(type, "sentence-begin") == 0) {
        ctx->sentences++;
        ctx->chunks = 0;
        return true;
    }

    if (strcmp(type, "sentence-synthesis") == 0) {
        ctx->chunks++;
        /* Extract the base64 audio data. */
        char data_b64[SSE_LINE_CAP];
        if (!json_get_str(json, "data", data_b64, sizeof(data_b64))) return false;

        size_t b64_len = strlen(data_b64);
        size_t pcm_bytes = (b64_len / 4) * 3;  /* upper bound */
        if (pcm_bytes / 2 > (size_t)ctx->pcm_cap) {
            ESP_LOGW(TAG, "tts: pcm chunk too big (%u bytes)", (unsigned)pcm_bytes);
            return false;
        }

        size_t decoded = 0;
        if (mbedtls_base64_decode((unsigned char *)ctx->pcm, pcm_bytes,
                                  &decoded,
                                  (const unsigned char *)data_b64,
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
    if (!s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "tts: %u chars", (unsigned)strlen(text));

    /* Build the TTS request body. */
    size_t text_len = strlen(text);
    /* Need to escape the text for JSON. */
    char *escaped = malloc(text_len * 2 + 1);
    if (!escaped) return ESP_ERR_NO_MEM;
    int esc_pos = 0;
    for (size_t i = 0; i < text_len; i++) {
        char ch = text[i];
        if (ch == '"' || ch == '\\') {
            escaped[esc_pos++] = '\\';
        }
        escaped[esc_pos++] = ch;
    }
    escaped[esc_pos] = '\0';

    char body[JSON_CAP];
    int body_len = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"input\":{"
        "\"text\":\"%s\",\"voice\":\"%s\",\"format\":\"pcm\",\"sample_rate\":48000"
        "}}",
        s.cfg.tts_model, escaped, s.cfg.voice);
    free(escaped);

    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return ESP_ERR_INVALID_SIZE;

    /* Open HTTP client. */
    esp_http_client_handle_t c = http_init(NULL);
    if (!c) return ESP_ERR_NO_MEM;

    esp_http_client_set_url(c, TTS_URL);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "X-DashScope-SSE", "enable");
    http_set_common_headers(c);
    esp_http_client_set_post_field(c, body, body_len);
    esp_http_client_set_timeout_ms(c, TTS_TIMEOUT_MS);

    /* Streaming post — open, write, then read the SSE stream. */
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK) {
        int written = esp_http_client_write(c, body, body_len);
        if (written < 0) {
            esp_http_client_cleanup(c);
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "tts: HTTP %d", status);
            esp_http_client_cleanup(c);
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* SSE read loop. */
        tts_ctx_t ctx;
        ctx.cb = cb;
        ctx.user = user;
        ctx.sentences = 0;
        ctx.chunks = 0;
        ctx.pcm_cap = 8192;  /* 8192 samples = 170 ms @ 48 kHz, per chunk */
        ctx.pcm = heap_caps_malloc(ctx.pcm_cap * 2, MALLOC_CAP_SPIRAM);
        if (!ctx.pcm) {
            esp_http_client_cleanup(c);
            return ESP_ERR_NO_MEM;
        }

        char line[4096];
        int line_pos = 0;
        bool done = false;

        while (!done) {
            line_pos = 0;
            if (!sse_read_line(c, line, &line_pos, sizeof(line))) {
                /* Buffer full — log and continue */
                ESP_LOGW(TAG, "tts sse: long line truncated");
                line[sizeof(line) - 1] = '\0';
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
                    ESP_LOGW(TAG, "tts sse: event parse failed");
                    break;
                }

                /* Check for finish_reason (sentence-end marks stop). */
                char fr[16];
                if (json_get_str(json, "finish_reason", fr, sizeof(fr))) {
                    if (strcmp(fr, "stop") == 0) {
                        done = true;
                    }
                }
            }
        }

        ESP_LOGI(TAG, "tts: %d sentences, %d chunks total",
                 ctx.sentences, ctx.chunks);
        free(ctx.pcm);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "tts: open failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    return err;
}

/* ------------------------------------------------------------- init */

esp_err_t ring_chat_init(const ring_chat_cfg_t *cfg, const char *api_key)
{
    if (cfg == NULL || api_key == NULL) return ESP_ERR_INVALID_ARG;
    memset(&s, 0, sizeof(s));

    s.cfg = *cfg;
    snprintf(s.api_key, sizeof(s.api_key), "%s", api_key);

    /* Allocate history arrays (worst-case: MAX_HISTORY_TURNS pairs). */
    int max_pairs = cfg->max_history_turns > 0 ? cfg->max_history_turns
                   : MAX_HISTORY_TURNS;
    s.hist.cap = max_pairs;
    s.hist.user_msgs = calloc(max_pairs, sizeof(char *));
    s.hist.assistant_msgs = calloc(max_pairs, sizeof(char *));
    if (!s.hist.user_msgs || !s.hist.assistant_msgs) {
        free(s.hist.user_msgs);
        free(s.hist.assistant_msgs);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate WAV and base64 buffers in PSRAM. */
    s_wav_buf = heap_caps_malloc(WAV_CAP, MALLOC_CAP_SPIRAM);
    s_b64_buf = heap_caps_malloc(B64_CAP, MALLOC_CAP_SPIRAM);
    if (!s_wav_buf || !s_b64_buf) {
        ESP_LOGW(TAG, "psram buffers: %s",
                 !s_wav_buf ? "wav" : "b64");
        /* Non-fatal — ring_chat_stt will return ESP_ERR_NO_MEM on first use. */
    }

    s.init = true;
    ESP_LOGI(TAG, "initialised (model %s, llm %s, tts %s, voice %s)",
             cfg->stt_model, cfg->llm_model, cfg->tts_model, cfg->voice);
    return ESP_OK;
}