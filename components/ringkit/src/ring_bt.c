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
 */
#include "ringkit/ring_bt.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "nvs_flash.h"
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

static ring_bt_ctx_t s_ctx;
static SemaphoreHandle_t s_mutex;

static void state_lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void state_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

/* --- forward decls -------------------------------------------------------- */

static void ring_bt_advertise(void);
static int  ring_bt_gap_event(struct ble_gap_event *event, void *arg);
static int  ring_bt_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

/* --- GATT flag mapping ---------------------------------------------------- */

static ble_gatt_chr_flags map_flags(uint8_t f)
{
    ble_gatt_chr_flags out = 0;
    if (f & RING_BT_CHR_READ)     out |= BLE_GATT_CHR_F_READ;
    if (f & RING_BT_CHR_WRITE)    out |= BLE_GATT_CHR_F_WRITE;
    if (f & RING_BT_CHR_NOTIFY)   out |= BLE_GATT_CHR_F_NOTIFY;
    if (f & RING_BT_CHR_INDICATE) out |= BLE_GATT_CHR_F_INDICATE;
    return out;
}

/* --- host callbacks -------------------------------------------------------- */

static void ring_bt_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    state_lock();
    s_ctx.state = RING_BT_OFF;
    s_ctx.connected = false;
    state_unlock();
}

/**
 * Host synced: the controller + host are up. Pick an identity address and
 * start advertising (if requested — it always is for a peripheral).
 */
static void ring_bt_on_sync(void)
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

    state_lock();
    s_ctx.synced = true;
    s_ctx.state = RING_BT_IDLE;
    bool adv = s_ctx.want_adv;
    state_unlock();

    if (adv) {
        ring_bt_advertise();
    }
}

/* --- advertising ----------------------------------------------------------- */

/**
 * Build and start connectable, general-discoverable advertising. If the host
 * is not synced yet, the request is remembered and started from on_sync().
 */
static void ring_bt_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    uint8_t own_addr_type;
    int rc;

    state_lock();
    bool synced = s_ctx.synced;
    bool want = s_ctx.want_adv;
    char name[32];
    snprintf(name, sizeof(name), "%s", s_ctx.name);
    uint16_t adv_uuid = s_ctx.adv_uuid;
    state_unlock();

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

    state_lock();
    s_ctx.state = RING_BT_ADVERTISING;
    state_unlock();
    ESP_LOGI(TAG, "advertising as '%s'", name);
}

/* --- GAP events ------------------------------------------------------------ */

