/*
 * ringkit — display. See ring_display.h.
 */
#include "ringkit/ring_display.h"

#include "bsp/smartring_plus.h"

void ring_display_backlight(uint8_t percent)
{
    bsp_display_backlight_set(percent);
}

lv_display_t *ring_display_handle(void)
{
    return bsp_display_get_handle();
}

void ring_display_set_swap_bytes(bool enable)
{
    bsp_display_set_swap_bytes(enable);
}

bool ring_display_get_swap_bytes(void)
{
    return bsp_display_get_swap_bytes();
}
