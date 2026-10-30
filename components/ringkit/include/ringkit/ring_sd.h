/*
 * ringkit — SD card (SDMMC 4-bit + FAT).
 *
 * There is no card-detect pin on this board, so software cannot distinguish
 * "no card inserted" from "mount failed" — treat both as simply "not mounted".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "ringkit/ring_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount point ("/sdcard"). */
#define RING_SD_MOUNT "/sdcard"

/** TRUE when a card is mounted. */
bool ring_sd_is_mounted(void);

/** Unmount; safe to call when not mounted. */
void ring_sd_deinit(void);

/** Card handle for sdmmc_card_print_info debugging; NULL when unmounted. */
const sdmmc_card_t *ring_sd_card(void);

/**
 * @brief List a directory.
 *
 * Sorted: directories first, then files, each ascending (case-insensitive).
 * Returns the raw directory contents including FAT system entries — the caller
 * decides what to hide.
 *
 * @param path     absolute path, e.g. "/sdcard" or "/sdcard/DCIM"
 * @param entries  output array, allocated by callee — free with ring_sd_free_list()
 * @param count    number of entries
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when unmounted; ESP_ERR_NOT_FOUND when
 *         the path cannot be opened.
 */
esp_err_t ring_sd_list(const char *path, ring_sd_entry_t **entries, size_t *count);

/** Free an array returned by ring_sd_list(). */
void ring_sd_free_list(ring_sd_entry_t *entries, size_t count);

#ifdef __cplusplus
}
#endif
