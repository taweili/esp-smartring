/*
 * ringkit — shared FreeRTOS stack-overflow hook.
 *
 * FreeRTOS calls this on stack overflow. The stock hook aborts with an
 * unusable backtrace; name the offending task and its headroom instead.
 *
 * This is shared by every ringkit module: ring_net and ring_geodis both used
 * to define it, which would be a duplicate symbol when both are compiled.
 */
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ringkit"

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_DRAM_LOGE(TAG, "*** STACK OVERFLOW in task '%s' (hwm %u words)",
                  pcTaskName, (unsigned)uxTaskGetStackHighWaterMark(xTask));
    abort();
}
