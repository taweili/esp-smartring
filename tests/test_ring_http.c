/*
 * Host test for ring_http.c: the response-body reader and the buffered SSE
 * reader, including the error paths that a real socket can produce.
 *
 * The HTTP client is a scriptable byte source (test_ring_http_stubs.c), so a
 * negative esp_http_client_read() return and a short buffer are deterministic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"

#include "ringkit/ring_http.h"

void test_http_fake_set(const char *data, size_t len, size_t err_at);

static int s_checks;
static int s_fail;

static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL: %s\n", what);
    }
}

#define CLIENT ((esp_http_client_handle_t)1)

static void test_read_body_ok(void)
{
    test_http_fake_set("hello", 5, (size_t)-1);
    char buf[32];
    int n = -1;
    bool truncated = true;
    esp_err_t err = ring_http_read_body_ex(CLIENT, buf, sizeof(buf), &n,
                                           &truncated);
    check(err == ESP_OK, "read_body ok: ESP_OK");
    check(n == 5, "read_body ok: length 5");
    check(strcmp(buf, "hello") == 0, "read_body ok: content");
    check(!truncated, "read_body ok: not truncated");

    /* Wrapper behaves the same. */
    test_http_fake_set("hello", 5, (size_t)-1);
    n = -1;
    err = ring_http_read_body(CLIENT, buf, sizeof(buf), &n);
    check(err == ESP_OK && n == 5, "read_body wrapper");
}

static void test_read_body_exact_fit(void)
{
    /* cap-1 == body length: the probe read returns 0, so this is NOT
     * truncation. */
    test_http_fake_set("hello", 5, (size_t)-1);
    char buf[6];
    int n = -1;
    bool truncated = true;
    esp_err_t err = ring_http_read_body_ex(CLIENT, buf, sizeof(buf), &n,
                                           &truncated);
    check(err == ESP_OK, "exact fit: ESP_OK");
    check(n == 5, "exact fit: length 5");
    check(!truncated, "exact fit: not truncated");
}

static void test_read_body_truncated(void)
{
    test_http_fake_set("hello world", 11, (size_t)-1);
    char buf[6];
    int n = -1;
    bool truncated = false;
    esp_err_t err = ring_http_read_body_ex(CLIENT, buf, sizeof(buf), &n,
                                           &truncated);
    check(err == ESP_OK, "truncated: ESP_OK");
    check(n == 5, "truncated: length 5");
    check(truncated, "truncated: flagged");
}

static void test_read_body_transport_error(void)
{
    /* Error after three bytes: must NOT be reported as a short success. */
    test_http_fake_set("abcdef", 6, 3);
    char buf[32];
    int n = -1;
    bool truncated = false;
    esp_err_t err = ring_http_read_body_ex(CLIENT, buf, sizeof(buf), &n,
                                           &truncated);
    check(err != ESP_OK, "mid-stream error: reported");

    /* Error before any byte. */
    test_http_fake_set("abcdef", 6, 0);
    err = ring_http_read_body(CLIENT, buf, sizeof(buf), &n);
    check(err != ESP_OK, "immediate error: reported");
}

static void test_read_body_empty(void)
{
    test_http_fake_set("", 0, (size_t)-1);
    char buf[8];
    int n = -1;
    esp_err_t err = ring_http_read_body(CLIENT, buf, sizeof(buf), &n);
    check(err != ESP_OK, "empty body: reported");
}

static void test_sse_lines(void)
{
    ring_http_sse_t s;
    check(ring_http_sse_init(&s, CLIENT), "sse init");

    test_http_fake_set("a\nbb\r\nc", 7, (size_t)-1);
    bool truncated = false;

    const char *l1 = ring_http_sse_next(&s, &truncated);
    check(l1 != NULL && strcmp(l1, "a") == 0, "sse line 1");
    const char *l2 = ring_http_sse_next(&s, &truncated);
    check(l2 != NULL && strcmp(l2, "bb") == 0, "sse line 2 (CR stripped)");
    const char *l3 = ring_http_sse_next(&s, &truncated);
    check(l3 != NULL && strcmp(l3, "c") == 0, "sse final line without newline");
    const char *l4 = ring_http_sse_next(&s, &truncated);
    check(l4 == NULL, "sse end");
    check(s.eof, "sse eof flag");
    check(!s.err, "sse no error flag");

    ring_http_sse_deinit(&s);
}

static void test_sse_long_line(void)
{
    ring_http_sse_t s;
    check(ring_http_sse_init(&s, CLIENT), "sse init (long)");

    size_t n = 20000;
    char *data = malloc(n + 2);
    memset(data, 'x', n);
    data[n] = '\n';
    data[n + 1] = '\0';
    test_http_fake_set(data, n + 1, (size_t)-1);

    bool truncated = false;
    const char *line = ring_http_sse_next(&s, &truncated);
    check(line != NULL, "sse long line returned");
    check(strlen(line) == n, "sse long line length");
    check(!truncated, "sse long line not truncated");
    check(!s.err, "sse long line no error");

    free(data);
    ring_http_sse_deinit(&s);
}

static void test_sse_transport_error(void)
{
    ring_http_sse_t s;
    check(ring_http_sse_init(&s, CLIENT), "sse init (error)");

    /* "ab\ncd": the first line succeeds, then the read errors out. */
    test_http_fake_set("ab\ncd", 5, 3);
    bool truncated = false;
    const char *l1 = ring_http_sse_next(&s, &truncated);
    check(l1 != NULL && strcmp(l1, "ab") == 0, "sse error: first line");
    const char *l2 = ring_http_sse_next(&s, &truncated);
    check(l2 == NULL, "sse error: returns NULL");
    check(s.err, "sse error: err flag set");
    check(!s.eof, "sse error: eof not set");

    ring_http_sse_deinit(&s);
}

static void test_bearer_length(void)
{
    char key[RING_HTTP_MAX_BEARER_KEY + 2];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    check(ring_http_set_bearer(CLIENT, key) == ESP_ERR_INVALID_ARG,
          "bearer: over-long key rejected");
    key[RING_HTTP_MAX_BEARER_KEY] = '\0';
    check(ring_http_set_bearer(CLIENT, key) == ESP_OK,
          "bearer: max-length key accepted");
}

int main(void)
{
    test_read_body_ok();
    test_read_body_exact_fit();
    test_read_body_truncated();
    test_read_body_transport_error();
    test_read_body_empty();
    test_sse_lines();
    test_sse_long_line();
    test_sse_transport_error();
    test_bearer_length();

    printf("ring_http: %d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
