/*
 * ringkit — embedded trust anchor. See ring_ca.h.
 */
#include "ringkit/ring_ca.h"

#include <stdint.h>

/* Embedded by EMBED_TXTFILES assets/qianwen_ca.pem. */
extern const uint8_t qianwen_ca_pem_start[] asm("_binary_qianwen_ca_pem_start");
extern const uint8_t qianwen_ca_pem_end[]   asm("_binary_qianwen_ca_pem_end");

const char *ring_ca_pem(void)
{
    return (const char *)qianwen_ca_pem_start;
}

size_t ring_ca_pem_len(void)
{
    return (size_t)(qianwen_ca_pem_end - qianwen_ca_pem_start);
}
