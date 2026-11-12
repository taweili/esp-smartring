/*
 * ringkit — ring_geodis mock transport.
 *
 * Replaces the network half of ring_geodis when CONFIG_RINGKIT_GEODIS_MOCK is
 * on: no Wi-Fi, no SNTP, no /token, no SDP, no DTLS, no RTP. Everything above
 * the transport is the real code — the same state machine, the same event
 * queue, the same downlink byte ring, the same tool-call task, the same
 * public API. That is the point: an app written against the mock is the app
 * that runs against the gateway.
 *
 * The mock owns one task (`ring-geodis-mock`) which is the stand-in for the
 * peer thread: it is the only thing that pushes downlink audio and emits
 * protocol events, exactly as `ring-geodis-peer` does in a real session.
 *
 * PCM is embedded with EMBED_TXTFILES, so there is no generated C array to
 * review, no compile-time hit for 150 KB of samples, and regenerating the clip
 * is `tools/gen_geodis_mock_clip.py` plus a rebuild.
 */
#include "ring_geodis_mock.h"

#ifdef CONFIG_RINGKIT_GEODIS_MOCK

#include "ring_geodis_priv.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "ring_geodis"

extern const uint8_t geodis_mock_reply_pcm_start[] asm("_binary_geodis_mock_reply_pcm_start");
extern const uint8_t geodis_mock_reply_pcm_end[]   asm("_binary_geodis_mock_reply_pcm_end");

/* ------------------------------------------------------------- timing */

#define MOCK_CONNECT_MS   300
#define MOCK_REPLY_MS     400
#define MOCK_TOOL_MS      600
/* One 20 ms frame of 48 kHz mono, pushed on a 20 ms tick so the receiver's
 * timing looks like a real RTP stream instead of a memory dump. */
#define MOCK_FRAME_MS     20

static ring_geodis_mock_cfg_t mock_cfg;
static TaskHandle_t mock_task;

esp_err_t ring_geodis_mock_config(const ring_geodis_mock_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mock_cfg = *cfg;
    return ESP_OK;
}

size_t ring_geodis_mock_reply_samples(void)
{
    return (size_t)(geodis_mock_reply_pcm_end - geodis_mock_reply_pcm_start) /
           sizeof(int16_t);
}

static uint32_t mock_ms(uint32_t configured, uint32_t fallback)
{
    return configured > 0 ? configured : fallback;
}

static const char *const k_mock_user_line =
    "Où est ma livraison 4457821 ?";
static const char *const k_mock_reply_line =
    "Votre colis est en transit et arrive demain avant midi.";

/* ------------------------------------------------------------- reply playback
 *
 * Called instead of the Opus decoder in a mock session: the canned PCM is
 * already 48 kHz mono, so it goes straight onto the ring the same way
 * ring_geodis_on_audio_data() would put a decoded frame there.
 *
 * Returns early (leaving the rest of the clip unplayed) when the response was
 * cancelled — that is what makes barge-in observable in the mock, and it is
 * the reason cancel() needs no mock-specific hook.
 */
static void mock_play_reply(void)
{
    size_t total = ring_geodis_mock_reply_samples();
    if (mock_cfg.drop_reply_audio) {
        ESP_LOGI(TAG, "mock: reply audio suppressed (%u samples elided)",
                 (unsigned)total);
        return;
    }

    const int16_t *pcm = (const int16_t *)geodis_mock_reply_pcm_start;
    size_t frame = RING_GEODIS_FRAME_SAMPLES;   /* 960 samples = 20 ms */
    for (size_t off = 0; off < total; off += frame) {
        /* Output is a polled interface: stop the moment the app asks for it
         * to, so a barge-in in the mock behaves like one on the wire. */
        if (!ring_geodis_s.speaking) {
            ESP_LOGI(TAG, "mock: reply interrupted at %u/%u samples",
                     (unsigned)off, (unsigned)total);
            return;
        }
        size_t n = (total - off < frame) ? (total - off) : frame;
        ring_geodis_dl_push((const uint8_t *)(pcm + off), n * sizeof(int16_t));
        vTaskDelay(pdMS_TO_TICKS(MOCK_FRAME_MS));
    }
}

/* ------------------------------------------------------------- mock task */

/* `ring-geodis-mock` is the peer-thread stand-in. It waits for a commit,
 * announces the response the way the server does (response.created, then the
 * transcript, then a tool call, then audio), and returns to READY. */
static void mock_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        /* The uplink is opened by the app task; a commit is the turn trigger.
         * Poll instead of using a notification: the mock is allowed to be
         * dumb, and polling costs nothing at a 20 ms cadence. */
        if (!ring_geodis_s.mock_turn_req) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        ring_geodis_s.mock_turn_req = false;

        uint32_t reply_ms = mock_ms(mock_cfg.reply_ms, MOCK_REPLY_MS);
        vTaskDelay(pdMS_TO_TICKS(reply_ms));

        /* The user's utterance arrives transcribed before the reply does. */
        ring_geodis_emit_text(k_mock_user_line, strlen(k_mock_user_line),
                              true, true);

        ring_geodis_s.speaking = true;
        ring_geodis_state_set(RING_GEODIS_SPEAKING);

        /* A reply that needs the RAG. The app's tool_cb sees this exactly as
         * it would see a real function call, and the module's own tool task
         * runs the lookup (which fails offline, and reports that honestly). */
        uint32_t tool_ms = mock_ms(mock_cfg.tool_ms, MOCK_TOOL_MS);
        if (tool_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(tool_ms));
            ring_geodis_emit_text("Je vérifie votre colis…", 22, false, false);
        }

        mock_play_reply();

        /* response.done: the turn ends, the uplink flag is unaffected (the
         * model finishing does not close the button). */
        ring_geodis_s.speaking = false;
        if (ring_geodis_state() == RING_GEODIS_SPEAKING ||
            ring_geodis_state() == RING_GEODIS_READY) {
            ring_geodis_state_set(RING_GEODIS_READY);
        }
        ring_geodis_emit_text(k_mock_reply_line, strlen(k_mock_reply_line),
                              false, true);
        ESP_LOGI(TAG, "mock: turn complete");
    }
}

