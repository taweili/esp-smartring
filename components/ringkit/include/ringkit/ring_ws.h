/*
 * ringkit — WebSocket realtime voice (Qianwen AI platform).
 *
 * Wraps esp_websocket_client plus the Qianwen session protocol and PCM
 * resampling/base64 transport, so an application can do push-to-talk or
 * hands-free voice without touching the WS client, base64 or JSON protocol
 * directly.
 *
 * Audio format on this interface is **48 kHz / 16-bit / mono PCM** in both
 * directions. Resampling to/from the endpoint's 16 kHz (input) / 24 kHz
 * (output) happens inside this module.
 *
 * Realtime API notes (see notes/qianwen-ai-platform.md):
 *  - WebSocket supports all three turn modes: server_vad, smart_turn_v2,
 *    and push-to-talk (null). Unlike WebRTC, PTT is possible.
 *  - Session events travel as JSON over the WS text channel; audio is
 *    base64-encoded PCM in the JSON.
 *  - No codec, no DTLS, no Opus — just PCM in JSON over TCP/TLS 443.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Samples per uplink frame: 20 ms at 48 kHz (matches ring_audio cadence). */
#define RING_WS_FRAME_SAMPLES 960

/** Realtime session lifecycle. Safe to read from any task. */
typedef enum {
    RING_WS_OFF = 0,      /**< not started                                   */
    RING_WS_CONNECTING,   /**< Wi-Fi / SNTP / WS / session.update in progress */
    RING_WS_READY,        /**< session configured, mic closed                 */
    RING_WS_LISTENING,    /**< uplink open (push-to-talk)                     */
    RING_WS_SPEAKING,     /**< model reply arriving                           */
    RING_WS_ERROR,        /**< connection failed; caller may retry            */
} ring_ws_state_t;

/** Session configuration — set once at ring_ws_start(). */
typedef struct {
    const char *voice;            /**< "longanqian_v3.1"; IDLE-only          */
    const char *instructions;     /**< system prompt; may be NULL            */
    const char *turn_detection;   /**< "smart_turn_v2" | "server_vad" | NULL = PTT */
    bool        enable_search;
    const char *tools_json;       /**< optional function-calling tools array  */
    const char *input_language;   /**< optional, e.g. "zh" for ASR           */
    const char *output_language;  /**< optional, e.g. "en" for TTS           */
    float       speech_rate;      /**< 0.5..2.0; <=0 = server default        */
    int         max_history_turns;/**< 1..50; 0 = server default (20)        */
} ring_ws_session_cfg_t;

/** Module statistics, reset on ring_ws_start(). */
typedef struct {
    uint32_t    bytes_up;
    uint32_t    bytes_down;
    uint32_t    turns;
    uint32_t    dropped_up;
    uint32_t    dropped_down;
    uint32_t    ring_high_ms;
    const char *turn_mode;
    const char *last_error_type;   /**< last "error" event's type, or "" */
    const char *last_error_param;  /**< last "error" event's param, or "" */
} ring_ws_stats_t;

/** State change notification, delivered on the ring-ws-event task. */
typedef void (*ring_ws_state_cb_t)(ring_ws_state_t state, void *user);

/**
 * @brief Transcript callback, invoked on the ring-ws-event task.
 *
 * @param text    Fragment text. For model output this is one delta; when
 *                @p is_user is true it is the complete recognised utterance.
 * @param len     Bytes in @p text (never includes the terminator).
 * @param is_user true for the transcribed user utterance, false for model.
 * @param final   true when @p text is the complete utterance/reply (the
 *                `.done` / `.completed` events); false for a streaming delta,
 *                which is incremental and should be appended.
 * @param user    Opaque pointer passed to ring_ws_init().
 */
typedef void (*ring_ws_transcript_cb_t)(const char *text, size_t len,
                                        bool is_user, bool final, void *user);

/**
 * @brief Function-calling callback, invoked on the ring-ws-event task.
 *
 * The app should run the tool, then call ring_ws_send_json() with a
 * conversation.item.create (type: function_call_output) + response.create.
 * May be NULL (no function calling).
 */
typedef void (*ring_ws_tool_cb_t)(const char *name, const char *args_json,
                                  void *user);

