/*
 * ringkit — SD card. See ring_sd.h.
 */
#include "ringkit/ring_sd.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/smartring_plus.h"

bool ring_sd_is_mounted(void)
{
    return bsp_sd_is_mounted();
}

void ring_sd_deinit(void)
{
    bsp_sd_deinit();
}

const sdmmc_card_t *ring_sd_card(void)
{
    return bsp_sd_get_card();
}

esp_err_t ring_sd_list(const char *path, ring_sd_entry_t **entries, size_t *count)
{
    if (entries == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_sd_entry_t *src = NULL;
    size_t n = 0;
    esp_err_t err = bsp_sd_list(path, &src, &n);
    if (err != ESP_OK) {
        return err;
    }

    /* Convert to ringkit types so applications never see BSP structs. */
    ring_sd_entry_t *dst = calloc(n ? n : 1, sizeof(*dst));
    if (dst == NULL) {
        bsp_sd_free_list(src, n);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < n; i++) {
        dst[i].name = strdup(src[i].name != NULL ? src[i].name : "");
        dst[i].type = (src[i].type == BSP_SD_ENTRY_DIR) ? RING_SD_DIR : RING_SD_FILE;
    }

    bsp_sd_free_list(src, n);
    *entries = dst;
    *count   = n;
    return ESP_OK;
}

void ring_sd_free_list(ring_sd_entry_t *entries, size_t count)
{
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}
