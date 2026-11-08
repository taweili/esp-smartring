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
 *
 * @param buf         Destination buffer.
 * @param len         Bytes to read.
 * @param timeout_ms  Read timeout in milliseconds.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the buffer was not filled.
 */
esp_err_t ring_audio_read(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Blocking PCM playback; returns once @p len bytes are written.
 *
 * Does NOT manage mute. The caller owns the sequence, otherwise the speaker
 * pops:
 *   ring_audio_set_mute(false)  ->  ring_audio_write(...)  ->  ring_audio_set_mute(true)
 *
 * @param buf   PCM buffer (48 kHz / 16-bit / stereo interleaved).
 * @param len   Bytes to write.
 * @return ESP_OK on success.
 */
esp_err_t ring_audio_write(const void *buf, size_t len);

/**
 * @brief Output mute switch (see the playback sequence above).
 *
 * @param mute  true to mute the speaker.
 * @return ESP_OK on success.
 */
esp_err_t ring_audio_set_mute(bool mute);

/*
 * The codec is fixed at 48 kHz / 16-bit / **stereo interleaved**, but the
 * interesting audio work on this board (loopback, uplink to a cloud model,
 * level meters) is mono. Both conversions are exact (average L/R, duplicate
 * L→R) and live here so no app re-implements them — `ring_net` and the voice
 * examples previously carried near-identical copies.
 */

/**
 * @brief Fold 48 kHz / 16-bit stereo interleaved -> mono (average L/R).
 *
 * @param stereo  Interleaved stereo frames.
 * @param mono    Destination mono frames.
 * @param frames  Number of frames.
 */
void ring_audio_stereo_to_mono(const int16_t *stereo, int16_t *mono,
                               size_t frames);

/**
 * @brief Expand mono -> 48 kHz / 16-bit stereo interleaved.
 *
 * @param mono    Mono frames.
 * @param stereo  Destination interleaved stereo frames.
 * @param frames  Number of frames.
 */
void ring_audio_mono_to_stereo(const int16_t *mono, int16_t *stereo,
                               size_t frames);

/* Sane defaults measured on this board (see notes/hardware-bsp-reference.md). */
#define RING_AUDIO_REC_GAIN_DB 42  /**< driver rounds down to a 6 dB step (ES8311 max) */
#define RING_AUDIO_OUT_VOL     75  /**< 0..100                             */

#ifdef __cplusplus
}
#endif
