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
/* Covers the worst case the signal task can take before it gives up: two SDP
 * attempts at HTTP_TIMEOUT_MS plus backoff, then DTLS and session.update. If
 * this is shorter than the signalling budget, ring_geodis_start() returns a
 * timeout for a session that is still coming up. */
#define RING_GEODIS_SESSION_TIMEOUT_MS     25000
/* The signal task's SDP POST budget: attempts x HTTP_TIMEOUT_MS must stay
 * under SESSION_TIMEOUT_MS. The gateway flakes ~1 in 12 connections, so two
 * attempts (1 in 144) is the trade-off between resilience and a start() that
 * blocks for a minute. */
#define RING_GEODIS_SDP_ATTEMPTS           2
#define RING_GEODIS_TOKEN_ATTEMPTS         3
/* A peer that will not let go within this window is wedged; start()/stop()
 * report it rather than blocking the caller for ever. */
#define RING_GEODIS_PEER_CLOSE_TIMEOUT_MS  3000

#define RING_GEODIS_BASE_URL_MAX           192
#define RING_GEODIS_SDP_URL_MAX            192
#define RING_GEODIS_VOICE_MAX              32
#define RING_GEODIS_PROMPT_MAX             512
#define RING_GEODIS_TOKEN_MAX              128
#define RING_GEODIS_SESSION_ID_MAX         64

/* ------------------------------------------------------------- events
 *
 * Everything the application is told arrives as one of these, on the
 * `ring-geodis-event` task. The queue is the ordering: a state transition and
 * the fragment that followed it cannot be delivered out of order, and a burst
 * of RING_GEODIS_READY -> LISTENING -> SPEAKING is three events, not one
 * overwritten cell.
 *
 * The peer task may only ever *enqueue*; it never calls application code.
 */

#define RING_GEODIS_EV_TEXT_MAX  192   /* == RING_GEODIS_TEXT_MAX */
#define RING_GEODIS_EVENT_DEPTH  24

typedef enum {
    RING_GEODIS_EV_STATE = 0,
    RING_GEODIS_EV_TEXT,
} ring_geodis_ev_kind_t;

typedef struct {
    uint8_t             kind;      /* ring_geodis_ev_kind_t */
    uint8_t             is_user;
    uint8_t             final;
    uint8_t             clipped;
    ring_geodis_state_t state;     /* EV_STATE */
    uint16_t            len;       /* EV_TEXT  */
    char                text[RING_GEODIS_EV_TEXT_MAX];
} ring_geodis_event_t;

/* ------------------------------------------------------------- state */

typedef struct {
    ring_geodis_state_t         state;
    ring_geodis_state_cb_t      state_cb;
    ring_geodis_transcript_cb_t text_cb;
    ring_geodis_tool_cb_t       tool_cb;
    ring_geodis_tool_done_cb_t  tool_done_cb;
    void                       *user;
    bool                        inited;

    esp_peer_handle_t           peer;
    TaskHandle_t                peer_task;
    TaskHandle_t                signal_task;
    TaskHandle_t                event_task;

    /* Peer ownership. Only `ring-geodis-peer` calls esp_peer_open()/
     * esp_peer_close(); everybody else uses the handle under peer_lock and
     * requests teardown via peer_close_req. Without this, stop() frees the
     * DTLS/SRTP state while the peer task is inside esp_peer_main_loop(). */
    SemaphoreHandle_t           peer_lock;
    SemaphoreHandle_t           peer_gone;
    volatile bool               peer_close_req;
    /* Set once esp_peer_new_connection() has returned: the main loop must not
     * run against a connection that is still being built. */
    volatile bool               peer_ready;

    /* Bumped per session. The signal task stamps its POST with the generation
     * it started under and drops the answer if a new session began meanwhile —
     * otherwise a slow /token+SDP round trip installs an answer for a peer that
     * no longer exists. */
    volatile uint32_t           session_gen;
    esp_err_t                   last_err;

    /* Diagnostics (ring_geodis_get_stats). Written from the start, signal,
     * peer/mock, audio and tool tasks; read as "last known". */
    ring_geodis_stats_t         stats;
    uint32_t                    commit_ms;
    volatile bool               commit_pending;

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

    QueueHandle_t               event_queue;   /* ring_geodis_event_t */
    SemaphoreHandle_t           state_mutex;
    /* Two independent facts: whether the app has the uplink open (the button)
     * and what the model is doing (the state). They overlap — the model can
     * start answering mid-hold — so gating send_audio on the state would kill
     * the uplink at the worst moment. */
    volatile bool               uplink_open;
    volatile bool               speaking;

    /* RTP presentation timestamp for the current session (sample count). */
    uint32_t                    pts;

    /* Mock transport (CONFIG_RINGKIT_GEODIS_MOCK only): commit() sets this and
     * the mock task turns it into a canned reply. Always present so the two
     * builds share one context layout. */
    volatile bool               mock_turn_req;

    /* Client-executed RAG tool call. The peer task only queues a job; the tool
     * task runs the app hook and the blocking HTTPS POST. */
    QueueHandle_t               tool_queue;
    TaskHandle_t                tool_task;
} ring_geodis_ctx_t;

