/*
 * ringkit — byte ring buffer. See ring_byte_ring.h.
 */
#include "ringkit/ring_byte_ring.h"

#include "esp_heap_caps.h"

esp_err_t ring_byte_ring_init(ring_byte_ring_t *r, size_t cap, uint32_t caps)
{
    if (r == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    r->buf = heap_caps_malloc(cap, caps);
    if (r->buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    r->cap = cap;
    r->head = r->tail = r->count = 0;
    r->mutex = xSemaphoreCreateMutex();
    if (r->mutex == NULL) {
        heap_caps_free(r->buf);
        r->buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ring_byte_ring_deinit(ring_byte_ring_t *r)
{
    if (r == NULL) {
        return;
    }
    if (r->mutex != NULL) {
        vSemaphoreDelete(r->mutex);
        r->mutex = NULL;
    }
    if (r->buf != NULL) {
        heap_caps_free(r->buf);
        r->buf = NULL;
    }
    r->cap = r->head = r->tail = r->count = 0;
}

void ring_byte_ring_reset(ring_byte_ring_t *r)
{
    if (r == NULL || r->mutex == NULL) {
        return;
    }
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    r->head = r->tail = r->count = 0;
    xSemaphoreGive(r->mutex);
}

void ring_byte_ring_push(ring_byte_ring_t *r, const uint8_t *data,
                         size_t len, size_t *dropped)
{
    if (r == NULL || r->buf == NULL || len == 0) {
        return;
    }
    size_t lost = 0;
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        if (r->count >= r->cap) {
            /* Newest wins. Drop in whole samples (the ring is even-sized) so
             * the byte stream never loses 16-bit alignment. */
            size_t drop = 2;
            if (r->count < drop) {
                drop = r->count;
            }
            r->tail = (r->tail + drop) % r->cap;
            r->count -= drop;
            lost += drop;
        }
        r->buf[r->head] = data[i];
        r->head = (r->head + 1) % r->cap;
        r->count++;
    }
    xSemaphoreGive(r->mutex);
    if (dropped != NULL) {
        *dropped = lost;
    }
}

size_t ring_byte_ring_pop(ring_byte_ring_t *r, uint8_t *data, size_t len)
{
    if (r == NULL || r->buf == NULL || data == NULL || len == 0) {
        return 0;
    }
    size_t got = 0;
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    while (got < len && r->count > 0) {
        data[got++] = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        r->count--;
    }
    xSemaphoreGive(r->mutex);
    return got;
}

size_t ring_byte_ring_count(ring_byte_ring_t *r)
{
    if (r == NULL || r->mutex == NULL) {
        return 0;
    }
    size_t n;
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    n = r->count;
    xSemaphoreGive(r->mutex);
    return n;
}
