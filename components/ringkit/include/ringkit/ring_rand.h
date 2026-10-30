/*
 * ringkit — deterministic pseudo-random numbers.
 *
 * Applications that scatter content (glyph shapes, accent/green split) want
 * reproducible output across boots, so this is a plain xorshift32 rather than
 * esp_random().
 *
 * NOTE: do NOT swap this for a plain LCG (`s = s * 1664525 + 1013904223`).
 * An LCG's low bits are degenerate: `x % 6` and `x & 1` then collapse to even
 * values only, which produced an all-accent field using just 3 of the 6 glyph
 * shapes. xorshift32 mixes all bits.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Reset the sequence. Seed 0 is remapped (xorshift must not run on zero). */
void ring_rand_seed(uint32_t seed);

/** Next 32-bit value. Auto-seeds on first use if ring_rand_seed() was never called. */
uint32_t ring_rand_u32(void);

/** Uniform-ish value in [0, n). Returns 0 when n == 0. */
uint32_t ring_rand_below(uint32_t n);

/** Fair coin. */
bool ring_rand_bool(void);

#ifdef __cplusplus
}
#endif
