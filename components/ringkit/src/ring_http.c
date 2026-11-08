/*
 * ringkit — shared HTTPS helpers. See ring_http.h.
 */
#include "ringkit/ring_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

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

esp_err_t ring_http_set_bearer(esp_http_client_handle_t c, const char *api_key)
{
    if (c == NULL || api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char auth[200];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    return esp_http_client_set_header(c, "Authorization", auth);
}

esp_err_t ring_http_read_body(esp_http_client_handle_t c,
                              char *buf, size_t cap, int *out_len)
{
    if (c == NULL || buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int total = 0;
    while (total < (int)cap - 1) {
        int r = esp_http_client_read(c, buf + total, (int)cap - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    if (out_len != NULL) {
        *out_len = total;
    }
    return total > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

bool ring_http_sse_line(esp_http_client_handle_t c,
                        char *dst, int *pos, int cap, bool *eof)
{
    if (c == NULL || dst == NULL || pos == NULL || cap <= 0) {
        return false;
    }
    if (eof != NULL) {
        *eof = false;
    }
    if (*pos < 0 || *pos >= cap) {
        *pos = 0;
    }
    bool truncated = false;
    while (*pos < cap - 1) {
        char ch;
        int r = esp_http_client_read(c, &ch, 1);
        if (r <= 0) {
            if (eof != NULL) {
                *eof = true;
            }
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        dst[(*pos)++] = ch;
    }
    if (*pos >= cap - 1) {
        truncated = true;
    }
    dst[*pos] = '\0';
    return !truncated;
}
