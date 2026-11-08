/*
 * Host-side unit tests for the newest-wins byte ring.
 *
 * Covers: basic push/pop, partial reads, overflow (newest wins), the even
 * drop that keeps 16-bit PCM aligned, reset, and the NULL/empty guards.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "ringkit/ring_byte_ring.h"

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

static void test_init_basic(void)
{
    ring_byte_ring_t r;
    size_t dropped = 99;

    CHECK(ring_byte_ring_init(&r, 8, MALLOC_CAP_INTERNAL) == ESP_OK, "init");
    CHECK(ring_byte_ring_count(&r) == 0, "empty count");

    uint8_t in[4] = {1, 2, 3, 4};
    ring_byte_ring_push(&r, in, 4, &dropped);
    CHECK(dropped == 0, "no drop under capacity");
    CHECK(ring_byte_ring_count(&r) == 4, "count after push");

    uint8_t out[8];
    memset(out, 0xAA, sizeof(out));
    size_t got = ring_byte_ring_pop(&r, out, sizeof(out));
    CHECK(got == 4, "pop all count");
    CHECK(memcmp(out, in, 4) == 0, "pop all value");
    CHECK(ring_byte_ring_count(&r) == 0, "empty after pop");

    ring_byte_ring_deinit(&r);
    CHECK(ring_byte_ring_count(&r) == 0, "count after deinit");

    /* Safe on a zero-initialised ring. */
    ring_byte_ring_t z = {0};
    ring_byte_ring_deinit(&z);
    ring_byte_ring_reset(&z);
    CHECK(ring_byte_ring_count(&z) == 0, "zero-init count");

    /* Bad args. */
    CHECK(ring_byte_ring_init(NULL, 8, MALLOC_CAP_INTERNAL) == ESP_ERR_INVALID_ARG,
          "init null");
    CHECK(ring_byte_ring_init(&r, 0, MALLOC_CAP_INTERNAL) == ESP_ERR_INVALID_ARG,
          "init zero cap");
}

static void test_partial_pop(void)
{
    ring_byte_ring_t r;
    ring_byte_ring_init(&r, 8, MALLOC_CAP_INTERNAL);

    uint8_t in[6] = {10, 11, 12, 13, 14, 15};
    ring_byte_ring_push(&r, in, 6, NULL);
    CHECK(ring_byte_ring_count(&r) == 6, "partial push count");

    uint8_t out[8];
    size_t got = ring_byte_ring_pop(&r, out, 3);
    CHECK(got == 3, "partial pop count");
    CHECK(memcmp(out, in, 3) == 0, "partial pop value");
    CHECK(ring_byte_ring_count(&r) == 3, "partial pop remaining");

    /* Popping more than remains returns only what is there. */
    got = ring_byte_ring_pop(&r, out, 8);
    CHECK(got == 3, "drain count");
    CHECK(memcmp(out, in + 3, 3) == 0, "drain value");
    CHECK(ring_byte_ring_count(&r) == 0, "drain empty");

    /* Pop from empty. */
    got = ring_byte_ring_pop(&r, out, 8);
    CHECK(got == 0, "pop empty");

    ring_byte_ring_deinit(&r);
}

static void test_overwrite_newest_wins(void)
{
    ring_byte_ring_t r;
    ring_byte_ring_init(&r, 8, MALLOC_CAP_INTERNAL);

    uint8_t seq[16];
    for (int i = 0; i < 16; i++) {
        seq[i] = (uint8_t)i;
    }

    size_t dropped = 0;
    ring_byte_ring_push(&r, seq, 16, &dropped);
    CHECK(ring_byte_ring_count(&r) == 8, "overwrite count is capacity");
    CHECK(dropped == 8, "overwrite dropped 8");

    uint8_t out[8];
    size_t got = ring_byte_ring_pop(&r, out, sizeof(out));
    CHECK(got == 8, "overwrite pop count");
    CHECK(memcmp(out, seq + 8, 8) == 0, "overwrite keeps newest 8");

    ring_byte_ring_deinit(&r);
}

static void test_overflow_alignment(void)
{
    /* Overflow drops in whole 16-bit samples, so the drop count is always
     * even and the retained stream stays sample-aligned. */
    ring_byte_ring_t r;
    ring_byte_ring_init(&r, 8, MALLOC_CAP_INTERNAL);

    uint8_t seq[9];
    for (int i = 0; i < 9; i++) {
        seq[i] = (uint8_t)i;
    }

    size_t dropped = 0;
    ring_byte_ring_push(&r, seq, 9, &dropped);
    CHECK(dropped % 2 == 0, "drop count is even");
    CHECK(ring_byte_ring_count(&r) == 7, "odd push leaves odd count");

    uint8_t out[8];
    size_t got = ring_byte_ring_pop(&r, out, sizeof(out));
    CHECK(got == 7, "aligned pop count");
    /* The retained window is the last 7 bytes of the input. */
    CHECK(memcmp(out, seq + 2, 7) == 0, "aligned pop value");

    ring_byte_ring_deinit(&r);
}

static void test_reset_and_zero_push(void)
{
    ring_byte_ring_t r;
    ring_byte_ring_init(&r, 8, MALLOC_CAP_INTERNAL);

    uint8_t in[4] = {1, 2, 3, 4};
    ring_byte_ring_push(&r, in, 4, NULL);
    CHECK(ring_byte_ring_count(&r) == 4, "count before reset");

    ring_byte_ring_reset(&r);
    CHECK(ring_byte_ring_count(&r) == 0, "count after reset");

    uint8_t out[4];
    CHECK(ring_byte_ring_pop(&r, out, sizeof(out)) == 0, "pop after reset");

    /* Zero-length push is a no-op. */
    size_t dropped = 5;
    ring_byte_ring_push(&r, in, 0, &dropped);
    CHECK(dropped == 5, "zero push leaves dropped untouched");
    CHECK(ring_byte_ring_count(&r) == 0, "zero push count");

    ring_byte_ring_deinit(&r);
}

static void test_null_guards(void)
{
    uint8_t out[4];
    uint8_t in[4] = {0};

    CHECK(ring_byte_ring_count(NULL) == 0, "count null");
    CHECK(ring_byte_ring_pop(NULL, out, 4) == 0, "pop null ring");

    /* void functions: just exercise the NULL path without crashing. */
    ring_byte_ring_push(NULL, in, 4, NULL);
    ring_byte_ring_reset(NULL);
    ring_byte_ring_deinit(NULL);
    checks += 3;
}

int main(void)
{
    test_init_basic();
    test_partial_pop();
    test_overwrite_newest_wins();
    test_overflow_alignment();
    test_reset_and_zero_push();
    test_null_guards();

    printf("ring_byte_ring: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
