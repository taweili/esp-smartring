/*
 * ringkit — power. See ring_power.h.
 */
#include "ringkit/ring_power.h"

#include "bsp/smartring_plus.h"

void ring_power_shutdown(void)
{
    bsp_power_shutdown();
}
