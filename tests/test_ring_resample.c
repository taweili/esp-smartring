/*
 * Host-side unit tests for the polyphase resampler (pure C + generated
 * coefficient tables).
 *
 * Covers: coefficient normalisation, DC gain, impulse response shape, and
 * streaming-chunk equivalence (the history carry must make chunked input
 * produce the same output as one shot).
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ringkit/ring_resample.h"

extern const float ring_resample_decim_coeffs[RING_RESAMPLE_DECIM_PHASES][RING_RESAMPLE_DECIM_PHASE_LEN];
extern const float ring_resample_interp_coeffs[RING_RESAMPLE_INTERP_PHASES][RING_RESAMPLE_INTERP_PHASE_LEN];

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

#define TOL 1e-5f

static bool close_to(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

/* Deterministic pseudo-random sample in [-1, 1). */
static float next_sample(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return (float)((int)(*state >> 8) % 2000 - 1000) / 1000.0f;
}

/* --------------------------------------------------------- coefficients */

static void test_coefficient_sums(void)
{
    float decim_total = 0.0f;
    for (int p = 0; p < RING_RESAMPLE_DECIM_PHASES; p++) {
        float s = 0.0f;
        for (int k = 0; k < RING_RESAMPLE_DECIM_PHASE_LEN; k++) {
            s += ring_resample_decim_coeffs[p][k];
        }
        decim_total += s;
    }
    CHECK(close_to(decim_total, 1.0f, 1e-3f), "decim DC gain = 1");

    for (int p = 0; p < RING_RESAMPLE_INTERP_PHASES; p++) {
        float s = 0.0f;
        for (int k = 0; k < RING_RESAMPLE_INTERP_PHASE_LEN; k++) {
            s += ring_resample_interp_coeffs[p][k];
        }
        CHECK(close_to(s, 1.0f, 2e-3f), "interp phase DC gain = 1");
    }
}

/* -------------------------------------------------------------- decim */

static void test_decim_dc(void)
{
    const int n_in = 300;
    float in[300];
    float out[100];
    for (int i = 0; i < n_in; i++) {
        in[i] = 1.0f;
    }

    ring_resample_decim_t s;
    ring_resample_decim_init(&s);
    int n_out = ring_resample_decim_process(&s, in, n_in, out);
    CHECK(n_out == n_in / 3, "decim dc count");

    /* After the filter settles the output must equal the DC input. */
    CHECK(close_to(out[n_out - 1], 1.0f, 2e-3f), "decim dc steady state");
    CHECK(close_to(out[n_out / 2], 1.0f, 2e-3f), "decim dc mid");
}

static void test_decim_impulse(void)
{
    const int n_in = 63;
    float in[63];
    float out[64];
    memset(in, 0, sizeof(in));
    in[0] = 1.0f;

    ring_resample_decim_t s;
    ring_resample_decim_init(&s);
    int n_out = ring_resample_decim_process(&s, in, n_in, out);
    CHECK(n_out == 21, "decim impulse count");

    /* With this indexing the impulse response is phase 0's tap vector. */
    for (int k = 0; k < RING_RESAMPLE_DECIM_PHASE_LEN; k++) {
        CHECK(close_to(out[k], ring_resample_decim_coeffs[0][k], TOL),
              "decim impulse tap");
    }
    for (int k = RING_RESAMPLE_DECIM_PHASE_LEN; k < n_out; k++) {
        CHECK(close_to(out[k], 0.0f, TOL), "decim impulse tail");
    }
}

static void test_decim_streaming(void)
{
    const int n_in = 300;
    float in[300];
    float ref[100];
    float got[100];
    uint32_t seed = 42;

    for (int i = 0; i < n_in; i++) {
        in[i] = next_sample(&seed);
    }

    ring_resample_decim_t s;
    ring_resample_decim_init(&s);
    int ref_n = ring_resample_decim_process(&s, in, n_in, ref);

    /* Feed the same signal in chunks that divide n_in exactly (the
     * decimator requires n_in to be a multiple of 3). */
    const int chunks[] = {3, 6, 15, 30};
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        ring_resample_decim_init(&s);
        int step = chunks[c];
        int got_n = 0;
        for (int off = 0; off < n_in; off += step) {
            got_n += ring_resample_decim_process(&s, in + off, step,
                                                 got + got_n);
        }
        CHECK(got_n == ref_n, "decim streaming count");
        for (int i = 0; i < ref_n; i++) {
            CHECK(close_to(got[i], ref[i], TOL), "decim streaming sample");
        }
    }
}

