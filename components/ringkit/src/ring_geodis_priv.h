/*
 * ringkit — ring_geodis internal interface.
 *
 * Private to the ringkit component. ring_geodis is split across four
 * translation units: ring_geodis.c (public API, state, tasks),
 * ring_geodis_signaling.c (token + SDP / HTTPS), ring_geodis_codec.c (Opus)
 * and ring_geodis_protocol.c (DataChannel JSON + client-executed tool call).
 * This header is the shared contract between them.
 *
 * Not part of the public API — applications include ringkit/ring_geodis.h only.
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
#include "ringkit/ring_geodis.h"

/* ------------------------------------------------------------- config */

/* The endpoint only accepts Opus on m=audio, so this is fixed at 48 kHz.
 * RING_GEODIS_FRAME_SAMPLES (960 = 20 ms) comes from the public header. */
#define RING_GEODIS_SAMPLE_RATE    48000
#define RING_GEODIS_CHANNELS       1   /* PCM on the public API is mono */
/* esp_peer offers `opus/48000/2` for Opus no matter what audio_info.channel
 * says, so the encoder must produce stereo to match the negotiated SDP;
 * ring_geodis_send_audio() up-mixes the mono API to stereo. */
#define RING_GEODIS_OPUS_CHANNELS  2
#define RING_GEODIS_FRAME_BYTES    (RING_GEODIS_FRAME_SAMPLES * 2)   /* 16-bit */
#define RING_GEODIS_BITRATE        32000

/* Downlink PCM ring buffer: 4 s of 48 kHz / 16-bit / mono = 384 KB (PSRAM). */
#define RING_GEODIS_DL_BYTES       (RING_GEODIS_SAMPLE_RATE * 2 * 4)

/* Internal RAM is scarce (250 Hz IMU task + LVGL panel buffers live there),
 * so keep our tasks lean. TLS work happens in the esp-tls task, not here. */
/* esp_peer_main_loop also runs the DTLS handshake, which is stack-hungry
 * (mbedTLS). 16 KB is sized from the observed high-water mark. */
#define RING_GEODIS_PEER_TASK_STACK        16384
#define RING_GEODIS_PEER_TASK_PRIO         5
#define RING_GEODIS_SIGNAL_TASK_STACK      4096
#define RING_GEODIS_SIGNAL_TASK_PRIO       4
/* Runs the app callback, which takes the LVGL lock and renders labels with
 * the 48 px font; 3.5 KB was tight. */
#define RING_GEODIS_EVENT_TASK_STACK       6144
#define RING_GEODIS_EVENT_TASK_PRIO        4
/* The tool-call task runs a blocking HTTPS POST to the GEODIS gateway; give it
 * room for TLS. */
#define RING_GEODIS_TOOL_TASK_STACK        12288
#define RING_GEODIS_TOOL_TASK_PRIO         4

#define RING_GEODIS_SDP_MAX_BYTES          4096
#define RING_GEODIS_HTTP_TIMEOUT_MS        10000
#define RING_GEODIS_SNTP_TIMEOUT_MS        15000
#define RING_GEODIS_SESSION_TIMEOUT_MS     20000

#define RING_GEODIS_BASE_URL_MAX           192
#define RING_GEODIS_SDP_URL_MAX            192
#define RING_GEODIS_VOICE_MAX              32
#define RING_GEODIS_PROMPT_MAX             512
#define RING_GEODIS_TOKEN_MAX              128
#define RING_GEODIS_SESSION_ID_MAX         64

/* ------------------------------------------------------------- state */

