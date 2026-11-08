/*
 * ringkit — HTTPS REST cascade module for STT → LLM → TTS voice assistant.
 *
 * Three sequential HTTPS POSTs to maas.qianwenaiapi.com (all TCP/443):
 *   1. STT  — OpenAI-compatible Chat Completions with input_audio
 *   2. LLM  — OpenAI-compatible Chat Completions with transcript
 *   3. TTS  — DashScope SpeechSynthesizer (SSE, raw PCM output)
 *
 * Audio format on this interface is 48 kHz / 16-bit / mono PCM.
 * Resampling to/from the endpoint rates (16k STT input) happens inside.
 *
 * All functions block. Call from the app's worker task; the TTS audio
 * callback fires on that same task, so it can take the LVGL lock briefly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Session configuration — set once at ring_chat_init(). */
typedef struct {
    const char *stt_model;      /**< "qwen3-asr-flash"                      */
    const char *llm_model;      /**< "qwen3.8-flash"                        */
    const char *tts_model;      /**< "qwen-audio-3.1-tts-flash"             */
    const char *voice;          /**< "longanhuan_v3.1"                      */
    const char *system_prompt;  /**< optional, may be NULL                  */
    const char *stt_language;   /**< optional ASR language hint ("en")      */
    int         max_history_turns; /**< 0 = no limit (use with care)        */
} ring_chat_cfg_t;

/** TTS audio chunk: 48 kHz / 16-bit / mono PCM. */
typedef void (*ring_chat_audio_cb_t)(const int16_t *pcm, size_t samples,
                                     void *user);

/** Text delta (LLM streaming) or final result (STT). */
typedef void (*ring_chat_text_cb_t)(const char *text, size_t len, void *user);

/**
 * @brief Initialise the module (allocates PSRAM buffers, stores config).
 *
 * @param cfg     Session config (keeps pointers — must be static).
 * @param api_key Qianwen API key ("sk-..." or "sk-ws-...").
 */
esp_err_t ring_chat_init(const ring_chat_cfg_t *cfg, const char *api_key);

/** Reset conversation history. */
void ring_chat_reset_history(void);

/**
 * @brief Transcribe 48 kHz mono PCM via STT API.
 *
 * Resamples to 16 kHz internally. The clip duration is
 * samples / 48000 seconds.
 *
 * @param pcm48k  48 kHz / 16-bit / mono PCM samples.
 * @param samples Number of samples in @p pcm48k.
 * @param out     Buffer for the recognised text (NUL-terminated).
 * @param out_cap Capacity of @p out.
 */
esp_err_t ring_chat_stt(const int16_t *pcm48k, size_t samples,
                        char *out, size_t out_cap);

/** Append a user message to the history. */
esp_err_t ring_chat_add_user(const char *text);

/** Append an assistant message to the history. */
esp_err_t ring_chat_add_assistant(const char *text);

/**
 * @brief Non-streaming LLM completion.
 *
 * Sends the full message history (capped by max_history_turns) and
 * stores the reply as an assistant message.
 *
 * @param reply Buffer for the reply text (NUL-terminated).
 * @param cap   Capacity of @p reply.
 */
esp_err_t ring_chat_llm_complete(char *reply, size_t cap);

/**
 * @brief Streaming LLM completion (optional, use in P4+).
 *
 * Calls @p text_cb with each delta as it arrives.
 * The final delta is signalled by a call with len == 0.
 */
esp_err_t ring_chat_llm_stream(ring_chat_text_cb_t text_cb, void *user);

/**
 * @brief Synthesise speech via TTS SSE stream.
 *
 * Each audio chunk is delivered via @p cb. Blocks until the SSE stream
 * finishes (or times out). The callback runs on the calling task.
 */
esp_err_t ring_chat_tts(const char *text, ring_chat_audio_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif