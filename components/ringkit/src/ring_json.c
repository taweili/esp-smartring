/*
 * ringkit — minimal JSON field extraction. See ring_json.h.
 */
#include "ringkit/ring_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ring_json_get_str_ex(const char *json, const char *key,
                          char *out, size_t cap, size_t *out_len,
                          bool *truncated)
{
    if (json == NULL || key == NULL || out == NULL || cap == 0) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t n = 0;
    bool dropped = false;
    while (*p != '\0' && *p != '"') {
        char c = *p;
        if (c != '\\') {
            if (n + 1 < cap) {
                out[n++] = c;
            } else {
                dropped = true;
            }
            p++;
            continue;
        }
        p++;
        switch (*p) {
        case '"':  c = '"';  break;
        case '\\': c = '\\'; break;
        case '/':  c = '/';  break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'n':  c = '\n'; break;
        case 'r':  c = '\r'; break;
        case 't':  c = '\t'; break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                p++;
                char h = *p;
                cp <<= 4;
                if (h >= '0' && h <= '9') {
                    cp |= (unsigned)(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    cp |= (unsigned)(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    cp |= (unsigned)(h - 'A' + 10);
                } else {
                    return false;
                }
            }
            /* Combine a UTF-16 surrogate pair when one follows. */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                p[1] == '\\' && p[2] == 'u') {
                unsigned lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    char h = p[3 + i];
                    lo <<= 4;
                    if (h >= '0' && h <= '9') {
                        lo |= (unsigned)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        lo |= (unsigned)(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        lo |= (unsigned)(h - 'A' + 10);
                    } else {
                        ok = false;
                        break;
                    }
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            char enc[4];
            int m;
            if (cp < 0x80) {
                enc[0] = (char)cp;
                m = 1;
            } else if (cp < 0x800) {
                enc[0] = (char)(0xC0 | (cp >> 6));
                enc[1] = (char)(0x80 | (cp & 0x3F));
                m = 2;
            } else if (cp < 0x10000) {
                enc[0] = (char)(0xE0 | (cp >> 12));
                enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                enc[2] = (char)(0x80 | (cp & 0x3F));
                m = 3;
            } else {
                enc[0] = (char)(0xF0 | (cp >> 18));
                enc[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                enc[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                enc[3] = (char)(0x80 | (cp & 0x3F));
                m = 4;
            }
            for (int i = 0; i < m; i++) {
                if (n + 1 < cap) {
                    out[n++] = enc[i];
                } else {
                    dropped = true;
                }
            }
            p++;
            continue;
        }
        default:
            return false;
        }
        if (n + 1 < cap) {
            out[n++] = c;
        } else {
            dropped = true;
        }
        p++;
    }
    if (*p != '"') {            /* unterminated: the frame was truncated */
        return false;
    }
    out[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    if (truncated != NULL && dropped) {
        *truncated = true;
    }
    return true;
}

bool ring_json_get_str(const char *json, const char *key,
                       char *out, size_t cap, size_t *out_len)
{
    return ring_json_get_str_ex(json, key, out, cap, out_len, NULL);
}

bool ring_json_has_key(const char *json, const char *key)
{
    if (json == NULL || key == NULL) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return *p == ':';
}

bool ring_json_count_array(const char *json, const char *key, int *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '[') {
        return false;
    }
    p++;

    int count = 0;
    int depth = 0;          /* nested object/array depth inside the array */
    bool in_str = false;
    bool esc = false;
    bool seen = false;      /* current slot holds a value */
    for (; *p != '\0'; p++) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        switch (c) {
        case '"':
            in_str = true;
            seen = true;
            break;
        case '{':
        case '[':
            depth++;
            seen = true;
            break;
        case '}':
            if (depth > 0) {
                depth--;
            }
            seen = true;
            break;
        case ']':
            if (depth == 0) {
                if (seen) {
                    count++;
                }
                *out = count;
                return true;
            }
            depth--;
            seen = true;
            break;
        case ',':
            if (depth == 0) {
                if (seen) {
                    count++;
                    seen = false;
                }
            } else {
                seen = true;
            }
            break;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default:
            seen = true;
            break;
        }
    }
    return false;           /* unterminated array */
}

bool ring_json_get_int(const char *json, const char *key, int *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool ring_json_get_bool(const char *json, const char *key, bool *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool ring_json_type_is(const char *json, const char *type)
{
    char buf[64];
    size_t len = 0;
    return ring_json_get_str(json, "type", buf, sizeof(buf), &len) &&
           len == strlen(type) && memcmp(buf, type, len) == 0;
}

bool ring_json_append_string(char *buf, size_t cap, int *pos,
                              const char *value)
{
    if (buf == NULL || pos == NULL || value == NULL) {
        return false;
    }
    if (!ring_json_append(buf, cap, pos, "\"")) {
        return false;
    }
    for (const char *p = value; *p != '\0'; p++) {
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
    return ring_json_append(buf, cap, pos, "\"");
}

bool ring_json_append(char *buf, size_t cap, int *pos, const char *fmt, ...)
{
    if (buf == NULL || pos == NULL || fmt == NULL) {
        return false;
    }
    if (*pos < 0 || (size_t)*pos >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *pos, cap - (size_t)*pos, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - (size_t)*pos) {
        *pos = (int)cap;
        return false;
    }
    *pos += w;
    return true;
}
