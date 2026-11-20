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

/** Longest transcript fragment delivered to ring_geodis_transcript_cb_t. */
#define RING_GEODIS_TEXT_MAX 192

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
    const char *base_url;      /**< GEODIS gateway; NULL = RING_GEODIS_DEFAULT_BASE_URL */
    const char *sdp_url;       /**< Azure SDP exchange; NULL = RING_GEODIS_DEFAULT_SDP_URL */
    const char *voice;         /**< assistant voice, e.g. "coral"; NULL/"" = "coral"  */
    const char *system_prompt; /**< optional persona; sent in the /token body     */
    int8_t      enable_stt;    /**< -1 = on (default), 0 = off, 1 = on            */
    int8_t      enable_ptt;    /**< -1 = on (default), 0 = off, 1 = on            */
} ring_geodis_cfg_t;

/**
 * @brief Initialise a config to "everything defaulted".
 *
 * Both flags are tri-state (-1 = default) because these two settings have a
 * safe default that a zero-initialised struct would silently invert: without
 * STT there is no user transcript, and without PTT the module leaves server
 * VAD on while the app still calls ring_geodis_commit().
 *
 *     ring_geodis_cfg_t cfg = RING_GEODIS_CFG_DEFAULT();
 *     cfg.system_prompt = "…";
 */
#define RING_GEODIS_CFG_DEFAULT()                          \
    {                                                      \
        .base_url = NULL, .sdp_url = NULL, .voice = NULL,   \
        .system_prompt = NULL, .enable_stt = -1, .enable_ptt = -1, \
    }

/**
 * @brief Per-session diagnostics.
 *
 * Reset by ring_geodis_start(). The fields are written from several tasks and
 * are intended for a status/latency line, not for control flow: read them as
 * "last known", not as a synchronised snapshot.
 */
typedef struct {
    uint32_t session_setup_ms;         /**< start() -> READY                */
    uint32_t token_ms;                 /**< POST /token round trip          */
    uint32_t sdp_ms;                   /**< SDP POST round trip (0: mock)   */
    uint32_t commit_to_first_audio_ms; /**< commit() -> first decoded frame  */
    uint32_t frames_sent;              /**< Opus frames accepted by uplink  */
    uint32_t frames_dropped;           /**< downlink frames lost to overflow */
    uint32_t tool_calls;               /**< RAG lookups dequeued            */
    uint32_t tool_ms;                  /**< last /tool-call POST round trip */
} ring_geodis_stats_t;

/**
 * @brief State change notification, delivered on `ring-geodis-event`.
 *
 * The event task is a 6 KB internal-RAM task that does nothing but walk a
 * queue and call these callbacks, so the LVGL lock may be taken here. Do not
 * block it: it also carries the transcript fragments, and a slow callback
 * costs fragments (the queue is bounded and drops with a warning).
 */
typedef void (*ring_geodis_state_cb_t)(ring_geodis_state_t state, void *user);

/**
 * @brief Transcript callback, invoked on `ring-geodis-event`.
 *
 * @param text    Fragment text, NUL-terminated but use @p len; fragments are
 *                capped at 191 bytes (see RING_GEODIS_TEXT_MAX).
 * @param len     Bytes in @p text (never includes the terminator).
 * @param is_user true for the transcribed user utterance, false for model
 *                output.
 * @param final   true when this fragment completes the line (`…done`, and the
 *                user utterance, which arrives whole). Deltas are false —
 *                append them; a final is what you commit to scrollback.
 * @param clipped true when @p text was truncated to fit the module's event
 *                payload (see RING_GEODIS_TEXT_MAX); a @p final line with this
 *                set is a prefix, not the whole line.
 * @param user    Opaque pointer passed to ring_geodis_init().
 */
typedef void (*ring_geodis_transcript_cb_t)(const char *text, size_t len,
                                            bool is_user, bool final,
                                            bool clipped, void *user);

/**
 * @brief Function-calling hook, invoked on `ring-geodis-tool`.
 *
 * Runs on the module's dedicated tool task (12 KB, never the peer task), just
 * before the RAG lookup — so it is the right place to put the UI into a
 * "looking up…" state. Return true when the application handled the call
 * itself (it must send the function_call_output + response.create); return
 * false to let the module run the built-in RAG execution. May be NULL.
 *
 * The lookup is blocked until this returns, so keep it short and never take a
 * lock that a network operation holds.
 */
