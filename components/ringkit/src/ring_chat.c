/*
 * ringkit — HTTPS REST cascade: STT → LLM → TTS.
 *
 * Three sequential HTTPS POSTs to maas.qianwenaiapi.com. All blocking;
 * call from the app's worker task. The TTS SSE audio callback runs on
 * that same task and may take the LVGL lock briefly.
 *
 * This file owns init, conversation history and the small audio helpers
 * shared by the stages. The stages themselves live in ring_stt.c,
 * ring_llm.c and ring_tts.c.
 */
#include "ring_chat_priv.h"

#include "ringkit/ring_json.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ring_chat"

ring_chat_ctx_t ring_chat_s;

/* Thin wrapper over ring_json_get_str() to keep the local 4-arg call sites. */
bool ring_chat_json_get_str(const char *json, const char *key,
                            char *out, size_t cap)
{
    return ring_json_get_str(json, key, out, cap, NULL);
}

/* ------------------------------------------------------------- audio helpers */

void ring_chat_pcm16_to_float(const int16_t *in, float *out, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (float)in[i];
}

void ring_chat_float_to_pcm16(const float *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (v < -32768.0f) v = -32768.0f;
        if (v > 32767.0f) v = 32767.0f;
        out[i] = (int16_t)v;
    }
}

/* Write a canonical 44-byte WAV header into dst (PCM 16-bit mono). */
void ring_chat_wav_header(uint8_t *dst, int sample_rate, int data_bytes)
{
    int bits = 16, channels = 1;
    int byte_rate = sample_rate * channels * bits / 8;
    int block_align = channels * bits / 8;
    int chunk_size = 36 + data_bytes;
    /* RIFF */
    dst[0] = 'R'; dst[1] = 'I'; dst[2] = 'F'; dst[3] = 'F';
    dst[4] = (uint8_t)(chunk_size & 0xff);
    dst[5] = (uint8_t)((chunk_size >> 8) & 0xff);
    dst[6] = (uint8_t)((chunk_size >> 16) & 0xff);
    dst[7] = (uint8_t)((chunk_size >> 24) & 0xff);
    dst[8] = 'W'; dst[9] = 'A'; dst[10] = 'V'; dst[11] = 'E';
    /* fmt  */
    dst[12] = 'f'; dst[13] = 'm'; dst[14] = 't'; dst[15] = ' ';
    dst[16] = 16; dst[17] = 0; dst[18] = 0; dst[19] = 0; /* chunk size */
    dst[20] = 1; dst[21] = 0;  /* PCM */
    dst[22] = (uint8_t)channels; dst[23] = 0;
    dst[24] = (uint8_t)(sample_rate & 0xff);
    dst[25] = (uint8_t)((sample_rate >> 8) & 0xff);
    dst[26] = (uint8_t)((sample_rate >> 16) & 0xff);
    dst[27] = (uint8_t)((sample_rate >> 24) & 0xff);
    dst[28] = (uint8_t)(byte_rate & 0xff);
    dst[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    dst[30] = (uint8_t)((byte_rate >> 16) & 0xff);
    dst[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    dst[32] = (uint8_t)block_align; dst[33] = 0;
    dst[34] = bits; dst[35] = 0;
    /* data */
    dst[36] = 'd'; dst[37] = 'a'; dst[38] = 't'; dst[39] = 'a';
    dst[40] = (uint8_t)(data_bytes & 0xff);
    dst[41] = (uint8_t)((data_bytes >> 8) & 0xff);
    dst[42] = (uint8_t)((data_bytes >> 16) & 0xff);
    dst[43] = (uint8_t)((data_bytes >> 24) & 0xff);
}

/* ------------------------------------------------------------- history */

esp_err_t ring_chat_add_user(const char *text)
{
    if (ring_chat_s.hist.count >= ring_chat_s.hist.cap) return ESP_ERR_NO_MEM;
    ring_chat_s.hist.user_msgs[ring_chat_s.hist.count] = strdup(text);
    if (!ring_chat_s.hist.user_msgs[ring_chat_s.hist.count]) return ESP_ERR_NO_MEM;
    ring_chat_s.hist.count++;
    return ESP_OK;
}

esp_err_t ring_chat_add_assistant(const char *text)
{
    if (ring_chat_s.hist.count >= ring_chat_s.hist.cap) return ESP_ERR_NO_MEM;
    ring_chat_s.hist.assistant_msgs[ring_chat_s.hist.count] = strdup(text);
    if (!ring_chat_s.hist.assistant_msgs[ring_chat_s.hist.count]) return ESP_ERR_NO_MEM;
    ring_chat_s.hist.count++;
    return ESP_OK;
}

void ring_chat_reset_history(void)
{
    for (int i = 0; i < ring_chat_s.hist.count; i++) {
        free(ring_chat_s.hist.user_msgs[i]);    ring_chat_s.hist.user_msgs[i] = NULL;
        free(ring_chat_s.hist.assistant_msgs[i]); ring_chat_s.hist.assistant_msgs[i] = NULL;
    }
    ring_chat_s.hist.count = 0;
}

/* ------------------------------------------------------------- init */

esp_err_t ring_chat_init(const ring_chat_cfg_t *cfg, const char *api_key)
{
    if (cfg == NULL || api_key == NULL) return ESP_ERR_INVALID_ARG;
    memset(&ring_chat_s, 0, sizeof(ring_chat_s));

    ring_chat_s.cfg = *cfg;
    snprintf(ring_chat_s.api_key, sizeof(ring_chat_s.api_key), "%s", api_key);

    /* Allocate history arrays for the compile-time worst case (MAX_HISTORY_TURNS
     * pairs); build_messages() windows to cfg->max_history_turns. */
    ring_chat_s.hist.cap = RING_CHAT_MAX_HISTORY_TURNS * 2;
    ring_chat_s.hist.user_msgs = calloc(ring_chat_s.hist.cap, sizeof(char *));
    ring_chat_s.hist.assistant_msgs = calloc(ring_chat_s.hist.cap, sizeof(char *));
    if (!ring_chat_s.hist.user_msgs || !ring_chat_s.hist.assistant_msgs) {
        free(ring_chat_s.hist.user_msgs);
        free(ring_chat_s.hist.assistant_msgs);
        return ESP_ERR_NO_MEM;
    }

    ring_chat_s.init = true;
    ESP_LOGI(TAG, "initialised (model %s, llm %s, tts %s, voice %s)",
             cfg->stt_model, cfg->llm_model, cfg->tts_model, cfg->voice);
    return ESP_OK;
}
