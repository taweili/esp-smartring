/*
 * Host-test stubs for ring_http.c.
 *
 * The HTTP client is replaced by a scriptable byte source so the error paths
 * (a negative esp_http_client_read return) and the truncation probe can be
 * exercised deterministically. This file is linked ONLY into
 * test_ring_http; the other tests keep stub_esp.c's no-op ring_http.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

/* ---- controllable fake response ------------------------------------------ */

struct fake_http {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      err_at;      /* return -1 once pos reaches this; SIZE_MAX = never */
};

static struct fake_http s_fake = {
    .data = NULL, .len = 0, .pos = 0, .err_at = (size_t)-1,
};

void test_http_fake_set(const char *data, size_t len, size_t err_at)
{
    s_fake.data = data;
    s_fake.len = len;
    s_fake.pos = 0;
    s_fake.err_at = err_at;
}

/* ---- heap caps ----------------------------------------------------------- */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    return realloc(ptr, size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

/* ---- esp_http_client ----------------------------------------------------- */

esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config)
{
    (void)config;
    /* Non-NULL dummy: ring_http only forwards the handle. */
    return (esp_http_client_handle_t)&s_fake;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value)
{
    (void)client; (void)key; (void)value;
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer,
                         int len)
{
    (void)client;
    if (s_fake.pos >= s_fake.err_at) {
        return -1;                      /* transport error */
    }
    size_t remain = s_fake.len - s_fake.pos;
    size_t n = remain < (size_t)len ? remain : (size_t)len;
    memcpy(buffer, s_fake.data + s_fake.pos, n);
    s_fake.pos += n;
    return (int)n;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ESP_ERR";
}