typedef bool (*ring_geodis_tool_cb_t)(const char *name, const char *args_json,
                                      void *user);

/**
 * @brief Tool-lookup result, invoked on `ring-geodis-tool`.
 *
 * `tool_cb` signals only the *start* of a lookup; this is how the UI learns
 * that the "looking up…" stage ended and what came back. Only module-executed
 * lookups get one: an app that returns true from tool_cb owns the call and its
 * result. Register with ring_geodis_set_tool_done_cb().
 *
 * @param ok      true when the gateway answered 200 with a parseable result.
 * @param status  HTTP status code, or 0 when no HTTP response was parsed.
 * @param chunks  `retrievedChunks` count, or -1 when unavailable.
 */
typedef void (*ring_geodis_tool_done_cb_t)(const char *name, bool ok,
                                           int status, int chunks, void *user);

/**
 * @brief Initialise the module (Opus codec, buffers, tasks).
 *
 * Call once from app_main after ring_board_init(). Calling it twice is a
 * no-op, not a re-initialisation. Every callback runs on one of the module's
 * own tasks, never on the peer/DTLS task and never on yours.
 */
esp_err_t ring_geodis_init(ring_geodis_state_cb_t state_cb,
                           ring_geodis_transcript_cb_t text_cb,
                           ring_geodis_tool_cb_t tool_cb, void *user);

/**
 * @brief Register the tool-result callback (may be NULL to clear).
 *
 * Uses the @p user pointer passed to ring_geodis_init(). Call after
 * ring_geodis_init() (which clears the context) and before
 * ring_geodis_start(); the callback runs on `ring-geodis-tool`, so it may take
 * the LVGL lock.
 */
esp_err_t ring_geodis_set_tool_done_cb(ring_geodis_tool_done_cb_t cb);

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

/** Last failure code (ESP_OK while the session is healthy). */
esp_err_t ring_geodis_last_err(void);

/**
 * @brief Copy the per-session diagnostics.
 *
 * @param out  Destination; must not be NULL.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG when @p out is NULL.
 */
esp_err_t ring_geodis_get_stats(ring_geodis_stats_t *out);

/**
 * @brief Open/close the uplink (the talk button).
 *
 * Opening it while the model is speaking is a barge-in: the module cancels the
 * response and flushes the decoded-audio buffer, so the ring does not keep
 * talking into its own microphone. Opening/closing is independent of the
 * reported state — the model may start answering while the button is still
 * held, which sets RING_GEODIS_SPEAKING without closing the uplink.
 *
 * Closing the uplink is what lets the server VAD detect the end of speech in
 * server-VAD mode; in PTT mode the caller then sends ring_geodis_commit().
 */
esp_err_t ring_geodis_set_listening(bool on);

/**
 * @brief Send one Opus frame of 48 kHz / 16-bit / mono PCM.
 *
 * Feed it RING_GEODIS_FRAME_SAMPLES samples at a time (20 ms). Returns
 * ESP_ERR_INVALID_STATE unless the uplink is open (ring_geodis_set_listening
 * (true)) and the session connected — note it stays valid while the model is
 * speaking back, which is exactly when a barge-in uplink is open.
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

/**
 * @brief Cancel the current response (barge-in). No-op if not speaking.
 *
 * Drops the decoded audio still queued for playback as well, so the caller
 * does not have to know about the buffer.
 */
esp_err_t ring_geodis_cancel(void);

/** Send an arbitrary JSON event over the DataChannel. */
esp_err_t ring_geodis_send_event(const char *json);

/**
 * @brief Re-assert the session configuration (READY only).
 *
 * Sends session.update again. The Azure session lifetime is undocumented and
 * the `ek_` token is spent at the SDP exchange, so this does not extend
 * anything the way a ping would — it is an idempotent reconfiguration, and the
 * endpoint rejects it while a response is in flight (returns ESP_ERR_INVALID_STATE
 * for that case). Prefer tearing the session down and calling
 * ring_geodis_start() again for real recovery.
 */
esp_err_t ring_geodis_keepalive(void);

#ifdef __cplusplus
}
#endif
