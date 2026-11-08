/*
 * ringkit — shared HTTPS helpers.
 *
 * Every cloud module talks to the same host with the same pinned CA and the
 * same Authorization header. This wraps the esp_http_client setup so the
 * modules only deal with their protocol.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an HTTPS client with the pinned CA and a bounded buffer.
 *
 * @param url          Full URL, e.g. "https://maas.qianwenaiapi.com/...".
 * @param ca_pem       PEM chain (see ring_ca_pem()).
 * @param ca_len       Bytes in @p ca_pem.
 * @param timeout_ms   Socket timeout.
 * @param buffer_size  esp_http_client RX buffer (internal RAM; keep it small).
 * @param tx_size      esp_http_client TX buffer.
 */
esp_http_client_handle_t ring_http_init(const char *url, const char *ca_pem,
                                        size_t ca_len, int timeout_ms,
                                        size_t buffer_size, size_t tx_size);

/** Set `Authorization: Bearer <key>`. */
esp_err_t ring_http_set_bearer(esp_http_client_handle_t c, const char *api_key);

/**
 * @brief Read the whole response body (chunked-safe) into @p buf.
 *
 * @param buf      Destination (NUL-terminated).
 * @param cap      Capacity.
 * @param out_len  Optional: bytes read.
 * @return ESP_OK when at least one byte was read.
 */
esp_err_t ring_http_read_body(esp_http_client_handle_t c,
                              char *buf, size_t cap, int *out_len);

/**
 * @brief Read one SSE line (up to '\n', CR skipped) into @p dst.
 *
 * Unlike a naive loop, this reports end-of-stream: when the underlying read
 * returns 0 and no bytes were accumulated, @p eof is set and the caller must
 * stop instead of spinning.
 *
 * @param dst   Destination (NUL-terminated).
 * @param pos   In/out offset; reset to 0 before each call.
 * @param cap   Capacity of @p dst.
 * @param eof   Optional: set when the stream ended.
 * @return false when the line was truncated (buffer full).
 */
bool ring_http_sse_line(esp_http_client_handle_t c,
                        char *dst, int *pos, int cap, bool *eof);

#ifdef __cplusplus
}
#endif
