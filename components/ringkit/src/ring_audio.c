/*
 * ringkit — audio. See ring_audio.h.
 */
#include "ringkit/ring_audio.h"

#include "bsp/smartring_plus.h"

esp_codec_dev_handle_t ring_audio_codec(void)
{
    return bsp_audio_get_codec();
}

i2s_chan_handle_t ring_audio_rx(void)
{
    return bsp_audio_get_rx();
}

esp_err_t ring_audio_read(void *buf, size_t len, uint32_t timeout_ms)
{
    return bsp_audio_read(buf, len, timeout_ms);
}

esp_err_t ring_audio_write(const void *buf, size_t len)
{
    /* bsp_audio_write already normalises the inverted esp_codec_dev_write
     * return value (0 == success) into an esp_err_t. */
    return bsp_audio_write(buf, len);
}

esp_err_t ring_audio_set_mute(bool mute)
{
    return bsp_audio_set_mute(mute);
}

void ring_audio_stereo_to_mono(const int16_t *stereo, int16_t *mono,
                               size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        mono[i] = (int16_t)(((int32_t)stereo[2 * i] +
                             (int32_t)stereo[2 * i + 1]) / 2);
    }
}

void ring_audio_mono_to_stereo(const int16_t *mono, int16_t *stereo,
                               size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
}
