/*
 * Host-test stub for esp_http_client.h.
 *
 * Only the surface ring_http.h and ring_llm.c use. The handle is an opaque
 * pointer; the stub functions are no-ops (the host tests never perform HTTP).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct esp_http_client *esp_http_client_handle_t;

/* Minimal subset of the real esp_http_client_config_t — only the fields
 * ring_http.c initialises. */
typedef struct {
    const char *url;
    const char *cert_pem;
    size_t cert_len;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *conf);
} esp_http_client_config_t;

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
} esp_http_client_method_t;

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                      esp_http_client_method_t method);
esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                      const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
void esp_http_client_cleanup(esp_http_client_handle_t client);

/* Streamed request/response, used by ring_llm.c and ring_geodis_*.c. The
 * stubs fail cleanly so a codepath that expects a body takes its error path
 * instead of dereferencing something. */
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer,
                          int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
int esp_http_client_read_response(esp_http_client_handle_t client, char *buffer,
                                  int len);