/* -------------------------------------------------------------- interp */

static void test_interp_dc(void)
{
    const int n_in = 100;
    float in[100];
    float out[200];
    for (int i = 0; i < n_in; i++) {
        in[i] = 1.0f;
    }

    ring_resample_interp_t s;
    ring_resample_interp_init(&s);
    int n_out = ring_resample_interp_process(&s, in, n_in, out);
    CHECK(n_out == 2 * n_in, "interp dc count");
    CHECK(close_to(out[n_out - 1], 1.0f, 2e-3f), "interp dc steady state");
    CHECK(close_to(out[n_out / 2], 1.0f, 2e-3f), "interp dc mid");
}

static void test_interp_impulse(void)
{
    const int n_in = 24;
    float in[24];
    float out[48];
    memset(in, 0, sizeof(in));
    in[0] = 1.0f;

    ring_resample_interp_t s;
    ring_resample_interp_init(&s);
    int n_out = ring_resample_interp_process(&s, in, n_in, out);
    CHECK(n_out == 2 * n_in, "interp impulse count");

    /* Even outputs carry phase 0, odd outputs phase 1. */
    for (int k = 0; k < RING_RESAMPLE_INTERP_PHASE_LEN; k++) {
        CHECK(close_to(out[2 * k], ring_resample_interp_coeffs[0][k], TOL),
              "interp impulse phase 0");
        CHECK(close_to(out[2 * k + 1], ring_resample_interp_coeffs[1][k], TOL),
              "interp impulse phase 1");
    }
    for (int k = 2 * RING_RESAMPLE_INTERP_PHASE_LEN; k < n_out; k++) {
        CHECK(close_to(out[k], 0.0f, TOL), "interp impulse tail");
    }
}

static void test_interp_streaming(void)
{
    const int n_in = 100;
    float in[100];
    float ref[200];
    float got[200];
    uint32_t seed = 7;

    for (int i = 0; i < n_in; i++) {
        in[i] = next_sample(&seed);
    }

    ring_resample_interp_t s;
    ring_resample_interp_init(&s);
    int ref_n = ring_resample_interp_process(&s, in, n_in, ref);

    const int chunks[] = {1, 2, 7, 25};
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        ring_resample_interp_init(&s);
        int step = chunks[c];
        int got_n = 0;
        for (int off = 0; off < n_in; off += step) {
            int n = (off + step <= n_in) ? step : n_in - off;
            got_n += ring_resample_interp_process(&s, in + off, n,
                                                  got + got_n);
        }
        CHECK(got_n == ref_n, "interp streaming count");
        for (int i = 0; i < ref_n; i++) {
            CHECK(close_to(got[i], ref[i], TOL), "interp streaming sample");
        }
    }
}

/* ------------------------------------------------------- degenerate args */

static void test_null_args(void)
{
    ring_resample_decim_t d;
    ring_resample_interp_t it;
    float in[3] = {1, 2, 3};
    float out[3];

    ring_resample_decim_init(NULL);
    ring_resample_interp_init(NULL);

    CHECK(ring_resample_decim_process(NULL, in, 3, out) == 0, "decim null state");
    CHECK(ring_resample_decim_process(&d, NULL, 3, out) == 0, "decim null in");
    CHECK(ring_resample_decim_process(&d, in, 3, NULL) == 0, "decim null out");
    CHECK(ring_resample_decim_process(&d, in, 0, out) == 0, "decim zero in");
    CHECK(ring_resample_decim_process(&d, in, 2, out) == 0, "decim partial in");

    CHECK(ring_resample_interp_process(NULL, in, 3, out) == 0, "interp null state");
    CHECK(ring_resample_interp_process(&it, NULL, 3, out) == 0, "interp null in");
    CHECK(ring_resample_interp_process(&it, in, 3, NULL) == 0, "interp null out");
    CHECK(ring_resample_interp_process(&it, in, 0, out) == 0, "interp zero in");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    test_coefficient_sums();
    test_decim_dc();
    test_decim_impulse();
    test_decim_streaming();
    test_interp_dc();
    test_interp_impulse();
    test_interp_streaming();
    test_null_args();

    printf("ring_resample: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
