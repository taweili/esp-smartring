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
 * @brief Buffered SSE reader.
 *
 * Reads the stream in blocks rather than a byte at a time: the TTS endpoint
 * sends ~100 KB `data:` lines, and a per-byte `esp_http_client_read()` costs
 * ~60 µs/byte on this stack (10 s for a 0.7 s reply).
 */
typedef struct {
    esp_http_client_handle_t client;
    char  *carry;        /**< bytes read past the last '\n' */
    size_t carry_len;
    size_t carry_cap;
    char  *line;         /**< growable line buffer */
    size_t line_cap;
    bool   eof;
} ring_http_sse_t;

/**
 * @brief Initialise the reader over an open HTTP client.
 *
 * @return false when the scratch buffers could not be allocated.
 */
bool ring_http_sse_init(ring_http_sse_t *s, esp_http_client_handle_t client);

/** Release the scratch buffers. */
void ring_http_sse_deinit(ring_http_sse_t *s);

/**
 * @brief Next SSE line, without the trailing newline.
 *
 * The returned pointer is valid until the next call. CR is stripped.
 *
 * @param s          Reader.
 * @param truncated  Optional; set when the line had to be truncated.
 * @return the line, or NULL at the end of the stream (or on allocation
 *         failure, with @p truncated set).
 */
const char *ring_http_sse_next(ring_http_sse_t *s, bool *truncated);

#ifdef __cplusplus
}
#endif
