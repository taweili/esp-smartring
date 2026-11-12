/*
 * ringkit — ring_geodis mock transport (CONFIG_RINGKIT_GEODIS_MOCK).
 *
 * A drop-in replacement for the network half of ring_geodis: same public API,
 * same state sequence, canned transcript + tool call + audio, no Wi-Fi, no
 * SNTP, no DTLS, no gateway. It exists because the bench LAN blocks outbound
 * UDP/DTLS, so without it the UI, the state machine and the push-to-talk
 * gesture cannot be developed or demonstrated at the desk at all.
 *
 * What it fakes, and with what timing:
 *
 *   start()             CONNECTING -> READY             after ~300 ms
 *   set_listening(true) LISTENING
 *   commit()            SPEAKING after ~400 ms, then a canned user
 *                       transcript, a canned tool call ~600 ms later, and
 *                       1.6 s of canned 48 kHz PCM; READY at the end
 *   cancel()            drops the queued PCM and returns to READY
 *
 * The delays are deliberately non-zero: a mock that answers instantly hides
 * every "the UI says nothing happened" bug, which is the class this is meant
 * to expose. They are all overridable through ring_geodis_mock_config() so a
 * test can speed them up.
 *
 * Not part of the public API; applications include ringkit/ring_geodis.h only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tune the mock's timing. All fields are optional (0 = keep default).
 *
 * Only meaningful when CONFIG_RINGKIT_GEODIS_MOCK is enabled; in a real build
 * this compiles to a no-op returning ESP_ERR_NOT_SUPPORTED.
 *
 * @param connect_ms        start() -> READY
 * @param reply_ms          commit() -> first reply audio
 * @param tool_ms           first reply audio -> tool call announcement
 * @param drop_reply_audio  true = never push the canned PCM (state/tool only)
 */
typedef struct {
    uint32_t connect_ms;
    uint32_t reply_ms;
    uint32_t tool_ms;
    bool     drop_reply_audio;
} ring_geodis_mock_cfg_t;

esp_err_t ring_geodis_mock_config(const ring_geodis_mock_cfg_t *cfg);

/**
 * @brief Number of canned reply samples, for a UI that wants to show a
 *        progress bar. 0 when the mock is not compiled in.
 */
size_t ring_geodis_mock_reply_samples(void);

#ifdef __cplusplus
}
#endif
