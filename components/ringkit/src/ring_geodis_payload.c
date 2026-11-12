/*
 * ringkit — ring_geodis request payload builders.
 *
 * Pure functions: no globals, no FreeRTOS, no logging. The only dependency is
 * ring_json's overflow-checked appenders, which the host test harness already
 * links. See ring_geodis_payload.h for the wire-format rules each builder
 * encodes.
 */
#include "ring_geodis_payload.h"

#include "ringkit/ring_json.h"

#include <string.h>

bool ring_geodis_payload_session_update(char *buf, size_t cap, const char *voice,
                                        bool enable_stt, bool enable_ptt)
{
    int pos = 0;
    bool ok = ring_json_append(buf, cap, &pos,
                               "{\"type\":\"session.update\",\"session\":{\"audio\":{");
    bool need_comma = false;

    if (ok && (enable_stt || enable_ptt)) {
        ok = ring_json_append(buf, cap, &pos, "\"input\":{");
        bool in_comma = false;
        if (ok && enable_stt) {
            ok = ring_json_append(buf, cap, &pos,
                                  "\"transcription\":{\"model\":\"whisper-1\"}");
            in_comma = true;
        }
        if (ok && enable_ptt) {
            if (in_comma) {
                ok = ring_json_append(buf, cap, &pos, ",");
            }
            if (ok) {
                /* null, not "none" and not omitted: this is what disables the
                 * server's VAD so the client's commit() defines a turn. */
                ok = ring_json_append(buf, cap, &pos, "\"turn_detection\":null");
            }
        }
        if (ok) {
            ok = ring_json_append(buf, cap, &pos, "}");
        }
        need_comma = true;
    }

    if (ok && voice != NULL && voice[0] != '\0') {
        if (need_comma) {
            ok = ring_json_append(buf, cap, &pos, ",");
        }
        if (ok) {
            ok = ring_json_append(buf, cap, &pos, "\"output\":{\"voice\":");
        }
        if (ok) {
            ok = ring_json_append_string(buf, cap, &pos, voice);
        }
        if (ok) {
            ok = ring_json_append(buf, cap, &pos, "}");
        }
    }

    if (ok) {
        ok = ring_json_append(buf, cap, &pos, "}}}");
    }
    return ok;
}

bool ring_geodis_payload_token_body(char *buf, size_t cap,
                                    const char *chat_session_id,
                                    const char *system_prompt)
{
    int pos = 0;
    bool ok = ring_json_append(buf, cap, &pos, "{\"chat_session_id\":");
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos,
                                     chat_session_id != NULL ? chat_session_id : "");
    }
    if (ok && system_prompt != NULL && system_prompt[0] != '\0') {
        ok = ring_json_append(buf, cap, &pos, ",\"system_prompt\":");
        if (ok) {
            ok = ring_json_append_string(buf, cap, &pos, system_prompt);
        }
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, "}");
    }
    return ok;
}

bool ring_geodis_payload_tool_body(char *buf, size_t cap, const char *name,
                                   const char *args_json, const char *session_id)
{
    int pos = 0;
    /* `arguments` is embedded as a JSON string: the event's field is already
     * JSON text, and the gateway rejects an object here with a 500. */
    bool ok = ring_json_append(buf, cap, &pos, "{\"name\":");
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos, name != NULL ? name : "");
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, ",\"arguments\":");
    }
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos,
                                     args_json != NULL ? args_json : "{}");
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, ",\"session_id\":");
    }
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos,
                                     session_id != NULL ? session_id : "");
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, "}");
    }
    return ok;
}

bool ring_geodis_payload_tool_output(char *buf, size_t cap, const char *call_id,
                                     const char *output)
{
    int pos = 0;
    bool ok = ring_json_append(buf, cap, &pos,
                               "{\"type\":\"conversation.item.create\",\"item\":"
                               "{\"type\":\"function_call_output\",\"call_id\":");
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos, call_id != NULL ? call_id : "");
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, ",\"output\":");
    }
    if (ok) {
        ok = ring_json_append_string(buf, cap, &pos, output != NULL ? output : "");
    }
    if (ok) {
        ok = ring_json_append(buf, cap, &pos, "}}");
    }
    if (ok) {
        return true;
    }

    /* The item overflowed. A truncated item is unparseable JSON and the turn
     * hangs; a short honest error lets the model apologise and move on. */
    pos = 0;
    ring_json_append(buf, cap, &pos,
                     "{\"type\":\"conversation.item.create\",\"item\":"
                     "{\"type\":\"function_call_output\",\"call_id\":\"%s\","
                     "\"output\":\"{\\\"error\\\":\\\"lookup_unavailable\\\","
                     "\\\"detail\\\":\\\"response_too_large\\\"}\"}",
                     call_id != NULL ? call_id : "");
    return (pos > 0 && (size_t)pos < cap);
}
