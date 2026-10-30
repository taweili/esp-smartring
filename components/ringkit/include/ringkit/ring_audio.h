/*
 * ringkit — audio: ES8311 codec + I2S, 48 kHz / 16-bit / stereo interleaved.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Codec handle for fine-grained control (gain, volume, streaming). */
esp_codec_dev_handle_t ring_audio_codec(void);

/** I2S RX channel for raw PCM reads. */
i2s_chan_handle_t ring_audio_rx(void);

/**
 * @brief Blocking PCM record; returns once @p len bytes are filled.
 *
 * Format is fixed: 48 kHz, 16-bit, stereo interleaved (~192 kB/s).
 */
esp_err_t ring_audio_read(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Blocking PCM playback; returns once @p len bytes are written.
 *
 * Does NOT manage mute. The caller owns the sequence, otherwise the speaker
 * pops:
 *   ring_audio_set_mute(false)  ->  ring_audio_write(...)  ->  ring_audio_set_mute(true)
 */
esp_err_t ring_audio_write(const void *buf, size_t len);

/** Output mute switch (see the playback sequence above). */
esp_err_t ring_audio_set_mute(bool mute);

/* Sane defaults measured on this board (see notes/esp-smartring.md). */
#define RING_AUDIO_REC_GAIN_DB 36  /**< driver rounds down to a 6 dB step */
#define RING_AUDIO_OUT_VOL     75  /**< 0..100                             */

#ifdef __cplusplus
}
#endif
