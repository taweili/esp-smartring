/*
 * ringkit — ring_bt internal interface.
 *
 * Private to the ringkit component. The BLE peripheral host is split across
 * three translation units: ring_bt.c (public API, init, state), ring_bt_gatt.c
 * (GATT database + access callbacks) and ring_bt_gap.c (GAP events +
 * advertising). This header is the shared contract between them.
 *
 * Not part of the public API — applications include ringkit/ring_bt.h only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "host/ble_hs.h"

#include "ringkit/ring_bt.h"

/* --- internal limits ----------------------------------------------------- */

#define RING_BT_MAX_SVCS 4
#define RING_BT_MAX_CHRS 8
#define RING_BT_MAX_VALUE 64   /* ring_bt_chr_t::max_len <= this */

/* --- per-characteristic runtime state ------------------------------------ */

typedef struct {
    uint16_t uuid;
    uint8_t  flags;
    uint16_t max_len;
    ring_bt_access_cb_t cb;
    void *user;

    uint16_t val_handle;      /* filled by NimBLE at registration */
    uint8_t  value[RING_BT_MAX_VALUE];
    uint16_t value_len;
    bool     subscribed;      /* a peer enabled notify/indicate */
} ring_bt_chr_state_t;

typedef struct {
    uint16_t svc_uuid;
    const ring_bt_chr_t *app_chars;  /* the app's const table */
    ring_bt_chr_state_t states[RING_BT_MAX_CHRS];
    uint16_t num_chars;
} ring_bt_svc_state_t;

/* --- context ------------------------------------------------------------- */

typedef struct {
    bool initialised;
    bool synced;
    bool want_adv;            /* start advertising once synced */
    char name[32];

    ring_bt_state_t state;
    uint16_t conn_handle;
    bool connected;

    const ring_bt_svc_t *svcs;   /* app table (NULL-terminated) */
    uint16_t adv_uuid;
    bool bonding, mitm, secure_conn;
    uint8_t io_cap;
    ring_bt_conn_cb_t on_connect, on_disconnect;
    void *user;

    ring_bt_svc_state_t svc_states[RING_BT_MAX_SVCS];
    uint16_t num_svcs;
} ring_bt_ctx_t;

extern ring_bt_ctx_t s_ctx;
extern SemaphoreHandle_t s_mutex;

/* ------------------------------------------------------------- shared helpers
 *
 * Defined in ring_bt.c unless noted. */

void ring_bt_state_lock(void);
void ring_bt_state_unlock(void);

/* GATT (defined in ring_bt_gatt.c). */
ble_gatt_chr_flags ring_bt_map_flags(uint8_t f);
esp_err_t ring_bt_build_database(const ring_bt_config_t *cfg);
esp_err_t ring_bt_register_gatt_database(void);
int ring_bt_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);

/* GAP (defined in ring_bt_gap.c). */
void ring_bt_on_reset(int reason);
void ring_bt_on_sync(void);
void ring_bt_advertise(void);
int ring_bt_gap_event(struct ble_gap_event *event, void *arg);

/* Host task (defined in ring_bt.c). */
void ring_bt_host_task(void *param);
