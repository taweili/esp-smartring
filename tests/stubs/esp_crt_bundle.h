/*
 * Host-test stub for esp_crt_bundle.h — the bundle is never used on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t esp_crt_bundle_attach(void *conf);
