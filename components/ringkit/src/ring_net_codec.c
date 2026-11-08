/*
 * ringkit — ring_net Opus codec: encoder/decoder bring-up and RTP audio in.
 *
 * Split out of ring_net.c. The encoder runs on the app's audio task, the
 * decoder on the peer thread, hence separate handles and a lock around the
 * decoder. See ring_net_priv.h for the shared state.
 */
#include "ring_net_priv.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_dec_default.h"
#include "esp_opus_dec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "ring_net"

/* ------------------------------------------------------------- downlink ring */

void ring_net_dl_push(const uint8_t *pcm, size_t len)
{
    size_t dropped = 0;
    ring_byte_ring_push(&ring_net_s.dl_ring, pcm, len, &dropped);
}

/* ------------------------------------------------------------- codec */

esp_err_t ring_net_opus_open(void)
{
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RING_NET_SAMPLE_RATE;
    cfg.channel = RING_NET_OPUS_CHANNELS;
    cfg.bits_per_sample = 16;
    cfg.bitrate = RING_NET_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 0;          /* lowest CPU: the ESP32-S3 is also running Wi-Fi */
    cfg.enable_fec = true;
    cfg.enable_vbr = false;

    esp_audio_enc_config_t ecfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    esp_audio_err_t e = esp_audio_enc_open(&ecfg, &ring_net_s.enc);
    if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encoder open failed: %d", e);
        return ESP_FAIL;
    }
    esp_audio_enc_get_frame_size(ring_net_s.enc, &ring_net_s.enc_in_size, &ring_net_s.enc_out_size);
    ring_net_s.enc_out = malloc(ring_net_s.enc_out_size > 0 ? ring_net_s.enc_out_size : 1024);
    ring_net_s.enc_in = malloc(RING_NET_FRAME_SAMPLES * RING_NET_OPUS_CHANNELS * 2);
    if (ring_net_s.enc_out == NULL || ring_net_s.enc_in == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "opus encoder: in %d B out %d B", ring_net_s.enc_in_size, ring_net_s.enc_out_size);

    esp_opus_dec_cfg_t dcfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dcfg.sample_rate = RING_NET_SAMPLE_RATE;
    /* The endpoint negotiates stereo Opus (a=rtpmap:opus/48000/2), so the
     * decoder must be opened for two channels; on_audio_data folds to mono. */
    dcfg.channel = ESP_AUDIO_DUAL;
    dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;

    esp_audio_dec_cfg_t adcfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &dcfg,
        .cfg_sz = sizeof(dcfg),
    };
    e = esp_audio_dec_open(&adcfg, &ring_net_s.dec);
    if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus decoder open failed: %d", e);
        return ESP_FAIL;
    }
    /* Worst case output for one 20 ms frame: stereo 16-bit PCM. */
    ring_net_s.dec_out_size = RING_NET_FRAME_BYTES * 2;
    ring_net_s.dec_out = malloc(ring_net_s.dec_out_size);
    ring_net_s.dec_mono = malloc(RING_NET_FRAME_BYTES);
    if (ring_net_s.dec_out == NULL || ring_net_s.dec_mono == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ring_net_s.dec_mutex = xSemaphoreCreateMutex();
    return ring_net_s.dec_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------- peer callback
 *
 * Runs on the peer task, which also runs esp_peer_main_loop: keep the stack
 * small and never log big strings.
 */

int ring_net_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->size <= 0 || ring_net_s.dec == NULL) {
        return 0;
    }

    xSemaphoreTake(ring_net_s.dec_mutex, portMAX_DELAY);
    esp_audio_dec_in_raw_t raw = {
        .buffer = frame->data,
        .len = (uint32_t)frame->size,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = ring_net_s.dec_out,
        .len = (uint32_t)ring_net_s.dec_out_size,
    };
    esp_audio_err_t e = esp_audio_dec_process(ring_net_s.dec, &raw, &out);
    xSemaphoreGive(ring_net_s.dec_mutex);

    if (e == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
        /* The endpoint sends stereo; the ring's audio path is mono. Fold
         * L/R here (the decoder is opened for two channels, so the buffer
         * holds interleaved stereo and decoded_size counts both channels). */
        const int16_t *stereo = (const int16_t *)ring_net_s.dec_out;
        size_t frames = out.decoded_size / (2 * sizeof(int16_t));
        if (frames > RING_NET_FRAME_SAMPLES) {
            frames = RING_NET_FRAME_SAMPLES;
        }
        int16_t *mono = (int16_t *)ring_net_s.dec_mono;
        for (size_t i = 0; i < frames; i++) {
            mono[i] = (int16_t)(((int32_t)stereo[2 * i] +
                                 (int32_t)stereo[2 * i + 1]) / 2);
        }
        ring_net_dl_push(ring_net_s.dec_mono, frames * sizeof(int16_t));
    } else if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "opus decode failed: %d", e);
    }

    ring_net_s.speaking = true;
    /* The reply can start after the button was released (state READY), so
     * accept both pre-turn states here. */
    ring_net_state_t st = ring_net_state();
    if (st == RING_NET_LISTENING || st == RING_NET_READY) {
        ring_net_state_set(RING_NET_SPEAKING);
    }
    return 0;
}
