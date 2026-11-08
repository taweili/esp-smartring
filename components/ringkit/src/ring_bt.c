/*
 * ringkit — Bluetooth LE peripheral host (NimBLE). See ring_bt.h.
 *
 * Design notes:
 *  - One host task (nimble_port_freertos_init) owns all NimBLE callbacks.
 *    GAP events (connect/disconnect/subscribe) and GATT access (read/write)
 *    all run there; ringkit only mirrors state under a mutex.
 *  - The GATT database is built once at ring_bt_init() from the app's
 *    declarative table. NimBLE requires the defs before the host starts, so
 *    ring_bt_init() must complete before the host task begins advertising.
 *  - ring_bt_notify() stores the value in the characteristic's internal
 *    buffer and calls ble_gatts_chr_updated(), which is safe from any task
 *    (it just schedules the transmission in the host task).
 *
 * This file owns the public API, init and the host task. The GATT database
 * and access callbacks live in ring_bt_gatt.c; GAP events and advertising
 * live in ring_bt_gap.c.
 */
#include "ring_bt_priv.h"

#include "ringkit/ring_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "ring_bt"

/* Declared here (not in a public NimBLE header); the definition lives in the
 * store/config NimBLE source compiled into the `bt` component. */
void ble_store_config_init(void);

ring_bt_ctx_t s_ctx;
SemaphoreHandle_t s_mutex;

void ring_bt_state_lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void ring_bt_state_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

/* --- host task ---------------------------------------------------------------- */

void ring_bt_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "host task started");
    nimble_port_run();   /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* --- public API ---------------------------------------------------------------- */

esp_err_t ring_bt_init(const ring_bt_config_t *cfg)
{
    if (cfg == NULL || cfg->name == NULL || cfg->svcs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.initialised) {
        return ESP_OK;   /* idempotent */
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    /* NVS (PHY calibration + bonding keys). Handle a clobbered flash. */
    ESP_RETURN_ON_ERROR(ring_nvs_init(), TAG, "nvs_flash_init failed");

    /* Copy the config into the context (it must be stable for the whole boot). */
    snprintf(s_ctx.name, sizeof(s_ctx.name), "%s", cfg->name);
    s_ctx.svcs = cfg->svcs;
    s_ctx.adv_uuid = cfg->adv_uuid;
    s_ctx.bonding = cfg->bonding;
    s_ctx.mitm = cfg->mitm;
    s_ctx.secure_conn = cfg->secure_conn;
    s_ctx.io_cap = cfg->io_cap;
    s_ctx.on_connect = cfg->on_connect;
    s_ctx.on_disconnect = cfg->on_disconnect;
    s_ctx.user = cfg->user;
    s_ctx.state = RING_BT_OFF;
    s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ctx.connected = false;

    ESP_RETURN_ON_ERROR(ring_bt_build_database(cfg), TAG, "bad service table");

    esp_err_t nimble_err = nimble_port_init();
    ESP_RETURN_ON_ERROR(nimble_err, TAG, "nimble_port_init failed: %s",
                        esp_err_to_name(nimble_err));

    /* Host config: callbacks are the whole programming model. */
    ble_hs_cfg.reset_cb = ring_bt_on_reset;
    ble_hs_cfg.sync_cb = ring_bt_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Security. */
    ble_hs_cfg.sm_io_cap = s_ctx.io_cap;
    ble_hs_cfg.sm_bonding = s_ctx.bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = s_ctx.mitm ? 1 : 0;
    ble_hs_cfg.sm_sc = s_ctx.secure_conn ? 1 : 0;
    if (s_ctx.bonding) {
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    }

    /* GATT database (must be before the host starts). */
    ESP_RETURN_ON_ERROR(ring_bt_register_gatt_database(), TAG, "GATT register failed");

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_ctx.name);

    /* Bonding storage (RAM now; NVS if CONFIG_BT_NIMBLE_NVS_PERSIST). */
    ble_store_config_init();

    nimble_port_freertos_init(ring_bt_host_task);

    ring_bt_state_lock();
    s_ctx.initialised = true;
    ring_bt_state_unlock();

    ESP_LOGI(TAG, "initialised as '%s'", s_ctx.name);
    return ESP_OK;
}

esp_err_t ring_bt_start_advertising(void)
{
    if (!s_ctx.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    ring_bt_state_lock();
    s_ctx.want_adv = true;
    ring_bt_state_unlock();
    ring_bt_advertise();
    return ESP_OK;
}

esp_err_t ring_bt_stop_advertising(void)
{
    if (!s_ctx.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    ring_bt_state_lock();
    s_ctx.want_adv = false;
    ring_bt_state_unlock();

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_stop rc=%d", rc);
    }
    ring_bt_state_lock();
    if (!s_ctx.connected) {
        s_ctx.state = RING_BT_IDLE;
    }
    ring_bt_state_unlock();
    return ESP_OK;
}

esp_err_t ring_bt_set_name(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        return ESP_FAIL;
    }
    ring_bt_state_lock();
    snprintf(s_ctx.name, sizeof(s_ctx.name), "%s", name);
    ring_bt_state_unlock();

    /* Restart advertising so the new name is in the next payload. */
    int was_adv;
    ring_bt_state_lock();
    was_adv = (s_ctx.state == RING_BT_ADVERTISING);
    ring_bt_state_unlock();
    if (was_adv) {
        ble_gap_adv_stop();   /* ADV_COMPLETE -> ring_bt_advertise() re-advs */
    }
    return ESP_OK;
}

esp_err_t ring_bt_notify(uint16_t chr_uuid, const void *data, size_t len)
{
    if (data == NULL || len == 0 || len > RING_BT_MAX_VALUE) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t svc = 0; svc < s_ctx.num_svcs; svc++) {
        ring_bt_svc_state_t *ss = &s_ctx.svc_states[svc];
        for (uint16_t c = 0; c < ss->num_chars; c++) {
            ring_bt_chr_state_t *cs = &ss->states[c];
            if (cs->uuid != chr_uuid) {
                continue;
            }
            memcpy(cs->value, data, len);
            cs->value_len = len;

            /* ble_gatts_chr_updated() is safe from any task: it schedules the
             * notification in the host task for every subscribed peer. */
            if (cs->flags & (RING_BT_CHR_NOTIFY | RING_BT_CHR_INDICATE)) {
                ble_gatts_chr_updated(cs->val_handle);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

ring_bt_state_t ring_bt_state(void)
{
    ring_bt_state_t st;
    ring_bt_state_lock();
    st = s_ctx.state;
    ring_bt_state_unlock();
    return st;
}

bool ring_bt_is_connected(void)
{
    bool c;
    ring_bt_state_lock();
    c = s_ctx.connected;
    ring_bt_state_unlock();
    return c;
}

uint16_t ring_bt_conn_handle(void)
{
    uint16_t h;
    ring_bt_state_lock();
    h = s_ctx.conn_handle;
    ring_bt_state_unlock();
    return h;
}
