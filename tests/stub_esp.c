/*
 * Host-test stubs for the ESP-IDF / FreeRTOS surface ringkit's pure-logic
 * modules touch. Linked into every host test so the real ringkit sources
 * compile and run unmodified on the development machine.
 *
 * Nothing here is supposed to be clever: malloc backs the heap-caps API, the
 * mutex is a dummy handle (tests are single-threaded), and the HTTP client
 * is a no-op (the host tests never perform network I/O).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ringkit/ring_ca.h"
#include "ringkit/ring_http.h"

/* ---- esp_err ---- */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                 return "ESP_OK";
    case ESP_FAIL:               return "ESP_FAIL";
    case ESP_ERR_NO_MEM:         return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:    return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:  return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:   return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:      return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:  return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    default:                     return "UNKNOWN";
    }
}

/* ---- heap caps ---- */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

/* ---- FreeRTOS mutex (dummy) ---- */

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int dummy;
    return (SemaphoreHandle_t)&dummy;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    (void)mutex;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)mutex;
    (void)ticks;
    return 1;
}

int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    (void)mutex;
    return 1;
}

/* ---- esp_http_client (no-op) ---- */

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                      esp_http_client_method_t method)
{
    (void)client;
    (void)method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                      const char *key, const char *value)
{
    (void)client;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data, int len)
{
    (void)client;
    (void)data;
    (void)len;
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return 200;
}

void esp_http_client_cleanup(esp_http_client_handle_t client)
{
    (void)client;
}

/* ---- ring_ca ---- */

const char *ring_ca_pem(void)
{
    return "";
}

size_t ring_ca_pem_len(void)
{
    return 0;
}

/* ---- ring_http (no-op) ---- */

esp_http_client_handle_t ring_http_init(const char *url, const char *ca_pem,
                                        size_t ca_len, int timeout_ms,
                                        size_t buffer_size, size_t tx_size)
{
    (void)url;
    (void)ca_pem;
    (void)ca_len;
    (void)timeout_ms;
    (void)buffer_size;
    (void)tx_size;
    return NULL;
}

esp_err_t ring_http_set_bearer(esp_http_client_handle_t c, const char *api_key)
{
    (void)c;
    (void)api_key;
    return ESP_OK;
}

esp_err_t ring_http_read_body(esp_http_client_handle_t c,
                              char *buf, size_t cap, int *out_len)
{
    (void)c;
    (void)buf;
    (void)cap;
    (void)out_len;
    return ESP_FAIL;
}

bool ring_http_sse_line(esp_http_client_handle_t c,
                        char *dst, int *pos, int cap, bool *eof)
{
    (void)c;
    (void)dst;
    (void)pos;
    (void)cap;
    (void)eof;
    return false;
}
