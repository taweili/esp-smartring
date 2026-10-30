/*
 * ringkit — Bluetooth LE peripheral host (NimBLE) for the Smart Ring Plus.
 *
 * This is a thin, self-contained wrapper around the ESP-IDF NimBLE host stack
 * that gives an application a BLE peripheral without it ever including a
 * nimble/ header. It:
 *
 *   - brings up NVS + the BT controller + the NimBLE host (one host task),
 *   - builds a GATT server from a simple table of services/characteristics,
 *   - advertises a device name (optionally a 16-bit service UUID), and
 *   - reports state (advertising / connected) thread-safely.
 *
 * The application supplies its GATT database declaratively at
 * ring_bt_init() and gets read/write callbacks per characteristic, so it never
 * touches struct ble_gatt_svc_def or struct ble_gap_event. It can push
 * notifications with ring_bt_notify().
 *
 * Design notes:
 *  - All NimBLE host callbacks (GAP + GATT access) run in the host task, which
 *    is NOT the LVGL task. Keep callbacks short; do UI work on your own timer.
 *  - ring_bt_notify() is safe from any task: it stores the value and schedules
 *    the notification, which the host task sends to subscribed peers.
 *  - The GATT database is fixed at init time (NimBLE requires the defs before
 *    the host starts). Use one ring_bt_init() per boot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BLE peripheral lifecycle, safe to read from any task. */
typedef enum {
    RING_BT_OFF = 0,   /**< not initialised (or stopped)          */
    RING_BT_IDLE,      /**< host up, not advertising              */
    RING_BT_ADVERTISING, /**< advertising, waiting for a peer     */
    RING_BT_CONNECTED, /**< link established                      */
} ring_bt_state_t;

/** Characteristic permission bits for ring_bt_chr_t::flags. */
#define RING_BT_CHR_READ     (1u << 0)
#define RING_BT_CHR_WRITE    (1u << 1)
#define RING_BT_CHR_NOTIFY   (1u << 2)
#define RING_BT_CHR_INDICATE (1u << 3)

/**
 * Per-characteristic access callback.
 *
 * @param conn_handle  the connection (BLE_HS_CONN_HANDLE_NONE during a stack
 *                     read, e.g. from ble_gatts_chr_updated()).
 * @param chr_uuid     the characteristic's 16-bit UUID.
 * @param is_read      true = peer read; false = peer wrote @p data/@p len.
 * @param data, len    on WRITE: the payload the peer sent.
 * @param out,out_len  on READ: fill *out (up to *out_len) with the value and
 *                     set *out_len to its length.
 * @param user         the ring_bt_chr_t::user pointer.
 *
 * @return ESP_OK to accept (write) / supply the value (read), or an error to
 *         reject the operation (write) / report BLE_ATT_ERR_INSUFFICIENT_RES.
 */
typedef esp_err_t (*ring_bt_access_cb_t)(uint16_t conn_handle, uint16_t chr_uuid,
                                         bool is_read, const uint8_t *data, size_t len,
                                         uint8_t *out, size_t *out_len, void *user);

/** One characteristic in a service. */
typedef struct {
    uint16_t uuid;          /**< 16-bit characteristic UUID (unique in the DB) */
    uint8_t  flags;         /**< RING_BT_CHR_* bits */
    uint16_t max_len;       /**< read buffer / max write payload, <= 64 */
    ring_bt_access_cb_t cb; /**< optional; NULL = served from last stored value */
    void *user;
} ring_bt_chr_t;

/** One primary service. @ref chars must be NULL-terminated. */
typedef struct {
    uint16_t svc_uuid;           /**< 16-bit service UUID */
    const ring_bt_chr_t *chars;
} ring_bt_svc_t;

/** Connection lifecycle callbacks (host task context). */
typedef void (*ring_bt_conn_cb_t)(uint16_t conn_handle, void *user);

/** Everything ring_bt needs at boot. Lives for the lifetime of the app. */
typedef struct {
    const char *name;             /**< advertised + GAP device name */
    const ring_bt_svc_t *svcs;    /**< NULL-terminated service table */
    uint16_t adv_uuid;            /**< 16-bit UUID to advertise, 0 = omit */
    bool bonding;                 /**< enable pairing + bonding   */
    bool mitm;                    /**< require MITM (needs io_cap) */
    bool secure_conn;             /**< LE Secure Connections only */
    uint8_t io_cap;               /**< BLE_SM_IO_CAP_* (NO_IO=3)  */
    ring_bt_conn_cb_t on_connect; /**< optional (host task)       */
    ring_bt_conn_cb_t on_disconnect; /**< optional (host task)    */
    void *user;                   /**< passed to the conn callbacks */
} ring_bt_config_t;

/**
 * @brief Initialise NVS + BT controller + NimBLE host and register the GATT DB.
 *
 * Starts the host task; advertising begins when the host syncs (or immediately
 * if ring_bt_start_advertising() was already requested). Idempotent. Should be
 * called once from ring_example_start(), after ring_board_init().
 */
esp_err_t ring_bt_init(const ring_bt_config_t *cfg);

/** Start (or restart) connectable undirected advertising. Non-blocking. */
esp_err_t ring_bt_start_advertising(void);

/** Stop advertising (keeps the host up; a peer stays connected if any). */
esp_err_t ring_bt_stop_advertising(void);

/** Change the advertised/GAP name (takes effect on the next adv cycle). */
esp_err_t ring_bt_set_name(const char *name);

/**
 * @brief Store @p data as the current value of @p chr_uuid and notify all
 * subscribed peers (no-op if nothing is subscribed or not connected).
 *
 * Safe from any task. The value is also what a later peer read returns if the
 * characteristic has no access callback.
 */
esp_err_t ring_bt_notify(uint16_t chr_uuid, const void *data, size_t len);

/** Current lifecycle state (thread safe). */
ring_bt_state_t ring_bt_state(void);

/** True when a BLE link is up. */
bool ring_bt_is_connected(void);

/** Current connection handle (0xFFFF = none). */
uint16_t ring_bt_conn_handle(void);

#ifdef __cplusplus
}
#endif
