/*
 * ringkit — ring_ws internal interface.
 *
 * Private to the ringkit component. ring_ws is split across three translation
 * units: ring_ws.c (public API, state, tasks), ring_ws_transport.c (WebSocket
 * lifecycle + reassembly, base64, playback) and ring_ws_protocol.c (session /
 * turn JSON, uplink). This header is the shared contract between them.
 *
 * Not part of the public API — applications include ringkit/ring_ws.h only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_websocket_client.h"

#include "ringkit/ring_byte_ring.h"
#include "ringkit/ring_resample.h"
#include "ringkit/ring_ws.h"

/* ------------------------------------------------------------- config */

#ifndef RING_WS_HOST
#define RING_WS_HOST "maas.qianwenaiapi.com"
#endif

/* Endpoint fixed at 16 kHz in / 24 kHz out. The public API stays 48 kHz mono. */
#define RING_WS_IN_RATE   16000
#define RING_WS_OUT_RATE 24000
#define RING_WS_API_RATE 48000
#define RING_WS_FRAME_BYTES (RING_WS_FRAME_SAMPLES * 2)  /* 1920 */

/* Downlink ring: 4 s of 24 kHz / 16-bit / mono = 192 KB (PSRAM, newest-wins). */
#define RING_WS_DL_BYTES  (RING_WS_OUT_RATE * 2 * 4)

/* Playback cushion: don't start playback until this many ms are buffered. */
#define RING_WS_CUSHION_MS 200

/* Half-duplex tail: keep the uplink muted this long after playback ends. */
#define RING_WS_ECHO_TAIL_MS 500

/* Reassembly buffer for fragmented WS messages (PSRAM). */
#define RING_WS_REASM_BYTES  (64 * 1024)

/* Base64/JSON scratch for one inbound delta, and the PCM it decodes to.
 * Sized to the whole reassembly buffer so a delta can never be silently
 * truncated: a truncated base64 blob decodes to garbage audio instead of
 * failing. Both live in PSRAM — never on the worker task's stack (8 KB). */
#define RING_WS_B64_MAX      RING_WS_REASM_BYTES
#define RING_WS_PCM24_MAX    (RING_WS_B64_MAX / 4 * 3 + 8)

/* Uplink scratch: one 20 ms frame at 16 kHz plus its base64 + JSON wrapper.
 * 960 samples / 3 = 320 samples = 640 B -> 856 B base64 -> ~930 B JSON. */
#define RING_WS_UP16_SAMPLES (RING_WS_FRAME_SAMPLES / 3)
#define RING_WS_UP_B64_MAX   (RING_WS_UP16_SAMPLES * 2 * 4 / 3 + 8)
#define RING_WS_UP_JSON_MAX  (RING_WS_UP_B64_MAX + 80)

/* Send timeout: short so the audio task never stalls the WS read loop. */
#define RING_WS_SEND_TIMEOUT_MS  20
/* Session setup timeout. */
#define RING_WS_SESSION_TIMEOUT_MS 20000
/* SNTP sync timeout (TLS needs a plausible clock). */
#define RING_WS_SNTP_TIMEOUT_MS   15000

/* Stuck-response watchdog: if responding stays true this long, force cancel. */
#define RING_WS_RESPONSE_WATCHDOG_MS 20000

/* Tasks: WS client has its own; we add a worker + an event task. */
#define RING_WS_WORKER_STACK  8192
#define RING_WS_WORKER_PRIO   5
#define RING_WS_EVENT_STACK   6144
#define RING_WS_EVENT_PRIO    4

/* ------------------------------------------------------------- messages */

typedef enum {
    RING_WS_MSG_TEXT = 0,   /* assembled JSON text message */
    RING_WS_MSG_LIFECYCLE,  /* connection lifecycle (CONNECTED/DISCONNECTED/ERROR) */
    RING_WS_MSG_STOP,
} ring_ws_msg_kind_t;

typedef struct {
    ring_ws_msg_kind_t kind;
    int   len;       /* valid bytes in data (text) */
    int   err_type;  /* lifecycle: WEBSOCKET_ERROR_TYPE_* or 0 */
} ring_ws_msg_t;

