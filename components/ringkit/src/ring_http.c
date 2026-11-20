/*
 * ringkit — shared HTTPS helpers. See ring_http.h.
 */
#include "ringkit/ring_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"

#define TAG "ring_http"

esp_http_client_handle_t ring_http_init(const char *url, const char *ca_pem,
                                        size_t ca_len, int timeout_ms,
                                        size_t buffer_size, size_t tx_size)
{
    if (url == NULL || ca_pem == NULL || ca_len == 0) {
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = ca_pem,
        .cert_len = ca_len,
        .timeout_ms = timeout_ms,
        .buffer_size = buffer_size,
        .buffer_size_tx = tx_size,
    };
    return esp_http_client_init(&cfg);
}

esp_http_client_handle_t ring_http_init_bundle(const char *url, int timeout_ms,
                                               size_t buffer_size, size_t tx_size)
{
    if (url == NULL) {
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = buffer_size,
        .buffer_size_tx = tx_size,
    };
    return esp_http_client_init(&cfg);
}

esp_err_t ring_http_set_bearer(esp_http_client_handle_t c, const char *api_key)
{
    if (c == NULL || api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(api_key) > RING_HTTP_MAX_BEARER_KEY) {
        /* A truncated key would silently produce a malformed header. */
        return ESP_ERR_INVALID_ARG;
    }
    char auth[200];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    return esp_http_client_set_header(c, "Authorization", auth);
}

esp_err_t ring_http_read_body_ex(esp_http_client_handle_t c,
                                 char *buf, size_t cap, int *out_len,
                                 bool *truncated)
{
    if (c == NULL || buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (truncated != NULL) {
        *truncated = false;
    }

    int total = 0;
    for (;;) {
        if (total >= (int)cap - 1) {
            /* Buffer full: probe for one more byte so a short buffer is
             * reported as truncation rather than as a complete body. */
            char extra;
            int r = esp_http_client_read(c, &extra, 1);
            if (r < 0) {
                return ESP_ERR_INVALID_RESPONSE;    /* transport error */
            }
            if (r > 0 && truncated != NULL) {
                *truncated = true;
            }
            break;
        }
        int r = esp_http_client_read(c, buf + total, (int)cap - 1 - total);
        if (r < 0) {
            /* esp_http_client_read() returns a negative esp_err_t on error;
             * treating it as EOF would hand the caller a truncated body with
             * ESP_OK. */
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (r == 0) {
            break;                                  /* clean end of stream */
        }
        total += r;
    }
    buf[total] = '\0';
    if (out_len != NULL) {
        *out_len = total;
    }
    return total > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ring_http_read_body(esp_http_client_handle_t c,
                              char *buf, size_t cap, int *out_len)
{
    return ring_http_read_body_ex(c, buf, cap, out_len, NULL);
}

static bool sse_buf_reserve(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) {
        return true;
    }
    size_t ncap = *cap ? *cap : 8192;
    while (ncap < need) {
        ncap *= 2;
    }
    char *nb = heap_caps_realloc(*buf, ncap, MALLOC_CAP_SPIRAM);
    if (nb == NULL) {
        return false;
    }
    *buf = nb;
    *cap = ncap;
    return true;
}

bool ring_http_sse_init(ring_http_sse_t *s, esp_http_client_handle_t client)
{
    if (s == NULL || client == NULL) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->client = client;
    if (!sse_buf_reserve(&s->carry, &s->carry_cap, 8192) ||
        !sse_buf_reserve(&s->line, &s->line_cap, 8192)) {
        free(s->carry);
        s->carry = NULL;
        return false;
    }
    return true;
}

void ring_http_sse_deinit(ring_http_sse_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->carry);
    free(s->line);
    s->carry = NULL;
    s->line = NULL;
}

const char *ring_http_sse_next(ring_http_sse_t *s, bool *truncated)
{
    if (s == NULL) {
        return NULL;
    }
    if (truncated != NULL) {
        *truncated = false;
    }

    for (;;) {
        char *nl = memchr(s->carry, '\n', s->carry_len);
        size_t len;
        size_t consumed;
        bool have_line = false;

        if (nl != NULL) {
            len = (size_t)(nl - s->carry);
            consumed = len + 1;
            have_line = true;
        } else if (s->eof) {
            if (s->carry_len == 0) {
                return NULL;
            }
            len = s->carry_len;   /* final line without a newline */
            consumed = len;
            have_line = true;
        }

        if (have_line) {
            while (len > 0 && s->carry[len - 1] == '\r') {
                len--;
            }
            if (!sse_buf_reserve(&s->line, &s->line_cap, len + 1)) {
                if (truncated != NULL) {
                    *truncated = true;
                }
                return NULL;
            }
            memcpy(s->line, s->carry, len);
            s->line[len] = '\0';
            memmove(s->carry, s->carry + consumed, s->carry_len - consumed);
            s->carry_len -= consumed;
            return s->line;
        }

        /* No complete line yet: read another block. */
        if (!sse_buf_reserve(&s->carry, &s->carry_cap, s->carry_len + 4096)) {
            if (truncated != NULL) {
                *truncated = true;
            }
            return NULL;
        }
        int n = esp_http_client_read(s->client, s->carry + s->carry_len, 4096);
        if (n < 0) {
            /* Transport error: not end-of-stream. Mark it so the caller can
             * tell the difference, and stop. */
            s->err = true;
            if (truncated != NULL) {
                *truncated = true;
            }
            return NULL;
        }
        if (n == 0) {
            s->eof = true;
            continue;
        }
        s->carry_len += (size_t)n;
    }
}
