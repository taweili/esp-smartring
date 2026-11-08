/*
 * Host-test stub for freertos/semphr.h.
 *
 * Single-threaded tests do not need real synchronisation; the mutex is a
 * dummy non-NULL handle. Swap in pthread_mutex_t here if a thread-safety test
 * is ever added.
 */
#pragma once

#include <stdint.h>

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
int  xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks);
int  xSemaphoreGive(SemaphoreHandle_t mutex);
