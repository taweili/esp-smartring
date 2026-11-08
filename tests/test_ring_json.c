/*
 * Host-side unit tests for ring_json (pure C, no ESP-IDF dependencies).
 *
 * Covers: string extraction (escapes, \uXXXX, surrogate pairs, truncation),
 * int/bool extraction, type matching, and the overflow-checked append
 * helpers.
 */
#include <stdio.h>
#include <string.h>

#include "ringkit/ring_json.h"

static int checks;
static int failures;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
        }                                                             \
    } while (0)

/* --------------------------------------------------------------- get_str */

static void test_get_str_basic(void)
{
    char out[64];
    size_t len = 0;

    CHECK(ring_json_get_str("{\"type\":\"session.created\"}", "type",
                            out, sizeof(out), &len), "basic present");
    CHECK(strcmp(out, "session.created") == 0, "basic value");
    CHECK(len == strlen("session.created"), "basic len");

    CHECK(ring_json_get_str("{ \"type\" : \"error\" }", "type",
                            out, sizeof(out), &len), "spaces present");
    CHECK(strcmp(out, "error") == 0, "spaces value");

    /* out_len is optional */
    CHECK(ring_json_get_str("{\"a\":\"b\"}", "a", out, sizeof(out), NULL),
          "null out_len");

    /* NULL / empty guards */
    CHECK(!ring_json_get_str(NULL, "a", out, sizeof(out), NULL), "null json");
    CHECK(!ring_json_get_str("{}", NULL, out, sizeof(out), NULL), "null key");
    CHECK(!ring_json_get_str("{}", "a", NULL, sizeof(out), NULL), "null out");
    CHECK(!ring_json_get_str("{}", "a", out, 0, NULL), "zero cap");
}

static void test_get_str_escapes(void)
{
    char out[64];
    size_t len = 0;

    CHECK(ring_json_get_str("{\"s\":\"a\\\"b\\\\c\\nd\"}", "s",
                            out, sizeof(out), &len), "escapes present");
    CHECK(strcmp(out, "a\"b\\c\nd") == 0, "escapes value");

    CHECK(ring_json_get_str("{\"s\":\"a\\/b\"}", "s", out, sizeof(out), &len),
          "slash present");
    CHECK(strcmp(out, "a/b") == 0, "slash value");

    CHECK(ring_json_get_str("{\"s\":\"a\\tb\"}", "s", out, sizeof(out), &len),
          "tab present");
    CHECK(strcmp(out, "a\tb") == 0, "tab value");

    CHECK(ring_json_get_str("{\"s\":\"a\\bb\\fc\\re\"}", "s",
                            out, sizeof(out), &len), "b/f/r present");
    CHECK(strcmp(out, "a\bb\fc\re") == 0, "b/f/r value");
}

static void test_get_str_unicode(void)
{
    char out[64];
    size_t len = 0;

    /* A + e-acute */
    CHECK(ring_json_get_str("{\"s\":\"\\u0041\\u00e9\"}", "s",
                            out, sizeof(out), &len), "bmp present");
    CHECK(strcmp(out, "A\xc3\xa9") == 0, "bmp utf-8");

    /* U+1F600 via a surrogate pair */
    CHECK(ring_json_get_str("{\"s\":\"\\ud83d\\ude00\"}", "s",
                            out, sizeof(out), &len), "surrogate present");
    CHECK(strcmp(out, "\xf0\x9f\x98\x80") == 0, "surrogate utf-8");

    /* Lone high surrogate: not a valid pair, so it is not combined. The
     * implementation encodes it as a 3-byte UTF-8 sequence. */
    CHECK(ring_json_get_str("{\"s\":\"\\ud83d\"}", "s",
                            out, sizeof(out), &len), "lone surrogate");
    CHECK((unsigned char)out[0] == 0xED, "lone surrogate byte 0");
}

static void test_get_str_truncation(void)
{
    char out[8];
    size_t len = 0;

    /* The value is longer than the buffer: truncate, NUL-terminate, and
     * report the number of bytes actually written. */
    CHECK(ring_json_get_str("{\"s\":\"hello world\"}", "s",
                            out, 4, &len), "truncated present");
    CHECK(strcmp(out, "hel") == 0, "truncated value");
    CHECK(len == 3, "truncated len");

    /* Exact fit: 5 chars + NUL in a 6-byte buffer. */
    CHECK(ring_json_get_str("{\"s\":\"hello\"}", "s", out, 6, &len),
          "exact fit present");
    CHECK(strcmp(out, "hello") == 0, "exact fit value");
    CHECK(len == 5, "exact fit len");

    /* One byte too small. */
    CHECK(ring_json_get_str("{\"s\":\"hello\"}", "s", out, 5, &len),
          "one small present");
    CHECK(strcmp(out, "hell") == 0, "one small value");
}

static void test_get_str_missing(void)
{
    char out[64];

    CHECK(!ring_json_get_str("{\"n\":42}", "s", out, sizeof(out), NULL),
          "absent key");
    CHECK(!ring_json_get_str("{\"s\":42}", "s", out, sizeof(out), NULL),
          "non-string value");
    CHECK(!ring_json_get_str("{\"s\":true}", "s", out, sizeof(out), NULL),
          "bool value");
    CHECK(!ring_json_get_str("{\"s\":\"unterminated}", "s",
                             out, sizeof(out), NULL), "unterminated");
    CHECK(!ring_json_get_str("not json", "s", out, sizeof(out), NULL),
          "not json");
}

