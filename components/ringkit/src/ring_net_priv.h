/*
 * ringkit — ring_net internal interface.
 *
 * Private to the ringkit component. ring_net is split across four translation
 * units: ring_net.c (public API, state, tasks), ring_net_signaling.c (SDP /
 * HTTPS), ring_net_codec.c (Opus) and ring_net_protocol.c (DataChannel JSON).
 * This header is the shared contract between them.
 *
 * Not part of the public API — applications include ringkit/ring_net.h only.
 *
 * Ownership model (mirrors ring_geodis):
 *  - `ring-net-peer` is the ONLY task that calls esp_peer_open()/close().
 *    Everybody else touches the handle under peer_lock and asks for teardown
 *    via peer_close_req, waiting on peer_gone. Without this, stop()/retry
 *    frees the DTLS/SRTP state while esp_peer_main_loop() is inside it.
 *  - The peer task may only ever *enqueue* events; application callbacks run
 *    on `ring-net-event`, which owns the LVGL lock.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_audio_dec.h"
#include "esp_audio_enc.h"
#include "esp_peer.h"

#include "ringkit/ring_byte_ring.h"
#include "ringkit/ring_net.h"

/* ------------------------------------------------------------- config */

#ifndef RING_NET_HOST
#define RING_NET_HOST "maas.qianwenaiapi.com"
#endif

/* The endpoint only accepts Opus on m=audio, so this is fixed at 48 kHz.
 * RING_NET_FRAME_SAMPLES (960 = 20 ms) comes from the public header. */
#define RING_NET_SAMPLE_RATE    48000
#define RING_NET_CHANNELS       1   /* PCM on the public API is mono */
/* esp_peer offers `opus/48000/2` for Opus no matter what audio_info.channel
 * says, so the encoder must produce stereo to match the negotiated SDP;
 * ring_net_send_audio() up-mixes the mono API to stereo. */
#define RING_NET_OPUS_CHANNELS  2
#define RING_NET_FRAME_BYTES    (RING_NET_FRAME_SAMPLES * 2)   /* 16-bit */
#define RING_NET_BITRATE        32000

/* Downlink PCM ring buffer: 4 s of 48 kHz / 16-bit / mono = 384 KB (PSRAM). */
#define RING_NET_DL_BYTES       (RING_NET_SAMPLE_RATE * 2 * 4)

/* Internal RAM is scarce (250 Hz IMU task + LVGL panel buffers live there),
 * so keep our tasks lean. TLS work happens in the esp-tls task, not here. */
/* esp_peer_main_loop also runs the DTLS handshake, which is stack-hungry
 * (mbedTLS). 16 KB is sized from the observed high-water mark. */
#define RING_NET_PEER_TASK_STACK        16384
#define RING_NET_PEER_TASK_PRIO         5
#define RING_NET_SIGNAL_TASK_STACK      4096
#define RING_NET_SIGNAL_TASK_PRIO       4
/* Runs the app callback, which takes the LVGL lock and renders labels with
 * the 48 px font; 3.5 KB was tight. */
#define RING_NET_EVENT_TASK_STACK       6144
#define RING_NET_EVENT_TASK_PRIO        4

#define RING_NET_SDP_MAX_BYTES          4096
#define RING_NET_HTTP_TIMEOUT_MS        10000
#define RING_NET_SNTP_TIMEOUT_MS        15000
#define RING_NET_SESSION_TIMEOUT_MS     20000
/* A peer that will not let go within this window is wedged; start()/stop()
 * report it rather than blocking the caller for ever. */
#define RING_NET_PEER_CLOSE_TIMEOUT_MS  3000

/* ------------------------------------------------------------- events
 *
 * Everything the application is told arrives as one of these, on the
 * `ring-net-event` task. The queue is the ordering: a burst of
 * READY -> LISTENING -> SPEAKING is three events, not one overwritten cell.
 * The peer task may only ever enqueue; it never calls application code.
 */

#define RING_NET_EV_TEXT_MAX  256
#define RING_NET_EVENT_DEPTH  24

typedef enum {
    RING_NET_EV_STATE = 0,
    RING_NET_EV_TEXT,
} ring_net_ev_kind_t;

