/*
 * ringkit — embedded trust anchor for the Qianwen endpoint.
 *
 * The server sends only its leaf certificate, so the intermediate CA has to be
 * supplied locally. The PEM is embedded once by the build
 * (EMBED_TXTFILES assets/qianwen_ca.pem) and every module that talks to
 * maas.qianwenaiapi.com shares it through here.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Null-terminated PEM chain. Valid for the lifetime of the firmware. */
const char *ring_ca_pem(void);

/** Bytes in the PEM chain (excluding any terminator). */
size_t ring_ca_pem_len(void);

#ifdef __cplusplus
}
#endif
