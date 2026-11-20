/*
 * ringkit — minimal JSON field extraction.
 *
 * The realtime endpoints return JSON, but pulling a field out with strstr is
 * brittle: `"type":"error"` misses `{"type": "error"}`, a delta that happens
 * to contain the text "session.created" would trigger a session.update, and a
 * hand-rolled string reader drops leading spaces and breaks on the first
 * escaped quote. These helpers are small enough to run on a transport thread.
 *
 * This is NOT a general JSON parser: it only extracts a top-level string, int
 * or bool by key. For building requests use ring_json_append(), which is
 * overflow-checked.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy the string value of `key` into `out` (always NUL-terminated).
 *
 * Handles the standard escapes and encodes \uXXXX as UTF-8, including
 * surrogate pairs. Returns false when the key is absent or is not a string.
 *
 * **A value that does not fit is silently truncated** and still returns true —
 * that is the right behaviour for a display fragment, but wrong when the value
 * is going to be handed to something that parses it. Use
 * ring_json_get_str_ex() there.
 *
 * @param out      Destination.
 * @param cap      Capacity of @p out.
 * @param out_len  Optional: receives the number of bytes written (excluding
 *                 the terminator).
 */
bool ring_json_get_str(const char *json, const char *key,
                       char *out, size_t cap, size_t *out_len);

/**
 * @brief ring_json_get_str(), but report truncation.
 *
 * Identical except that @p truncated is set when the value did not fit in
 * @p out. A truncated value is a well-formed prefix, not a value: treating it
 * as one is how a JSON payload turns into a request the server rejects.
 *
 * @param truncated Optional: set true when bytes were dropped. Never cleared
 *                  on success/failure, so initialise it at the call site.
 */
bool ring_json_get_str_ex(const char *json, const char *key,
                          char *out, size_t cap, size_t *out_len,
                          bool *truncated);

/**
 * @brief True when `key` appears as a field name at all.
 *
 * Distinguishes "the field is not there" from "the field is there and I could
 * not read it" — the two need different error handling.
 */
bool ring_json_has_key(const char *json, const char *key);

/**
 * @brief Count the elements of a top-level array field.
 *
 * Unlike the getters above this walks the array rather than copying a value, so
 * it can report how much data came back without a second buffer. Understands
 * nested objects/arrays, escaped quotes and commas inside strings. Returns false
 * when `key` is absent, is not an array, or the array is unterminated.
 *
 * @param out  Receives the element count (0 for an empty array).
 */
bool ring_json_count_array(const char *json, const char *key, int *out);

/** Extract an integer value. Returns false when absent or not a number. */
bool ring_json_get_int(const char *json, const char *key, int *out);

/** Extract a bool value (true/false). Returns false when absent. */
bool ring_json_get_bool(const char *json, const char *key, bool *out);

/** True when the event's top-level "type" field equals `type`. */
bool ring_json_type_is(const char *json, const char *type);

/**
 * @brief Append formatted text to a fixed JSON buffer.
 *
 * `snprintf` reports the would-be length, so a plain `pos += snprintf(...)`
 * underflows `cap - pos` on the next call and writes out of bounds. This
 * helper returns false on overflow instead.
 *
 * @param buf  Destination buffer.
 * @param cap  Capacity of @p buf.
 * @param pos  In/out: current offset. Set to `cap` on overflow.
 * @return false when the append did not fit.
 */
bool ring_json_append(char *buf, size_t cap, int *pos, const char *fmt, ...);

/**
 * @brief Append a quoted, escaped JSON string value (no surrounding key).
 *
 * Appends `"<escaped value>"`. Returns false on overflow.
 */
bool ring_json_append_string(char *buf, size_t cap, int *pos,
                              const char *value);

#ifdef __cplusplus
}
#endif