/* ------------------------------------------------------------- transport API
 *
 * ring_geodis_signaling.c is not compiled in a mock build; these are the
 * symbols it would have provided. Everything else (send_event, commit,
 * cancel, the tool task) is the real code, unchanged. */

esp_err_t ring_geodis_peer_open(void)
{
    /* Nothing to open: the mock task is the "peer". Created once, here, so a
     * stop()/start() cycle does not churn a task for nothing. */
    if (mock_task == NULL) {
        if (xTaskCreate(mock_task_entry, "ring-geodis-mock",
                        RING_GEODIS_SIGNAL_TASK_STACK, NULL,
                        RING_GEODIS_PEER_TASK_PRIO, &mock_task) != pdPASS) {
            mock_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t ring_geodis_peer_close(void)
{
    /* Keep the task: it idles on a 20 ms poll and holds no per-session state.
     * Clearing the turn request is what makes a cancelled turn stay cancelled. */
    ring_geodis_s.mock_turn_req = false;
    return ESP_OK;
}

/* The signal task has nothing to do without a token/SDP exchange. When the
 * mock is on, start() never notifies it (peer_connect goes straight to
 * READY), so it only has to exist for ring_geodis_init() to create. */
void ring_geodis_signal_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

esp_err_t ring_geodis_fetch_token(void)
{
    snprintf(ring_geodis_s.token, sizeof(ring_geodis_s.token), "mock-token");
    snprintf(ring_geodis_s.session_id, sizeof(ring_geodis_s.session_id),
             "mock-session");
    return ESP_OK;
}

/* start() calls ring_geodis_peer_connect() directly; in a mock build that is
 * this hook instead of esp_peer_new_connection(). */
esp_err_t ring_geodis_peer_connect(void)
{
    uint32_t connect_ms = mock_ms(mock_cfg.connect_ms, MOCK_CONNECT_MS);
    ESP_LOGI(TAG, "mock: session coming up (%u ms)", (unsigned)connect_ms);
    vTaskDelay(pdMS_TO_TICKS(connect_ms));
    ring_geodis_s.send_stream_id = 1;
    ring_geodis_s.peer_ready = true;
    ring_geodis_state_set(RING_GEODIS_READY);
    ESP_LOGI(TAG, "mock: session ready (voice %s, %s, %s)",
             ring_geodis_s.voice,
             ring_geodis_s.enable_ptt ? "push-to-talk" : "server VAD",
             ring_geodis_s.enable_stt ? "stt on" : "stt off");
    return ESP_OK;
}

esp_err_t ring_geodis_peer_disconnect(void)
{
    ring_geodis_s.mock_turn_req = false;
    ring_geodis_s.peer_ready = false;
    return ESP_OK;
}

/* Called by the real ring_geodis_send_event() after it has handed the event
 * to the peer. In a mock build there is no peer, so this is where a commit or
 * a cancel becomes a turn. */
void ring_geodis_transport_on_event(const char *json, esp_err_t sent)
{
    if (json == NULL || sent != ESP_OK) {
        return;
    }
    if (strstr(json, "input_audio_buffer.commit") != NULL ||
        strstr(json, "response.create") != NULL) {
        ring_geodis_s.mock_turn_req = true;
    }
    ESP_LOGD(TAG, "mock dc out: %.80s", json);
}

/* The RAG lookup needs a network. Offline, the honest answer is a failure the
 * model can speak ("the lookup is unavailable, try again") — which is exactly
 * what the real path produces when the gateway is down, so the app sees the
 * same behaviour with and without the mock. */
esp_err_t ring_geodis_post_json_ex(const char *url, const char *body,
                                   int expect_status, char *resp, size_t resp_cap,
                                   int *resp_len, int *status, int attempts)
{
    (void)url;
    (void)body;
    (void)expect_status;
    (void)attempts;
    if (resp != NULL && resp_cap > 0) {
        resp[0] = '\0';
    }
    if (resp_len != NULL) {
        *resp_len = 0;
    }
    if (status != NULL) {
        *status = 0;
    }
    ESP_LOGW(TAG, "mock: /tool-call is offline, reporting lookup_unavailable");
    return ESP_FAIL;
}

esp_err_t ring_geodis_post_json(const char *url, const char *body,
                                int expect_status, char *resp, size_t resp_cap,
                                int *resp_len, int *status)
{
    return ring_geodis_post_json_ex(url, body, expect_status, resp, resp_cap,
                                    resp_len, status, RING_GEODIS_TOKEN_ATTEMPTS);
}

#endif /* CONFIG_RINGKIT_GEODIS_MOCK */