typedef struct {
    uint8_t             kind;      /* ring_net_ev_kind_t */
    uint8_t             is_user;
    uint8_t             final;
    uint8_t             clipped;
    ring_net_state_t    state;     /* EV_STATE */
    uint16_t            len;       /* EV_TEXT */
    char                text[RING_NET_EV_TEXT_MAX];
} ring_net_event_t;

/* ------------------------------------------------------------- state */

typedef struct {
    ring_net_state_t         state;
    ring_net_state_cb_t      state_cb;
    ring_net_transcript_cb_t text_cb;
    void                    *user;

    esp_peer_handle_t        peer;
    TaskHandle_t             peer_task;
    TaskHandle_t             signal_task;
    TaskHandle_t             event_task;

    char                     model[64];
    char                     api_key[128];

    /* Peer ownership. Only `ring-net-peer` calls esp_peer_open()/
     * esp_peer_close(); everybody else uses the handle under peer_lock and
     * requests teardown via peer_close_req. */
    SemaphoreHandle_t        peer_lock;
    SemaphoreHandle_t        peer_gone;
    volatile bool            peer_close_req;
    /* Set once esp_peer_new_connection() has returned: the main loop must not
     * run against a connection that is still being built. */
    volatile bool            peer_ready;

    /* Bumped per session. The signal task stamps its POST with the generation
     * it started under and drops the answer if a new session began meanwhile. */
    volatile uint32_t        session_gen;
    esp_err_t                last_err;

    /* SCTP stream the server's event channel ('txt') uses. The server pushes
     * session events on its own channel and expects replies on the same one,
     * so sends must carry this id (esp_peer defaults to stream 0, which is
     * not a channel the server opened and makes esp_peer_send_data fail). */
    uint16_t                 send_stream_id;

    /* Local Offer handed from the peer callback to the signalling task. */
    char                     local_sdp[RING_NET_SDP_MAX_BYTES];
    SemaphoreHandle_t        sdp_mutex;

    /* Opus codec. Encoder runs on the app's audio task, decoder on the peer
     * thread, hence separate handles and a lock around the decoder. */
    esp_audio_enc_handle_t   enc;
    int                      enc_in_size;
    int                      enc_out_size;
    uint8_t                 *enc_out;
    uint8_t                 *enc_in;      /* mono -> stereo up-mix buffer */

    esp_audio_dec_handle_t   dec;
    SemaphoreHandle_t        dec_mutex;
    uint8_t                 *dec_out;
    int                      dec_out_size;
    /* Stereo decode -> mono fold scratch (the ring's audio path is mono). */
    uint8_t                 *dec_mono;

    /* Downlink PCM ring buffer (48 kHz mono). */
    ring_byte_ring_t         dl_ring;

    QueueHandle_t            event_queue;   /* ring_net_event_t */
    SemaphoreHandle_t        state_mutex;
    volatile bool            speaking;

    /* RTP presentation timestamp for the current session (sample count). */
    uint32_t                 pts;
} ring_net_ctx_t;

extern ring_net_ctx_t ring_net_s;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_net.c unless noted. */

/* Update the state and queue a transition for the event task. Safe from any
 * task, including the peer task; never calls application code. */
void ring_net_state_set(ring_net_state_t st);

/* Queue a transcript fragment for the event task. Called on the peer task. */
void ring_net_emit_text(const char *text, size_t len, bool is_user, bool final,
                        bool clipped);

/* Push decoded downlink PCM onto the ring (defined in ring_net_codec.c). */
void ring_net_dl_push(const uint8_t *pcm, size_t len);

/* Open the Opus encoder/decoder (defined in ring_net_codec.c). */
esp_err_t ring_net_opus_open(void);

/* Peer bring-up and SDP exchange (defined in ring_net_signaling.c). */
esp_err_t ring_net_peer_open(void);
esp_err_t ring_net_peer_close(void);
esp_err_t ring_net_peer_connect(void);
esp_err_t ring_net_peer_disconnect(void);

/* Task entry points (defined where their work lives). */
void ring_net_signal_task(void *arg);   /* ring_net_signaling.c */
void ring_net_peer_task(void *arg);     /* ring_net_signaling.c */

/* esp_peer callbacks. */
int ring_net_on_state(esp_peer_state_t state, void *ctx);
int ring_net_on_msg(esp_peer_msg_t *msg, void *ctx);
int ring_net_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx);
int ring_net_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx); /* codec */
int ring_net_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx);
int ring_net_on_data_frame(esp_peer_data_frame_t *frame, void *ctx);
