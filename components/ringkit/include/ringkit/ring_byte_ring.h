/*
 * ringkit — byte ring buffer (newest-wins) with a mutex.
 *
 * Used for the downlink PCM rings in ring_net and ring_ws. A fixed-size byte
 * ring is simpler than a PCM-aware one because both producers push whole
 * decoded frames and both consumers pop whole frames; overflow drops whole
 * 16-bit samples so the stream never loses alignment.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t          *buf;
    size_t            cap;
    size_t            head;
    size_t            tail;
    size_t            count;
    SemaphoreHandle_t mutex;
} ring_byte_ring_t;

/**
 * @brief Allocate and initialise.
 *
 * @param cap  Capacity in bytes (should be even for 16-bit PCM).
 * @param caps `MALLOC_CAP_SPIRAM` or `MALLOC_CAP_INTERNAL`.
 */
esp_err_t ring_byte_ring_init(ring_byte_ring_t *r, size_t cap, uint32_t caps);

/** Free the buffer. Safe on a zero-initialised ring. */
void ring_byte_ring_deinit(ring_byte_ring_t *r);

/** Drop all contents without freeing the buffer. */
void ring_byte_ring_reset(ring_byte_ring_t *r);

/**
 * @brief Append bytes; on overflow discard the oldest bytes (newest wins).
 *
 * @param dropped  Optional: receives the number of bytes discarded.
 */
void ring_byte_ring_push(ring_byte_ring_t *r, const uint8_t *data,
                         size_t len, size_t *dropped);

/** Read up to @p len bytes into @p data. Returns the number read. */
size_t ring_byte_ring_pop(ring_byte_ring_t *r, uint8_t *data, size_t len);

/** Bytes currently buffered. */
size_t ring_byte_ring_count(ring_byte_ring_t *r);

#ifdef __cplusplus
}
#endif
