/*
 * ringkit — wall-clock sync (SNTP).
 *
 * TLS certificate validation needs a plausible clock, and every cloud module
 * (ring_net, ring_ws, ring_chat) needs the same sync. This is the single
 * implementation; it is idempotent and safe to call from any task.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sync the wall clock over SNTP.
 *
 * Initialises SNTP once, then waits up to @p timeout_ms for a sync. Returns
 * ESP_OK once the clock is valid; a retry does not re-initialise SNTP.
 *
 * @param timeout_ms  Max time to wait for the first sync (0 = poll only).
 * @return ESP_OK once the clock is valid, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t ring_time_sync(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
