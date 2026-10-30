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