/* --------------------------------------------------------------- get_int */

static void test_get_int(void)
{
    int v = 0;

    CHECK(ring_json_get_int("{\"n\":42}", "n", &v), "int positive");
    CHECK(v == 42, "int positive value");

    CHECK(ring_json_get_int("{\"n\":-7}", "n", &v), "int negative");
    CHECK(v == -7, "int negative value");

    CHECK(ring_json_get_int("{\"n\": 123 }", "n", &v), "int spaces");
    CHECK(v == 123, "int spaces value");

    CHECK(!ring_json_get_int("{\"n\":\"42\"}", "n", &v), "int string");
    CHECK(!ring_json_get_int("{\"n\":true}", "n", &v), "int bool");
    CHECK(!ring_json_get_int("{\"x\":1}", "n", &v), "int absent");
    CHECK(!ring_json_get_int(NULL, "n", &v), "int null json");
    CHECK(!ring_json_get_int("{\"n\":1}", "n", NULL), "int null out");
}

/* -------------------------------------------------------------- get_bool */

static void test_get_bool(void)
{
    bool b = false;

    CHECK(ring_json_get_bool("{\"b\":true}", "b", &b), "bool true");
    CHECK(b == true, "bool true value");

    CHECK(ring_json_get_bool("{\"b\":false}", "b", &b), "bool false");
    CHECK(b == false, "bool false value");

    CHECK(!ring_json_get_bool("{\"b\":1}", "b", &b), "bool number");
    CHECK(!ring_json_get_bool("{\"x\":true}", "b", &b), "bool absent");
    CHECK(!ring_json_get_bool(NULL, "b", &b), "bool null json");
}

/* -------------------------------------------------------------- type_is */

static void test_type_is(void)
{
    CHECK(ring_json_type_is("{\"type\":\"error\"}", "error"), "type match");
    CHECK(!ring_json_type_is("{\"type\":\"error\"}", "session.created"),
          "type mismatch");
    CHECK(ring_json_type_is("{ \"type\" : \"error\" }", "error"),
          "type spaces");
    CHECK(!ring_json_type_is("{}", "error"), "type absent");
    CHECK(!ring_json_type_is("not json", "error"), "type not json");
}

/* --------------------------------------------------------------- append */

static void test_append(void)
{
    char buf[32];
    int pos = 0;

    CHECK(ring_json_append(buf, sizeof(buf), &pos, "%d", 42), "append int");
    CHECK(pos == 2 && strcmp(buf, "42") == 0, "append int value");

    CHECK(ring_json_append(buf, sizeof(buf), &pos, "|%s", "x"), "append str");
    CHECK(pos == 4 && strcmp(buf, "42|x") == 0, "append str value");

    /* Empty append is a no-op that succeeds. */
    CHECK(ring_json_append(buf, sizeof(buf), &pos, "%s", ""), "append empty");
    CHECK(pos == 4, "append empty pos");

    /* Overflow: the helper must fail and set pos = cap. */
    char small[4];
    pos = 0;
    CHECK(!ring_json_append(small, sizeof(small), &pos, "abcdef"),
          "append overflow");
    CHECK(pos == (int)sizeof(small), "append overflow pos");

    /* Appending to a full buffer fails. */
    pos = 3;
    CHECK(!ring_json_append(small, sizeof(small), &pos, "d"), "append full");

    CHECK(!ring_json_append(NULL, 4, &pos, "x"), "append null buf");
    CHECK(!ring_json_append(small, 4, NULL, "x"), "append null pos");
    CHECK(!ring_json_append(small, 4, &pos, NULL), "append null fmt");
}

static void test_append_string(void)
{
    char buf[64];
    int pos = 0;

    CHECK(ring_json_append_string(buf, sizeof(buf), &pos, "a\"b\\c\nd"),
          "append_string present");
    CHECK(strcmp(buf, "\"a\\\"b\\\\c\\nd\"") == 0, "append_string value");

    pos = 0;
    CHECK(ring_json_append_string(buf, sizeof(buf), &pos, "a\x01" "b"),
          "append_string control");
    CHECK(strcmp(buf, "\"a\\u0001b\"") == 0, "append_string control value");

    pos = 0;
    CHECK(ring_json_append_string(buf, sizeof(buf), &pos, ""),
          "append_string empty");
    CHECK(strcmp(buf, "\"\"") == 0, "append_string empty value");

    /* Overflow. */
    char small[4];
    pos = 0;
    CHECK(!ring_json_append_string(small, sizeof(small), &pos, "toolong"),
          "append_string overflow");
    CHECK(pos == (int)sizeof(small), "append_string overflow pos");

    CHECK(!ring_json_append_string(NULL, 4, &pos, "x"), "append_string null");
    CHECK(!ring_json_append_string(small, 4, NULL, "x"), "append_string null pos");
    CHECK(!ring_json_append_string(small, 4, &pos, NULL), "append_string null val");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    test_get_str_basic();
    test_get_str_escapes();
    test_get_str_unicode();
    test_get_str_truncation();
    test_get_str_missing();
    test_get_int();
    test_get_bool();
    test_type_is();
    test_append();
    test_append_string();

    printf("ring_json: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
