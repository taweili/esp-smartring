/*
 * ringkit — NVS-backed secrets with a build-time fallback.
 *
 * The pattern is shared by every app that talks to a cloud API: a value
 * normally lives in NVS (so it survives reflashing and can be changed without
 * rebuilding), but the first boot — or a build from a clean checkout — has
 * nothing stored yet, so the value compiled in from the generated
 * `ring_secrets.h` (itself sourced from the git-ignored `secrets.local`) is
 * used and persisted.
 *
 * This is the same split `ring_wifi` uses for SSID/password; it lives here so
 * the Qianwen API key (and any future cloud credential) does not need its own
 * copy of the NVS dance in every app.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read a secret from NVS, falling back to a build-time value.
 *
 * Precedence: NVS if present, else @p build_fallback. When the fallback is
 * used it is written back to NVS, so later builds do not need it on the
 * command line.
 *
 * @param ns             NVS namespace, e.g. "qianwen".
 * @param key            NVS key, e.g. "api_key".
 * @param build_fallback Value compiled in from `ring_secrets.h`; pass "" when
 *                       unset. Never committed.
 * @param out            Destination buffer.
 * @param len            Size of @p out.
 *
 * @return ESP_OK when @p out holds a non-empty value, ESP_ERR_NOT_FOUND when
 *         neither NVS nor the fallback supplied one.
 */
esp_err_t ring_secret_get(const char *ns, const char *key,
                          const char *build_fallback, char *out, size_t len);

#ifdef __cplusplus
}
#endif
