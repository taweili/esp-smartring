/*
 * ringkit — ring_chat internal interface.
 *
 * Private to the ringkit component. The HTTPS REST cascade is split across
 * four translation units: ring_chat.c (init, history, shared audio helpers),
 * ring_stt.c (speech-to-text), ring_llm.c (chat completion + history JSON)
 * and ring_tts.c (speech synthesis SSE). This header is the shared contract.
 *
 * Not part of the public API — applications include ringkit/ring_chat.h only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ringkit/ring_ca.h"
#include "ringkit/ring_chat.h"

/* ------------------------------------------------------------- config */

#define RING_CHAT_HOST        "maas.qianwenaiapi.com"
#define RING_CHAT_STT_URL     "https://" RING_CHAT_HOST "/compatible-mode/v1/chat/completions"
#define RING_CHAT_LLM_URL     RING_CHAT_STT_URL
#define RING_CHAT_TTS_URL     "https://" RING_CHAT_HOST "/api/v1/services/audio/tts/SpeechSynthesizer"

/* The pinned CA (the server sends only its leaf cert). */
#define RING_CHAT_CA_PEM       ring_ca_pem()
#define RING_CHAT_CA_LEN       ring_ca_pem_len()

#define RING_CHAT_MAX_HISTORY_TURNS  20     /* compile-time cap for safety */
#define RING_CHAT_WAV_CAP            (512 * 1024)   /* ~16 s @ 16 kHz mono: 512 KB */
#define RING_CHAT_B64_CAP            ((RING_CHAT_WAV_CAP + 2) / 3 * 4 + 256) /* base64 + Data URI prefix */
/* TTS SSE: the server sends one base64 PCM blob per `sentence-synthesis`
 * event, up to ~100 KB (1.6 s @ 48 kHz). The base64/PCM buffers must hold a
 * whole blob or the chunk is silently truncated. The SSE line itself is read
 * by a growable buffered reader (ring_http_sse_t). */
#define RING_CHAT_TTS_B64_CAP        (128 * 1024)
#define RING_CHAT_TTS_PCM_SAMPLES    ((RING_CHAT_TTS_B64_CAP / 4) * 3)
#define RING_CHAT_LLM_REPLY_CAP      8192
#define RING_CHAT_JSON_CAP           8192

#define RING_CHAT_STT_TIMEOUT_MS     15000
#define RING_CHAT_LLM_TIMEOUT_MS     20000
#define RING_CHAT_TTS_TIMEOUT_MS     40000

/* ------------------------------------------------------------- state */

typedef struct ring_chat_history_t {
    char **user_msgs;
    char **assistant_msgs;
    int    count;
    int    cap;
} ring_chat_history_t;

typedef struct {
    ring_chat_cfg_t     cfg;
    char                api_key[160];
    ring_chat_history_t hist;
    bool                init;
} ring_chat_ctx_t;

extern ring_chat_ctx_t ring_chat_s;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_chat.c; used by the STT / LLM / TTS stages. */

/* Thin wrapper over ring_json_get_str() to keep the local 4-arg call sites. */
bool ring_chat_json_get_str(const char *json, const char *key,
                            char *out, size_t cap);

void ring_chat_pcm16_to_float(const int16_t *in, float *out, size_t n);
void ring_chat_float_to_pcm16(const float *in, int16_t *out, size_t n);

/* Write a canonical 44-byte WAV header into dst (PCM 16-bit mono). */
void ring_chat_wav_header(uint8_t *dst, int sample_rate, int data_bytes);
