/*
 * ringkit — WebRTC realtime voice (Qianwen AI platform).
 *
 * Wraps esp_peer plus the Qianwen SDP-exchange signaling and an Opus codec, so
 * an application can do push-to-talk voice without touching esp_peer,
 * esp_http_client or the codec directly.
 *
 * Audio format on this interface is **48 kHz / 16-bit / mono PCM** in both
 * directions. Opus encoding/decoding happens inside this module: the Qianwen
 * endpoint accepts only Opus on m=audio.
 *
 * Realtime API notes (see notes/qianwenai.md):
 *  - WebRTC supports only server-side VAD ("server_vad"); manual turn-taking
 *    is WebSocket-only. So the button gates the uplink and the model detects
 *    the end of speech.
 *  - Media travels on the RTP track; model events on a DataChannel.
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
#define RING_NET_FRAME_SAMPLES 960

/** Realtime session lifecycle. Safe to read from any task. */
typedef enum {
    RING_NET_OFF = 0,     /**< not started                                 */
    RING_NET_CONNECTING,  /**< Wi-Fi / SNTP / SDP / DTLS / session in progress */
    RING_NET_READY,       /**< session configured, uplink closed           */
    RING_NET_LISTENING,   /**< uplink open (button held)                   */
    RING_NET_SPEAKING,    /**< model reply arriving                        */
    RING_NET_ERROR,       /**< connection failed; caller may retry         */
} ring_net_state_t;

/** State change notification, delivered on the ring_net event task. */
typedef void (*ring_net_state_cb_t)(ring_net_state_t state, void *user);

/** Text/transcript fragment from the model (also on the event task). */
typedef void (*ring_net_transcript_cb_t)(const char *text, size_t len, void *user);

/**
 * @brief Initialise the module (Opus codec, buffers, tasks).
 *
 * Call once from app_main after ring_board_init(). The callbacks run on a
 * dedicated task, never on a peer thread, so they may take the LVGL lock.
 */
esp_err_t ring_net_init(ring_net_state_cb_t state_cb,
                        ring_net_transcript_cb_t text_cb,
                        void *user);

/**
 * @brief Bring up Wi-Fi + SNTP, then connect the WebRTC session.
 *
 * Blocks until the session is READY or fails. The peer main loop keeps running
 * in its own task afterwards.
 *
 * @param model   Qianwen model id, e.g. "qwen-audio-3.1-realtime-flash"
 * @param api_key Qianwen API key ("sk-...")
 */
esp_err_t ring_net_start(const char *model, const char *api_key);

/** Tear down the session. */
esp_err_t ring_net_stop(void);

/** Current state (thread safe). */
ring_net_state_t ring_net_state(void);

/** Static name for a state, for logging/UI. */
const char *ring_net_state_name(ring_net_state_t state);

/**
 * @brief Open/close the uplink (the talk button).
 *
 * Closing the uplink is what lets the server VAD detect the end of speech.
 */
esp_err_t ring_net_set_listening(bool on);

/**
 * @brief Send one Opus frame of 48 kHz / 16-bit / mono PCM.
 *
 * Feed it RING_NET_FRAME_SAMPLES samples at a time (20 ms). Returns
 * ESP_ERR_INVALID_STATE unless the session is LISTENING.
 */
esp_err_t ring_net_send_audio(const int16_t *pcm, size_t samples);

/**
 * @brief Read received 48 kHz / 16-bit / mono PCM (Opus-decoded).
 *
 * Blocks up to @p timeout_ms. @p out_samples receives the samples actually
 * read. Returns ESP_OK, or ESP_ERR_TIMEOUT if nothing arrived.
 */
esp_err_t ring_net_read_audio(int16_t *pcm, size_t samples, size_t *out_samples,
                              uint32_t timeout_ms);

/** True while the model is producing a reply. */
bool ring_net_is_speaking(void);

/**
 * @brief Send a JSON event over the DataChannel (e.g. a session.update).
 *
 * Returns ESP_ERR_INVALID_STATE if the session is not up.
 */
esp_err_t ring_net_send_event(const char *json);

/**
 * @brief Trust anchors for the Qianwen endpoint.
 *
 * The server sends only its leaf certificate, so the intermediate CA has to be
 * supplied (embedded as assets/qianwen_ca.pem).
 */
const char *ring_net_ca_pem(void);
size_t ring_net_ca_pem_len(void);

#ifdef __cplusplus
}
#endif
