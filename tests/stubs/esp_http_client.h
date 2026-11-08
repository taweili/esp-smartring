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

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
} esp_http_client_method_t;

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                      esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                      const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
void esp_http_client_cleanup(esp_http_client_handle_t client);
