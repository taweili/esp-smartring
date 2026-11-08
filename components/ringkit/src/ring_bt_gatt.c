/*
 * ringkit — ring_bt GATT database and access callbacks.
 *
 * Split out of ring_bt.c. The database is built once from the app's
 * declarative table; one access callback serves every characteristic.
 * See ring_bt_priv.h.
 */
#include "ring_bt_priv.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"

#define TAG "ring_bt"

/* --- GATT flag mapping ---------------------------------------------------- */

ble_gatt_chr_flags ring_bt_map_flags(uint8_t f)
{
    ble_gatt_chr_flags out = 0;
    if (f & RING_BT_CHR_READ)     out |= BLE_GATT_CHR_F_READ;
    if (f & RING_BT_CHR_WRITE)    out |= BLE_GATT_CHR_F_WRITE;
    if (f & RING_BT_CHR_NOTIFY)   out |= BLE_GATT_CHR_F_NOTIFY;
    if (f & RING_BT_CHR_INDICATE) out |= BLE_GATT_CHR_F_INDICATE;
    return out;
}

/* --- GATT access ------------------------------------------------------------ */

/**
 * One access callback for every characteristic. `arg` is the
 * ring_bt_chr_state_t. Reads serve the app callback if present, else the last
 * stored value; writes store the payload and forward it to the app callback.
 */
int ring_bt_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    ring_bt_chr_state_t *st = (ring_bt_chr_state_t *)arg;
    (void)attr_handle;

    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (st->cb) {
            uint8_t buf[RING_BT_MAX_VALUE];
            size_t out_len = 0;
            esp_err_t err = st->cb(conn_handle, st->uuid, true,
                                   NULL, 0, buf, &out_len, st->user);
            if (err == ESP_OK && out_len > 0) {
                if (out_len > RING_BT_MAX_VALUE) {
                    out_len = RING_BT_MAX_VALUE;
                }
                int rc = os_mbuf_append(ctxt->om, buf, out_len);
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }
        /* Fall back to the last value stored by ring_bt_notify(). */
        int rc = os_mbuf_append(ctxt->om, st->value, st->value_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > RING_BT_MAX_VALUE) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t len = om_len;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, st->value, sizeof(st->value), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        st->value_len = len;
        if (st->cb) {
            esp_err_t err = st->cb(conn_handle, st->uuid, false,
                                   st->value, st->value_len, NULL, NULL, st->user);
            if (err != ESP_OK) {
                return BLE_ATT_ERR_UNLIKELY;
            }
        }
        /* Notify any subscribed peer that the value changed (like bleprph). */
        if (st->subscribed && (st->flags & (RING_BT_CHR_NOTIFY | RING_BT_CHR_INDICATE))) {
            ble_gatts_chr_updated(st->val_handle);
        }
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* --- GATT database construction --------------------------------------------- */

/** Build the internal svc/chr state from the app table. */
esp_err_t ring_bt_build_database(const ring_bt_config_t *cfg)
{
    s_ctx.num_svcs = 0;
    for (const ring_bt_svc_t *svc = cfg->svcs;
         svc->svc_uuid != 0 && s_ctx.num_svcs < RING_BT_MAX_SVCS; svc++) {
        ring_bt_svc_state_t *ss = &s_ctx.svc_states[s_ctx.num_svcs];
        memset(ss, 0, sizeof(*ss));
        ss->svc_uuid = svc->svc_uuid;
        ss->app_chars = svc->chars;

        uint16_t n = 0;
        for (const ring_bt_chr_t *chr = svc->chars;
             chr->uuid != 0 && n < RING_BT_MAX_CHRS; chr++, n++) {
            ring_bt_chr_state_t *cs = &ss->states[n];
            cs->uuid = chr->uuid;
            cs->flags = chr->flags;
            cs->max_len = chr->max_len;
            cs->cb = chr->cb;
            cs->user = chr->user;
            cs->value_len = 0;
        }
        ss->num_chars = n;
        s_ctx.num_svcs++;
    }
    return (s_ctx.num_svcs > 0) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/**
 * Build the NimBLE struct ble_gatt_svc_def[] / ble_gatt_chr_def[] from the
 * internal state, then count + register. Needs stable storage, so we keep
 * static arrays (one entry per service/chr, plus the NULL terminators).
 */
esp_err_t ring_bt_register_gatt_database(void)
{
    static struct ble_gatt_svc_def svc_defs[RING_BT_MAX_SVCS + 1];
    static struct ble_gatt_chr_def chr_defs[RING_BT_MAX_SVCS][RING_BT_MAX_CHRS + 1];
    static ble_uuid16_t svc_uuids[RING_BT_MAX_SVCS];
    static ble_uuid16_t chr_uuids[RING_BT_MAX_SVCS][RING_BT_MAX_CHRS];

    for (uint16_t svc = 0; svc < s_ctx.num_svcs; svc++) {
        ring_bt_svc_state_t *ss = &s_ctx.svc_states[svc];

        svc_uuids[svc] = (ble_uuid16_t)BLE_UUID16_INIT(ss->svc_uuid);
        svc_defs[svc].type = BLE_GATT_SVC_TYPE_PRIMARY;
        svc_defs[svc].uuid = &svc_uuids[svc].u;
        svc_defs[svc].characteristics = chr_defs[svc];

        for (uint16_t c = 0; c < ss->num_chars; c++) {
            ring_bt_chr_state_t *cs = &ss->states[c];
            chr_uuids[svc][c] = (ble_uuid16_t)BLE_UUID16_INIT(cs->uuid);
            chr_defs[svc][c].uuid = &chr_uuids[svc][c].u;
            chr_defs[svc][c].access_cb = ring_bt_gatt_access;
            chr_defs[svc][c].arg = cs;
            chr_defs[svc][c].flags = ring_bt_map_flags(cs->flags);
            chr_defs[svc][c].val_handle = &cs->val_handle;
            chr_defs[svc][c].descriptors = NULL;
        }
        memset(&chr_defs[svc][ss->num_chars], 0, sizeof(chr_defs[svc][0]));
    }
    memset(&svc_defs[s_ctx.num_svcs], 0, sizeof(svc_defs[0]));

    int rc = ble_gatts_count_cfg(svc_defs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(svc_defs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
