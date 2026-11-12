/*
 * ringkit — GEODIS Voice Agent realtime voice (Azure OpenAI Realtime over WebRTC).
 *
 * Wraps esp_peer plus the GEODIS ephemeral-token signalling and an Opus codec,
 * so an application can do push-to-talk voice without touching esp_peer,
 * esp_http_client or the codec directly. The GEODIS UAT gateway is
 * unauthenticated: a POST /token returns a short-lived `ek_…` token that is
 * exchanged for an Azure OpenAI Realtime SDP answer.
 *
 * Audio format on this interface is **48 kHz / 16-bit / mono PCM** in both
 * directions. Opus encoding/decoding happens inside this module; the SDP
 * negotiates opus/48000/2.
 *
 * Realtime API notes (see notes/geodis-ai-api.md):
 *  - WebRTC is the only transport (the WebSocket route is 404).
 *  - The GA session schema nests audio under `session.audio.input` /
 *    `session.audio.output`. The old top-level `session.turn_detection` and
 *    `session.modalities` are rejected with `unknown_parameter`.
 *  - STT is OFF by default; opt in with `audio.input.transcription`.
 *  - Push-to-talk is `audio.input.turn_detection: null` plus an explicit
 *    `input_audio_buffer.commit` + `response.create` on release.
 *  - The backend does NOT execute the RAG tool call. This module POSTs
 *    /tool-call itself and writes the result back as a function_call_output.
 *  - The assistant transcript event is `response.output_audio_transcript.done`
 *    (Qianwen uses `response.audio_transcript.done`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per uplink frame: 20 ms at 48 kHz. */
#define RING_GEODIS_FRAME_SAMPLES 960

/** Default GEODIS UAT gateway (unauthenticated). */
#define RING_GEODIS_DEFAULT_BASE_URL \
    "https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1"

/** Default Azure OpenAI Realtime SDP exchange (resource geodisopenai). */
#define RING_GEODIS_DEFAULT_SDP_URL \
    "https://geodisopenai.openai.azure.com/openai/v1/realtime/calls"

/** Realtime session lifecycle. Safe to read from any task. */
typedef enum {
    RING_GEODIS_OFF = 0,      /**< not started                                */
    RING_GEODIS_CONNECTING,   /**< Wi-Fi / SNTP / token / SDP / DTLS / session */
    RING_GEODIS_READY,        /**< session configured, uplink closed          */
    RING_GEODIS_LISTENING,    /**< uplink open (button held)                  */
    RING_GEODIS_SPEAKING,     /**< model reply arriving                       */
    RING_GEODIS_ERROR,        /**< connection failed; caller may retry        */
} ring_geodis_state_t;

/** Session configuration — copied into the module by ring_geodis_start(). */
typedef struct {
    const char *base_url;      /**< GEODIS gateway; NULL = default              */
    const char *sdp_url;       /**< Azure SDP exchange; NULL = default         */
    const char *voice;         /**< assistant voice, e.g. "coral"; NULL/"" = default */
    const char *system_prompt; /**< optional persona; sent in the /token body  */
    bool        enable_stt;    /**< true = whisper-1 opt-in (default true)     */
    bool        enable_ptt;    /**< true = turn_detection:null + commit (default true) */
} ring_geodis_cfg_t;

/** State change notification, delivered on the ring_geodis event task. */
typedef void (*ring_geodis_state_cb_t)(ring_geodis_state_t state, void *user);

/**
 * @brief Transcript callback, invoked on the ring_geodis event task.
 *
 * @param text    Fragment text. For model output this is one delta; when
 *                @p is_user is true it is the complete recognised utterance.
 * @param len     Bytes in @p text (never includes the terminator).
 * @param is_user true for the transcribed user utterance, false for model
 *                output.
 * @param user    Opaque pointer passed to ring_geodis_init().
 */
typedef void (*ring_geodis_transcript_cb_t)(const char *text, size_t len,
                                            bool is_user, void *user);

/**
 * @brief Function-calling hook, invoked on the ring_geodis event task.
 *
 * Return true when the application handled the call itself (it must send the
 * function_call_output + response.create); return false to let the module run
 * the built-in RAG execution. May be NULL.
 */
typedef bool (*ring_geodis_tool_cb_t)(const char *name, const char *args_json,
                                      void *user);

/**
 * @brief Initialise the module (Opus codec, buffers, tasks).
 *
 * Call once from app_main after ring_board_init(). The callbacks run on a
 * dedicated task, never on a peer thread, so they may take the LVGL lock.
 */
esp_err_t ring_geodis_init(ring_geodis_state_cb_t state_cb,
                           ring_geodis_transcript_cb_t text_cb,
                           ring_geodis_tool_cb_t tool_cb, void *user);

/**
 * @brief Bring up Wi-Fi + SNTP, fetch a token, then connect the WebRTC session.
 *
 * Blocks until the session is READY or fails. The peer main loop keeps running
 * in its own task afterwards.
 *
 * @param cfg  Session configuration (copied internally; may be a stack object)
 */
esp_err_t ring_geodis_start(const ring_geodis_cfg_t *cfg);

/** Tear down the session. */
esp_err_t ring_geodis_stop(void);

/** Current state (thread safe). */
ring_geodis_state_t ring_geodis_state(void);

/** Static name for a state, for logging/UI. */
const char *ring_geodis_state_name(ring_geodis_state_t state);

/**
 * @brief Open/close the uplink (the talk button).
 *
 * Closing the uplink is what lets the server VAD detect the end of speech in
 * server-VAD mode; in PTT mode the caller then sends ring_geodis_commit().
 */
esp_err_t ring_geodis_set_listening(bool on);

/**
 * @brief Send one Opus frame of 48 kHz / 16-bit / mono PCM.
 *
 * Feed it RING_GEODIS_FRAME_SAMPLES samples at a time (20 ms). Returns
 * ESP_ERR_INVALID_STATE unless the session is LISTENING.
 */
esp_err_t ring_geodis_send_audio(const int16_t *pcm, size_t samples);

/**
 * @brief Read received 48 kHz / 16-bit / mono PCM (Opus-decoded).
 *
 * Blocks up to @p timeout_ms. @p out_samples receives the samples actually
 * read. Returns ESP_OK, or ESP_ERR_TIMEOUT if nothing arrived.
 */
esp_err_t ring_geodis_read_audio(int16_t *pcm, size_t samples, size_t *out_samples,
                                 uint32_t timeout_ms);

/** True while the model is producing a reply. */
bool ring_geodis_is_speaking(void);

/**
 * @brief Push-to-talk: commit the audio buffer and trigger a response.
 *
 * Combines input_audio_buffer.commit + response.create. Only meaningful when
 * enable_ptt is set (turn_detection=null).
 */
esp_err_t ring_geodis_commit(void);

/** Cancel the current response (barge-in). No-op if not speaking. */
esp_err_t ring_geodis_cancel(void);

/** Send an arbitrary JSON event over the DataChannel. */
esp_err_t ring_geodis_send_event(const char *json);

/**
 * @brief Keep an idle session alive.
 *
 * The Azure Realtime session lifetime is undocumented; re-sending
 * session.update is a safe no-op unless READY.
 */
esp_err_t ring_geodis_keepalive(void);

#ifdef __cplusplus
}
#endif
