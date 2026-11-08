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

/**
 * @brief Reset the sequence. Seed 0 is remapped (xorshift must not run on zero).
 *
 * @param seed  Seed value.
 */
void ring_rand_seed(uint32_t seed);

/**
 * @brief Next 32-bit value. Auto-seeds on first use if ring_rand_seed() was never called.
 *
 * @return the next pseudo-random value.
 */
uint32_t ring_rand_u32(void);

/**
 * @brief Uniform-ish value in [0, n).
 *
 * @param n  Exclusive upper bound.
 * @return a value in [0, n), or 0 when n == 0.
 */
uint32_t ring_rand_below(uint32_t n);

/**
 * @brief Fair coin.
 *
 * @return true or false.
 */
bool ring_rand_bool(void);

#ifdef __cplusplus
}
#endif
