/*
 * ringkit — ring_chat LLM stage: history JSON → HTTPS POST → reply.
 *
 * Split out of ring_chat.c. build_messages() is overflow-checked and escapes
 * the history text; the reply is appended to the history on success.
 * See ring_chat_priv.h.
 */
#include "ring_chat_priv.h"

#include "ringkit/ring_http.h"
#include "ringkit/ring_json.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ring_chat"

/* Append one chat message as a JSON object with a trailing comma.
 * Escapes the control characters that would otherwise break the request.
 * Returns false when the buffer is full. */
static bool append_message(char *buf, size_t cap, int *pos,
                           const char *role, const char *text)
{
    if (!ring_json_append(buf, cap, pos, "{\"role\":\"%s\",\"content\":\"", role)) {
        return false;
    }
    for (const char *p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        char esc_buf[8];
        const char *esc;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", c);
            } else {
                esc_buf[0] = (char)c;
                esc_buf[1] = '\0';
            }
            esc = esc_buf;
            break;
        }
        if (!ring_json_append(buf, cap, pos, "%s", esc)) {
            return false;
        }
    }
    return ring_json_append(buf, cap, pos, "\"},");
}

/* Build a messages JSON array from history, respecting the cap.
 * Returns the length, or -1 when the buffer is too small. */
static int build_messages(char *buf, size_t cap)
{
    int pos = 0;
    int cap_val = ring_chat_s.cfg.max_history_turns > 0 ? ring_chat_s.cfg.max_history_turns
                  : RING_CHAT_MAX_HISTORY_TURNS;
    int start = ring_chat_s.hist.count > cap_val * 2 ? ring_chat_s.hist.count - cap_val * 2 : 0;
    /* Ensure system prompt alignment — if cap implies removing the first turn,
     * remove pairs so user+assistant stays intact. */
    if (start > 0 && (ring_chat_s.hist.count - start) % 2 != 0) start--;

    if (!ring_json_append(buf, cap, &pos, "[")) {
        return -1;
    }
    if (ring_chat_s.cfg.system_prompt &&
        !append_message(buf, cap, &pos, "system", ring_chat_s.cfg.system_prompt)) {
        return -1;
    }
    for (int i = start; i < ring_chat_s.hist.count; i++) {
        if (ring_chat_s.hist.user_msgs[i] &&
            !append_message(buf, cap, &pos, "user", ring_chat_s.hist.user_msgs[i])) {
            return -1;
        }
        if (ring_chat_s.hist.assistant_msgs[i] &&
            !append_message(buf, cap, &pos, "assistant", ring_chat_s.hist.assistant_msgs[i])) {
            return -1;
        }
    }
    /* Replace trailing comma with ] */
    if (pos > 1 && buf[pos - 1] == ',') {
        pos--;
    }
    if (!ring_json_append(buf, cap, &pos, "]")) {
        return -1;
    }
    return pos;
}

esp_err_t ring_chat_llm_complete(char *reply, size_t cap)
{
    if (!ring_chat_s.init) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "llm: %d history turns", ring_chat_s.hist.count);

    char *msgs = malloc(RING_CHAT_JSON_CAP);
    if (!msgs) return ESP_ERR_NO_MEM;
    int msgs_len = build_messages(msgs, RING_CHAT_JSON_CAP);
    if (msgs_len < 0) {
        free(msgs);
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = malloc(msgs_len + 256);
    if (!body) { free(msgs); return ESP_ERR_NO_MEM; }
    int body_len = snprintf(body, msgs_len + 256,
        "{\"model\":\"%s\",\"messages\":%.*s,\"stream\":false,"
        "\"extra_body\":{\"enable_thinking\":false}}",
        ring_chat_s.cfg.llm_model, (int)msgs_len, msgs);
    if (body_len <= 0 || (size_t)body_len >= msgs_len + 256) {
        free(msgs);
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    free(msgs);

    esp_http_client_handle_t c = ring_http_init(RING_CHAT_LLM_URL, RING_CHAT_CA_PEM,
                                                RING_CHAT_CA_LEN, RING_CHAT_LLM_TIMEOUT_MS,
                                                4096, 1024);
    if (!c) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    ring_http_set_bearer(c, ring_chat_s.api_key);

    /* Stream instead of perform(): perform() swallows the response body (it
     * resets raw_len at the end), and the reply *is* the body. */
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK) {
        if (esp_http_client_write(c, body, body_len) < 0) {
            err = ESP_FAIL;
        }
    }
    free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "llm: open/write failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        char ebuf[257];
        int n = esp_http_client_read_response(c, ebuf, sizeof(ebuf) - 1);
        if (n > 0) {
            ebuf[n] = '\0';
            ESP_LOGW(TAG, "llm: HTTP %d: %s", status, ebuf);
        } else {
            ESP_LOGW(TAG, "llm: HTTP %d", status);
        }
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *resp = malloc(RING_CHAT_JSON_CAP);
    if (!resp) {
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    int rlen = 0;
    esp_err_t rerr = ring_http_read_body(c, resp, RING_CHAT_JSON_CAP, &rlen);
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "llm: read body: %s", esp_err_to_name(rerr));
        free(resp);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!ring_chat_json_get_str(resp, "content", reply, cap)) {
        ESP_LOGW(TAG, "llm: could not extract content from: %.200s", resp);
        free(resp);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "llm: \"%s\"", reply);
    ring_chat_add_assistant(reply);
    free(resp);
    esp_http_client_cleanup(c);
    return ESP_OK;
}

esp_err_t ring_chat_llm_stream(ring_chat_text_cb_t text_cb, void *user)
{
    /* Placeholder — P4 implements streaming LLM with sentence bridge. */
    (void)text_cb; (void)user;
    return ESP_ERR_NOT_SUPPORTED;
}