extern ring_geodis_ctx_t ring_geodis_s;

/* One queued tool call. */
#define RING_GEODIS_TOOL_ARGS_MAX 1024

typedef struct {
    uint8_t bad_args;                    /* arguments present but unreadable */
    char    call_id[64];
    char    name[64];
    char    args_json[RING_GEODIS_TOOL_ARGS_MAX];
} ring_geodis_tool_job_t;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_geodis.c unless noted. */

/* Monotonic millisecond tick for diagnostics (wrap-safe comparisons live in
 * ring_geodis.c). */
uint32_t ring_geodis_now_ms(void);

/* Update the state and queue a transition for the event task. Safe from any
 * task, including the peer task; never calls application code. */
void ring_geodis_state_set(ring_geodis_state_t st);

/* Queue a transcript fragment for the event task. Called on the peer task. */
void ring_geodis_emit_text(const char *text, size_t len, bool is_user, bool final,
                           bool clipped);

/* Push decoded downlink PCM onto the ring (defined in ring_geodis_codec.c). */
void ring_geodis_dl_push(const uint8_t *pcm, size_t len);

/* Feed a synthetic DataChannel frame through the real protocol handler.
 * Used by the mock transport and host tests. */
void ring_geodis_dispatch_data_frame(const char *json);

/* Open the Opus encoder/decoder (defined in ring_geodis_codec.c). */
esp_err_t ring_geodis_opus_open(void);

/* Peer bring-up and token fetch (defined in ring_geodis_signaling.c).
 * ring_geodis_peer_open() must only be called once the previous peer is gone
 * (ring_geodis_peer_close() returned).
 *
 * With CONFIG_RINGKIT_GEODIS_MOCK, ring_geodis_mock.c supplies these three
 * plus ring_geodis_send_event instead, and the build compiles exactly one of
 * the two translation units. */
esp_err_t ring_geodis_peer_open(void);
esp_err_t ring_geodis_peer_close(void);
esp_err_t ring_geodis_fetch_token(void);

/* open/close a session inside the peer. The real implementation wraps
 * esp_peer_new_connection()/esp_peer_disconnect(); the mock just moves the
 * state machine along. */
esp_err_t ring_geodis_peer_connect(void);
esp_err_t ring_geodis_peer_disconnect(void);

/* Exactly one of these is linked: ring_geodis_codec.c encodes + sends over
 * esp_peer, ring_geodis_mock.c accepts and discards. */
esp_err_t ring_geodis_uplink_send(const int16_t *pcm, size_t samples);

/* Called by ring_geodis_send_event() after it has handed a DataChannel event
 * to the peer. A no-op in a real build; the mock uses it to see commit and
 * cancel, which is all a mocked server needs to react to. */
void ring_geodis_transport_on_event(const char *json, esp_err_t sent);

/* HTTPS helpers (defined in ring_geodis_signaling.c). */
esp_err_t ring_geodis_post_json(const char *url, const char *body,
                                int expect_status, char *resp, size_t resp_cap,
                                int *resp_len, int *status);
/* Same, with an explicit attempt count: the tool call is on the user's
 * critical path, so it retries less aggressively than the token fetch. */
esp_err_t ring_geodis_post_json_ex(const char *url, const char *body,
                                   int expect_status, char *resp, size_t resp_cap,
                                   int *resp_len, int *status, int attempts);
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
