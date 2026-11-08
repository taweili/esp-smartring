/*
 * ringkit — ring_net internal interface.
 *
 * Private to the ringkit component. ring_net is split across four translation
 * units: ring_net.c (public API, state, tasks), ring_net_signaling.c (SDP /
 * HTTPS), ring_net_codec.c (Opus) and ring_net_protocol.c (DataChannel JSON).
 * This header is the shared contract between them.
 *
 * Not part of the public API — applications include ringkit/ring_net.h only.
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

    QueueHandle_t            event_queue;
    SemaphoreHandle_t        state_mutex;
    volatile bool            speaking;

    /* RTP presentation timestamp for the current session (sample count). */
    uint32_t                 pts;
} ring_net_ctx_t;

extern ring_net_ctx_t ring_net_s;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_net.c unless noted. */

/* Update the state and notify the event task (safe from any task). */
void ring_net_state_set(ring_net_state_t st);

/* Push decoded downlink PCM onto the ring (defined in ring_net_codec.c). */
void ring_net_dl_push(const uint8_t *pcm, size_t len);

/* Open the Opus encoder/decoder (defined in ring_net_codec.c). */
esp_err_t ring_net_opus_open(void);

/* Peer bring-up (defined in ring_net_signaling.c). */
esp_err_t ring_net_peer_open(void);

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