typedef struct {
    ring_geodis_state_t         state;
    ring_geodis_state_cb_t      state_cb;
    ring_geodis_transcript_cb_t text_cb;
    ring_geodis_tool_cb_t       tool_cb;
    void                       *user;

    esp_peer_handle_t           peer;
    TaskHandle_t                peer_task;
    TaskHandle_t                signal_task;
    TaskHandle_t                event_task;

    char                        base_url[RING_GEODIS_BASE_URL_MAX];
    char                        sdp_url[RING_GEODIS_SDP_URL_MAX];
    char                        voice[RING_GEODIS_VOICE_MAX];
    char                        system_prompt[RING_GEODIS_PROMPT_MAX];
    bool                        enable_stt;
    bool                        enable_ptt;

    /* Ephemeral token from POST /token. The session_id is the Realtime
     * session the tool-call POST must address; chat_session_id is the id we
     * generated for transcript persistence. */
    char                        token[RING_GEODIS_TOKEN_MAX];
    char                        session_id[RING_GEODIS_SESSION_ID_MAX];
    char                        chat_session_id[RING_GEODIS_SESSION_ID_MAX];

    /* SCTP stream the server's event channel uses. */
    uint16_t                    send_stream_id;

    /* Local Offer handed from the peer callback to the signalling task. */
    char                        local_sdp[RING_GEODIS_SDP_MAX_BYTES];
    SemaphoreHandle_t           sdp_mutex;

    /* Opus codec. Encoder runs on the app's audio task, decoder on the peer
     * thread, hence separate handles and a lock around the decoder. */
    esp_audio_enc_handle_t      enc;
    int                         enc_in_size;
    int                         enc_out_size;
    uint8_t                    *enc_out;
    uint8_t                    *enc_in;      /* mono -> stereo up-mix buffer */

    esp_audio_dec_handle_t      dec;
    SemaphoreHandle_t           dec_mutex;
    uint8_t                    *dec_out;
    int                         dec_out_size;
    uint8_t                    *dec_mono;    /* stereo decode -> mono fold */

    /* Downlink PCM ring buffer (48 kHz mono). */
    ring_byte_ring_t            dl_ring;

    QueueHandle_t               event_queue;
    SemaphoreHandle_t           state_mutex;
    volatile bool               speaking;

    /* RTP presentation timestamp for the current session (sample count). */
    uint32_t                    pts;

    /* Client-executed RAG tool call. The DataChannel callback only queues a
     * job; the tool task does the blocking HTTPS POST. */
    QueueHandle_t               tool_queue;
    TaskHandle_t                tool_task;
} ring_geodis_ctx_t;

extern ring_geodis_ctx_t ring_geodis_s;

/* One queued tool call. */
typedef struct {
    char call_id[64];
    char name[64];
    char args_json[1024];
} ring_geodis_tool_job_t;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_geodis.c unless noted. */

/* Update the state and notify the event task (safe from any task). */
void ring_geodis_state_set(ring_geodis_state_t st);

/* Push decoded downlink PCM onto the ring (defined in ring_geodis_codec.c). */
void ring_geodis_dl_push(const uint8_t *pcm, size_t len);

/* Open the Opus encoder/decoder (defined in ring_geodis_codec.c). */
esp_err_t ring_geodis_opus_open(void);

/* Peer bring-up and token fetch (defined in ring_geodis_signaling.c). */
esp_err_t ring_geodis_peer_open(void);
esp_err_t ring_geodis_fetch_token(void);

/* HTTPS helpers (defined in ring_geodis_signaling.c). */
esp_err_t ring_geodis_post_json(const char *url, const char *body,
                                int expect_status, char *resp, size_t resp_cap,
                                int *resp_len, int *status);
esp_err_t ring_geodis_post_sdp(const char *url, const char *token,
                               const char *offer, char *answer, size_t answer_cap);

/* Task entry points (defined where their work lives). */
void ring_geodis_signal_task(void *arg);   /* ring_geodis_signaling.c */
void ring_geodis_peer_task(void *arg);     /* ring_geodis_signaling.c */
void ring_geodis_tool_task(void *arg);     /* ring_geodis_protocol.c */

/* esp_peer callbacks. */
int ring_geodis_on_state(esp_peer_state_t state, void *ctx);
int ring_geodis_on_msg(esp_peer_msg_t *msg, void *ctx);
int ring_geodis_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx);
int ring_geodis_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx); /* codec */
int ring_geodis_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx);
int ring_geodis_on_data_frame(esp_peer_data_frame_t *frame, void *ctx);
