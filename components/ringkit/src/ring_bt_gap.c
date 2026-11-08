/*
 * ringkit — ring_bt GAP events and advertising.
 *
 * Split out of ring_bt.c. All callbacks run on the NimBLE host task; state is
 * mirrored under the module mutex. See ring_bt_priv.h.
 */
#include "ring_bt_priv.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"

#define TAG "ring_bt"

/* --- host callbacks -------------------------------------------------------- */

void ring_bt_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    ring_bt_state_lock();
    s_ctx.state = RING_BT_OFF;
    s_ctx.connected = false;
    ring_bt_state_unlock();
}

/**
 * Host synced: the controller + host are up. Pick an identity address and
 * start advertising (if requested — it always is for a peripheral).
 */
void ring_bt_on_sync(void)
{
    uint8_t own_addr_type;
    uint8_t addr[6] = {0};
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "synced, BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    ring_bt_state_lock();
    s_ctx.synced = true;
    s_ctx.state = RING_BT_IDLE;
    bool adv = s_ctx.want_adv;
    ring_bt_state_unlock();

    if (adv) {
        ring_bt_advertise();
    }
}

/* --- advertising ----------------------------------------------------------- */

/**
 * Build and start connectable, general-discoverable advertising. If the host
 * is not synced yet, the request is remembered and started from on_sync().
 */
void ring_bt_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    uint8_t own_addr_type;
    int rc;

    ring_bt_state_lock();
    bool synced = s_ctx.synced;
    bool want = s_ctx.want_adv;
    char name[32];
    snprintf(name, sizeof(name), "%s", s_ctx.name);
    uint16_t adv_uuid = s_ctx.adv_uuid;
    ring_bt_state_unlock();

    if (!want) {
        return;   /* stop was requested; do not restart from ADV_COMPLETE */
    }
    if (!synced) {
        /* on_sync() re-runs this once the host is up. */
        return;
    }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    if (name[0]) {
        fields.name = (uint8_t *)name;
        fields.name_len = strlen(name);
        fields.name_is_complete = 1;
    }
    if (adv_uuid != 0) {
        fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(adv_uuid) };
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ring_bt_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }

    ring_bt_state_lock();
    s_ctx.state = RING_BT_ADVERTISING;
    ring_bt_state_unlock();
    ESP_LOGI(TAG, "advertising as '%s'", name);
}

/* --- GAP events ------------------------------------------------------------ */

int ring_bt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ring_bt_state_lock();
            s_ctx.connected = true;
            s_ctx.conn_handle = event->connect.conn_handle;
            s_ctx.state = RING_BT_CONNECTED;
            ring_bt_conn_cb_t cb = s_ctx.on_connect;
            void *user = s_ctx.user;
            ring_bt_state_unlock();
            ESP_LOGI(TAG, "connected, handle=%d", event->connect.conn_handle);
            if (cb) cb(event->connect.conn_handle, user);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            ring_bt_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ring_bt_state_lock();
        s_ctx.connected = false;
        s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ring_bt_conn_cb_t cb = s_ctx.on_disconnect;
        void *user = s_ctx.user;
        ring_bt_state_unlock();
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        if (cb) cb(event->disconnect.conn.conn_handle, user);
        ring_bt_advertise();   /* resume advertising */
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete, reason=%d", event->adv_complete.reason);
        ring_bt_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe, handle=%d attr=%d cur_notify=%d cur_ind=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.cur_indicate);
        for (uint16_t svc = 0; svc < s_ctx.num_svcs; svc++) {
            ring_bt_svc_state_t *ss = &s_ctx.svc_states[svc];
            for (uint16_t c = 0; c < ss->num_chars; c++) {
                if (ss->states[c].val_handle == event->subscribe.attr_handle) {
                    ss->states[c].subscribed =
                        event->subscribe.cur_notify || event->subscribe.cur_indicate;
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated, handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        ESP_LOGI(TAG, "notify tx, handle=%d attr=%d status=%d",
                 event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                 event->notify_tx.status);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc change, status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Already bonded; the peer re-paired. Drop the old bond and retry. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        pkey.action = event->passkey.params.action;
        switch (event->passkey.params.action) {
        case BLE_SM_IOACT_DISP:
            /* Show on the peer (or here) — inject a fixed demo passkey. */
            pkey.passkey = 123456;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "passkey 123456 injected");
            break;
        case BLE_SM_IOACT_NUMCMP:
            /* Auto-accept numeric comparison for a button-less ring. */
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            break;
        default:
            break;
        }
        return 0;
    }

    default:
        return 0;
    }
}
