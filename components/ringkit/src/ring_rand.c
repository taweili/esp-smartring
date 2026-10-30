/*
 * ringkit — deterministic pseudo-random numbers. See ring_rand.h.
 */
#include "ringkit/ring_rand.h"

#define RING_RAND_DEFAULT_SEED 0x9E3779B9u

static uint32_t s_state = RING_RAND_DEFAULT_SEED;
static bool     s_seeded;

void ring_rand_seed(uint32_t seed)
{
    s_state = (seed != 0) ? seed : RING_RAND_DEFAULT_SEED;
    s_seeded = true;
}

uint32_t ring_rand_u32(void)
{
    if (!s_seeded) {
        s_seeded = true;      /* keep the default seed */
    }
    uint32_t x = s_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x;
    return x;
}

uint32_t ring_rand_below(uint32_t n)
{
    return (n == 0) ? 0 : (ring_rand_u32() % n);
}

bool ring_rand_bool(void)
{
    return (ring_rand_u32() & 1u) != 0;
}