/* ------------------------------------------------------------- state */

typedef struct {
    ring_ws_state_t         state;
    ring_ws_state_cb_t      state_cb;
    ring_ws_transcript_cb_t text_cb;
    ring_ws_tool_cb_t       tool_cb;
    void                   *user;

    esp_websocket_client_handle_t client;

    TaskHandle_t             worker_task;
    TaskHandle_t             event_task;

    char                     model[64];
    char                     api_key[160];
    ring_ws_session_cfg_t    cfg;     /* copied at start */

    char                     turn_mode[24];  /* negotiated mode for UI */

    /* Reassembly: one buffer per active message, owned by the WS task. */
    uint8_t                 *reasm;
    int                      reasm_len;
    int                      reasm_cap;
    SemaphoreHandle_t        reasm_ack;   /* worker finished with ring_ws_s.reasm */

    /* Inbound delta scratch (PSRAM): base64 text + decoded 24 kHz PCM. */
    char                    *b64_text;
    size_t                   b64_cap;
    uint8_t                 *pcm24_scratch;
    size_t                   pcm24_cap;

    /* Uplink scratch (internal RAM). One writer task only (see header). */
    int16_t                  up16[RING_WS_UP16_SAMPLES];
    char                     up_b64[RING_WS_UP_B64_MAX];
    char                     up_json[RING_WS_UP_JSON_MAX];

    /* Worker <-> WS-task queue: pushes ring_ws_msg_t when a message completes. */
    QueueHandle_t            msg_queue;
    /* Lifecycle queue entry scratch (avoid stack alloc on WS task). */
    ring_ws_msg_t            lifecycle_msg;

    /* Downlink PCM ring buffer (24 kHz mono, 192 KB). */
    ring_byte_ring_t         dl_ring;

    /* Resampler state. */
    ring_resample_decim_t    decim;
    ring_resample_interp_t   interp;
    float                    decim_out[RING_WS_FRAME_SAMPLES / 3 + 8]; /* 320 */
    float                    interp_out[RING_WS_FRAME_SAMPLES * 2 + 32]; /* 1920 */

    /* Half-duplex / turn tracking. */
    volatile bool            responding;     /* set on response.created, cleared on response.done */
    volatile bool            speaking;       /* mirror of responding for the app */
    uint32_t                 last_playback_ms;  /* last ring_audio_write time */
    uint32_t                 response_start_ms; /* watchdog */
    uint32_t                 cushion_ms;       /* current cushion target */

    /* Stats. */
    uint32_t                 bytes_up;
    uint32_t                 bytes_down;
    uint32_t                 turns;
    uint32_t                 dropped_up;
    uint32_t                 dropped_down;
    uint32_t                 ring_high_samples;  /* at 24 kHz */
    uint32_t                 buffered_high_ms;

    /* Event marshalling: a single-slot queue of state values. */
    QueueHandle_t            event_queue;
    SemaphoreHandle_t        state_mutex;
} ring_ws_ctx_t;

extern ring_ws_ctx_t ring_ws_s;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_ws.c unless noted. */

uint32_t ring_ws_now_ms(void);
void ring_ws_state_set(ring_ws_state_t st);
void ring_ws_response_watchdog(void);

void ring_ws_dl_push(const uint8_t *pcm, size_t len);
size_t ring_ws_dl_pop(uint8_t *pcm, size_t len);
size_t ring_ws_dl_count_bytes(void);

/* Base64 helpers (defined in ring_ws_transport.c). */
esp_err_t ring_ws_b64_encode(const uint8_t *in, size_t n, char *out,
                             size_t out_cap, size_t *out_len);
int ring_ws_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap);

/* Protocol handlers (defined in ring_ws_protocol.c). */
esp_err_t ring_ws_send_session_update(const char *turn_detection_override);
void ring_ws_handle_audio_delta(const char *json);
void ring_ws_worker_handle_text(const char *json, int len);
void ring_ws_worker_handle_lifecycle(ring_ws_msg_t *m);

/* Tasks / event handler (defined in ring_ws_transport.c). */
void ring_ws_worker_task(void *arg);
void ring_ws_event_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);
