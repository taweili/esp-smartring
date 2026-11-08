/*
 * Host-test stub for esp_heap_caps.h — plain malloc on the host.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0

void *heap_caps_malloc(size_t size, uint32_t caps);
void  heap_caps_free(void *ptr);