/**
 * @brief Initialise the module (resampler state, buffers, tasks).
 *
 * Call once from app_main after ring_board_init(). The callbacks run on a
 * dedicated event task, never on the WS client task, so they may take the
 * LVGL lock.
 */
esp_err_t ring_ws_init(ring_ws_state_cb_t state_cb,
                       ring_ws_transcript_cb_t text_cb,
                       ring_ws_tool_cb_t tool_cb,
                       void *user);

/**
 * @brief Bring up Wi-Fi + SNTP, then connect the WebSocket session.
 *
 * Blocks until the session is READY or fails. To reconnect, call
 * ring_ws_stop() then ring_ws_start() again.
 *
 * @param model   Qianwen model id, e.g. "qwen-audio-3.1-realtime-flash"
 * @param api_key Qianwen API key ("sk-...")
 * @param cfg     Session configuration (copied internally)
 */
esp_err_t ring_ws_start(const char *model, const char *api_key,
                        const ring_ws_session_cfg_t *cfg);

/** Tear down the session. Safe to call from any task. */
esp_err_t ring_ws_stop(void);

/** Current state (thread safe). */
ring_ws_state_t ring_ws_state(void);

/** Static name for a state, for logging/UI. */
const char *ring_ws_state_name(ring_ws_state_t state);

/** The turn mode actually negotiated (after fallback): "ptt",
 *  "server_vad", "smart_turn_v2", ... */
const char *ring_ws_turn_mode(void);

/** Copy the stats structure. */
esp_err_t ring_ws_get_stats(ring_ws_stats_t *out);

/**
 * @brief Open/close the mic.
 *
 * In PTT mode, call set_listening(true) to start sending audio,
 * set_listening(false) to stop, then commit() to trigger the response.
 *
 * In server_vad / smart_turn_v2 modes the mic is always logically open;
 * the half-duplex gate inside send_audio() handles the mute during playback.
 */
esp_err_t ring_ws_set_listening(bool on);

/**
 * @brief Send one frame of 48 kHz / 16-bit / mono PCM.
 *
 * Feed it RING_WS_FRAME_SAMPLES (960) samples at a time (20 ms).
 * The half-duplex gate drops frames while the model is replying or recent
 * playback is still in the tail.
 */
esp_err_t ring_ws_send_audio(const int16_t *pcm48k, size_t samples);

/**
 * @brief Read received 48 kHz / 16-bit / mono PCM (resampled from 24 kHz).
 *
 * Blocks up to @p timeout_ms. @p out receives the samples actually read.
 * In hands-free mode, this returns ESP_ERR_TIMEOUT until the playback
 * cushion (~200 ms) is filled.
 */
esp_err_t ring_ws_read_audio(int16_t *pcm48k, size_t samples,
                             size_t *out, uint32_t timeout_ms);

/** True while the model is producing a reply. */
bool ring_ws_is_speaking(void);

/** True when downlink audio is buffered (playback active or catching up). */
bool ring_ws_playback_pending(void);

/** Milliseconds of buffered downlink audio at 24 kHz rate. */
uint32_t ring_ws_buffered_ms(void);

/**
 * @brief Push-to-talk: commit the audio buffer and trigger a response.
 *
 * Combines input_audio_buffer.commit + response.create.
 * Only meaningful in PTT mode (turn_detection=NULL).
 */
esp_err_t ring_ws_commit(void);

/** Manually trigger a response (e.g. after function_call_output). */
esp_err_t ring_ws_response_create(void);

/** Cancel the current response (barge-in). No-op if not speaking. */
esp_err_t ring_ws_cancel(void);

/** Send an arbitrary JSON event over the WebSocket. */
esp_err_t ring_ws_send_json(const char *json);

/**
 * @brief Keep an idle session alive.
 *
 * The Qianwen realtime server closes a session that has not generated a
 * response for 180 s ("response_idle_timeout"). Re-sending session.update
 * resets that timer without producing a reply. No-op unless READY, so it is
 * safe to call on a timer.
 */
esp_err_t ring_ws_keepalive(void);

/** Trust anchors for the Qianwen WS endpoint (same host as ring_net). */
const char *ring_ws_ca_pem(void);
size_t     ring_ws_ca_pem_len(void);

#ifdef __cplusplus
}
#endif