static int ring_bt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            state_lock();
            s_ctx.connected = true;
            s_ctx.conn_handle = event->connect.conn_handle;
            s_ctx.state = RING_BT_CONNECTED;
            ring_bt_conn_cb_t cb = s_ctx.on_connect;
            void *user = s_ctx.user;
            state_unlock();
            ESP_LOGI(TAG, "connected, handle=%d", event->connect.conn_handle);
            if (cb) cb(event->connect.conn_handle, user);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            ring_bt_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        state_lock();
        s_ctx.connected = false;
        s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ring_bt_conn_cb_t cb = s_ctx.on_disconnect;
        void *user = s_ctx.user;
        state_unlock();
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
        for (uint16_t s = 0; s < s_ctx.num_svcs; s++) {
            ring_bt_svc_state_t *svc = &s_ctx.svc_states[s];
            for (uint16_t c = 0; c < svc->num_chars; c++) {
                if (svc->states[c].val_handle == event->subscribe.attr_handle) {
                    svc->states[c].subscribed =
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

/* --- GATT access ------------------------------------------------------------ */

/**
 * One access callback for every characteristic. `arg` is the
 * ring_bt_chr_state_t. Reads serve the app callback if present, else the last
 * stored value; writes store the payload and forward it to the app callback.
 */
static int ring_bt_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    ring_bt_chr_state_t *st = (ring_bt_chr_state_t *)arg;
    (void)attr_handle;

    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (st->cb) {
            uint8_t buf[RING_BT_MAX_VALUE];
            size_t out_len = st->max_len > RING_BT_MAX_VALUE ? RING_BT_MAX_VALUE
                                                             : st->max_len;
            esp_err_t err = st->cb(conn_handle, st->uuid, true,
                                   NULL, 0, buf, &out_len, st->user);
            if (err == ESP_OK && out_len > 0) {
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
static esp_err_t build_database(const ring_bt_config_t *cfg)
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
static esp_err_t register_gatt_database(void)
{
    static struct ble_gatt_svc_def svc_defs[RING_BT_MAX_SVCS + 1];
    static struct ble_gatt_chr_def chr_defs[RING_BT_MAX_SVCS][RING_BT_MAX_CHRS + 1];
    static ble_uuid16_t svc_uuids[RING_BT_MAX_SVCS];
    static ble_uuid16_t chr_uuids[RING_BT_MAX_SVCS][RING_BT_MAX_CHRS];

    for (uint16_t s = 0; s < s_ctx.num_svcs; s++) {
        ring_bt_svc_state_t *ss = &s_ctx.svc_states[s];

        svc_uuids[s] = (ble_uuid16_t)BLE_UUID16_INIT(ss->svc_uuid);
        svc_defs[s].type = BLE_GATT_SVC_TYPE_PRIMARY;
        svc_defs[s].uuid = &svc_uuids[s].u;
        svc_defs[s].characteristics = chr_defs[s];

        for (uint16_t c = 0; c < ss->num_chars; c++) {
            ring_bt_chr_state_t *cs = &ss->states[c];
            chr_uuids[s][c] = (ble_uuid16_t)BLE_UUID16_INIT(cs->uuid);
            chr_defs[s][c].uuid = &chr_uuids[s][c].u;
            chr_defs[s][c].access_cb = ring_bt_gatt_access;
            chr_defs[s][c].arg = cs;
            chr_defs[s][c].flags = map_flags(cs->flags);
            chr_defs[s][c].val_handle = &cs->val_handle;
            chr_defs[s][c].descriptors = NULL;
        }
        memset(&chr_defs[s][ss->num_chars], 0, sizeof(chr_defs[s][0]));
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

/* --- host task ---------------------------------------------------------------- */

static void ring_bt_host_task(void *param)
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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs re-format, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));

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

    ESP_RETURN_ON_ERROR(build_database(cfg), TAG, "bad service table");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init failed: %s",
                        esp_err_to_name(err));

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
    ESP_RETURN_ON_ERROR(register_gatt_database(), TAG, "GATT register failed");

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_ctx.name);

    /* Bonding storage (RAM now; NVS if CONFIG_BT_NIMBLE_NVS_PERSIST). */
    ble_store_config_init();

    nimble_port_freertos_init(ring_bt_host_task);

    state_lock();
    s_ctx.initialised = true;
    state_unlock();

    ESP_LOGI(TAG, "initialised as '%s'", s_ctx.name);
    return ESP_OK;
}

esp_err_t ring_bt_start_advertising(void)
{
    if (!s_ctx.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    state_lock();
    s_ctx.want_adv = true;
    state_unlock();
    ring_bt_advertise();
    return ESP_OK;
}

esp_err_t ring_bt_stop_advertising(void)
{
    if (!s_ctx.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    state_lock();
    s_ctx.want_adv = false;
    state_unlock();

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_stop rc=%d", rc);
    }
    state_lock();
    if (!s_ctx.connected) {
        s_ctx.state = RING_BT_IDLE;
    }
    state_unlock();
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
    state_lock();
    snprintf(s_ctx.name, sizeof(s_ctx.name), "%s", name);
    state_unlock();

    /* Restart advertising so the new name is in the next payload. */
    int was_adv;
    state_lock();
    was_adv = (s_ctx.state == RING_BT_ADVERTISING);
    state_unlock();
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

    for (uint16_t s = 0; s < s_ctx.num_svcs; s++) {
        ring_bt_svc_state_t *svc = &s_ctx.svc_states[s];
        for (uint16_t c = 0; c < svc->num_chars; c++) {
            ring_bt_chr_state_t *cs = &svc->states[c];
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
    state_lock();
    st = s_ctx.state;
    state_unlock();
    return st;
}

bool ring_bt_is_connected(void)
{
    bool c;
    state_lock();
    c = s_ctx.connected;
    state_unlock();
    return c;
}

uint16_t ring_bt_conn_handle(void)
{
    uint16_t h;
    state_lock();
    h = s_ctx.conn_handle;
    state_unlock();
    return h;
}
