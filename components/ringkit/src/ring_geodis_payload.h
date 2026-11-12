/*
 * ringkit — ring_geodis request payload builders (pure, host-testable).
 *
 * Split out of ring_geodis_protocol.c / ring_geodis_signaling.c because these
 * three functions are where a silent regression costs a whole turn: the server
 * 400s on a top-level `modalities`, 500s on a non-string `arguments`, and
 * ignores a truncated body. None of them touches global state or hardware, so
 * tests/ can link this file alone and assert the exact bytes.
 *
 * Every builder returns false on overflow and leaves @p buf NUL-terminated at
 * whatever prefix fit — the caller must treat false as "do not send this".
 *
 * Not part of the public API; applications include ringkit/ring_geodis.h only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------- session.update
 *
 * The GA endpoint rejects the legacy top-level `modalities` / `turn_detection`
 * with `unknown_parameter`, and rejects them nested the same way. The only
 * accepted shape is session.audio.{input,output}:
 *
 *   {"type":"session.update","session":{"audio":{
 *      "input":{ "transcription":{"model":"whisper-1"}, "turn_detection":null },
 *      "output":{"voice":"coral"} }}}
 *
 * `turn_detection: null` is what makes push-to-talk work — it disables server
 * VAD so the client owns commit(). Omitting the key instead of sending null
 * leaves the session on whatever preset the server picked.
 *
 * `input` is omitted entirely when both flags are off (nothing to say), and
 * `transcription` / `turn_detection` are omitted individually.
 *
 * @param buf      Destination.
 * @param cap      Capacity of @p buf.
 * @param voice    Voice name; NULL or "" omits the output block.
 * @param enable_stt  Include input.transcription.
 * @param enable_ptt  Include input.turn_detection = null.
 * @return false on overflow.
 */
bool ring_geodis_payload_session_update(char *buf, size_t cap, const char *voice,
                                        bool enable_stt, bool enable_ptt);

/* ------------------------------------------------------------- POST /token
 *
 * Body of the ephemeral-token request. `system_prompt` is optional: an empty
 * prompt omits the field rather than sending "", which the gateway treats as
 * an explicit empty prompt and which shows up as a silent assistant.
 *
 * Escaping can roughly double the prompt, so @p cap should be at least
 * strlen(chat_session_id) + 2 * strlen(system_prompt) + 64. Returns false
 * rather than posting a truncated body.
 */
bool ring_geodis_payload_token_body(char *buf, size_t cap,
                                    const char *chat_session_id,
                                    const char *system_prompt);

/* ------------------------------------------------------------- POST /tool-call
 *
 * `arguments` MUST be a JSON **string**, not an object: the gateway 500s on an
 * object, and the realtime event already hands us the arguments as a string,
 * so it is embedded verbatim (re-escaped once, as a JSON value).
 *
 * @param args_json  The `arguments` string as received (already JSON text,
 *                   e.g. `{"numero":"12345"}`), NOT pre-escaped.
 */
bool ring_geodis_payload_tool_body(char *buf, size_t cap, const char *name,
                                   const char *args_json, const char *session_id);

/* ------------------------------------------------------------- function_call_output
 *
 * The conversation.item.create that attaches a tool result to the call. When
 * @p output does not fit, a short `lookup_unavailable` item is emitted instead
 * of a truncated one — the model can act on an error, not on half a JSON blob.
 * Always returns true unless even the fallback does not fit, in which case
 * @p buf must not be sent.
 */
bool ring_geodis_payload_tool_output(char *buf, size_t cap, const char *call_id,
                                     const char *output);

/* ------------------------------------------------------------- sizes */

/* Capacity that always suffices for ring_geodis_payload_session_update(). */
#define RING_GEODIS_SESSION_UPDATE_CAP 512

/* Capacity for a token body: JSON escaping can roughly double the prompt.
 * Kept as a local literal rather than referencing RING_GEODIS_PROMPT_MAX in
 * ring_geodis_priv.h, so this file stays linkable on its own (the host tests
 * build it without the ESP-IDF headers priv.h pulls in). tests/ asserts that
 * the two numbers agree. */
#define RING_GEODIS_TOKEN_BODY_CAP (512 * 2 + 320)
