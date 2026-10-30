/*
 * ringkit — touch. See ring_touch.h.
 */
#include "ringkit/ring_touch.h"

#include "bsp/smartring_plus.h"

esp_lcd_touch_handle_t ring_touch_handle(void)
{
    return bsp_touch_get_handle();
}
