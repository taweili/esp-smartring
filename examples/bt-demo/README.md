# bt-demo — Bluetooth LE peripheral bring-up on the round screen

Design & implementation guide for `examples/bt-demo/`, the Bluetooth LE
peripheral demo on the Smart Ring Plus. This is also the project's **reference
for "how do I use Bluetooth on the ring?"** — it covers what the silicon
supports, which stack to pick, the exact `sdkconfig` knobs for **ESP-IDF v6.1 /
esp32s3**, a minimal GATT-server bring-up adapted to the `ringkit` skeleton,
Wi-Fi coexistence, and the board-specific pitfalls (RAM, partitions, NVS, core
pinning).

> The BSP (`viewesmart/smartring_plus`) does **not** touch the radio —
> Bluetooth is a pure ESP-IDF feature, exactly like the Wi-Fi that
> `ringkit/ring_wifi.c` already wraps. Nothing here requires a BSP change.

Target: VIEWE Smart Ring Plus, ESP32-S3-N16R8, ESP-IDF v6.1, NimBLE host.
Implementation: `examples/bt-demo/bt_demo.c` + the `ring_bt` wrapper in
`components/ringkit/`.

---

## 1. What the app does

The ring advertises itself as a connectable BLE peripheral named **`SmartRing`**
with one vendor service (**0xFFE0**) and three characteristics:

| Char | UUID | Access | Meaning |
|------|------|--------|---------|
| name | `0xFFE1` | READ | the device name (`"SmartRing"`) |
| battery | `0xFFE2` | READ \| NOTIFY | battery %, updated every 2 s |
| command | `0xFFE3` | WRITE \| NOTIFY | write a command, get an `"ok"` notification |

A phone (nRF Connect / LightBlue) can scan for `SmartRing`, connect, pair
(bonding is on), read the name/battery, write a command and receive a
notification. The screen shows the connection state, battery, and last received
command; a 2 Hz UI timer refreshes it.

The connection itself is asynchronous: `ring_bt_init()` starts the host task and
advertising starts when the host syncs. The example just reads
`ring_bt_state()` / `ring_bt_is_connected()` from a UI timer.

## 2. What the hardware supports (silicon, not marketing)

- **SoC:** ESP32-S3-N16R8 (240 MHz dual-core, 8 MB octal PSRAM, 16 MB flash).
- **Radio:** 2.4 GHz Wi-Fi (802.11 b/g/n) **+ Bluetooth Low Energy only**.
  The ESP32-S3 has **no Bluetooth Classic / BR-EDR** controller — there is no
  BT 3.0, no A2DP sink, no SPP-over-BR. Anything you build is **BLE**.
- **BLE version:** the controller is a **Bluetooth 5.0 (LE)** controller, and the
  SoC is **certified for Bluetooth LE 5.4**. What you get in practice:
  - 1 Mbps / 2 Mbps PHY (2M PHY is the headline 5.0 feature).
  - Advertising extensions / extended advertising (optional compile-time,
    `CONFIG_BT_NIMBLE_EXT_ADV`), multiple advertising sets, periodic advertising.
  - LE Coded PHY (long range) is configurable on S3 but **costs Wi-Fi throughput**
    when both are running (see §7).
  - No BLE audio / ISO / LC3 (that is ESP32-H4); no 802.15.4.
- **Antenna:** the on-module PCB antenna — no external wiring needed, nothing to
  configure. This is the same RF front-end Wi-Fi uses, so BT and Wi-Fi **time-share**
  the radio (see §7).

**Takeaway:** treat the ring as a **BLE peripheral/central**, not a Bluetooth
classic device. For phone-app UIs, sensor streams, or provisioning, BLE is the
only path.

## 3. Stack choice: NimBLE vs Bluedroid

ESP-IDF v6.1 ships **two** BLE host stacks over the same ESP32-S3 controller
(`components/bt/host/{bluedroid,nimble}`, controller at `components/bt/controller/esp32s3`):

| | ESP-NimBLE | ESP-Bluedroid |
|---|---|---|
| Origin | Apache Mynewt NimBLE (ported) | Modified Android Bluedroid |
| Bluetooth Classic | ❌ (LE only) | ❌ on ESP32-S3 (LE only; Classic is ESP32-only) |
| Heap / flash | **Smaller** | Larger |
| API style | C, event-callback (`ble_gap_event`) | `esp_ble_gatts_*` / `esp_ble_gap_*` |
| Best for | BLE-only, resource-constrained (this board) | Dual-mode, legacy porting, BLE Mesh familiarity |

**Recommendation: NimBLE.** The ring is BLE-only, the device is RAM-tight (internal
heap, UI + LVGL + possibly Wi-Fi), and NimBLE is the stack Espressif's own BLE
examples default to. Bluedroid buys nothing here except a larger footprint and a
bigger API surface.

> The BLE **controller** is the same either way (`esp_bt_controller_*` under the
> hood). On ESP32-S3 it is inherently BLE-only — `BT_CTRL_MODE_EFF=1`. The
> `CONFIG_BTDM_CTRL_MODE_*` options in some examples are **ESP32-only**; on S3 they
> are silently ignored (the S3 controller Kconfig comes from
> `components/bt/controller/esp32c3/Kconfig.in`, which has no dual-mode option).

## 4. The minimal `sdkconfig` delta (verified on ESP-IDF v6.1 / esp32s3)

Generated from a real ESP32-S3 `sdkconfig` from the canonical
`examples/bluetooth/nimble/bleprph` example. Add to `sdkconfig.defaults` (only
the first three are strictly required; the rest are the defaults the example
relies on, listed so you know what you're getting):

```ini
# --- BLE: enable the controller + NimBLE host -------------------------------
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y            # (Bluedroid stays off)
# CONFIG_BT_BLUEDROID_ENABLED is not set
CONFIG_BT_CONTROLLER_ENABLED=y

# --- NimBLE host (these are the defaults, shown for reference) --------------
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y       # you usually want BOTH roles
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3    # 3 = controller BT_CTRL_BLE_MAX_ACT default
CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y # 2M PHY / ext-adv *capability* (not the feature)
# CONFIG_BT_NIMBLE_EXT_ADV is not set # enable only if you use ext advertising
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y   # host buffers from internal DRAM
CONFIG_BT_NIMBLE_MEM_OPTIMIZATION=y
CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y

# --- Controller --------------------------------------------------------------
CONFIG_BT_CTRL_BLE_MAX_ACT=6          # 828 B each — see §6 to trim
CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0
CONFIG_BT_CTRL_PINNED_TO_CORE_0=y     # run BT host+controller on core 0 (see §7)
```

**Notes:**
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` **is** available (this project has
  `CONFIG_SPIRAM_USE_MALLOC=y`), and would put the *host* heap in PSRAM. But the
  **controller** memory cannot go to PSRAM, and BLE host data in PSRAM is slower
  and less cache-friendly. Given the 8 MB PSRAM is already the LVGL/Wi-Fi pool,
  prefer `INTERNAL` for BLE unless you measure a heap shortfall.
- `CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y` enables the 5.0 *capabilities* (2M PHY,
  extended adv APIs). `CONFIG_BT_NIMBLE_EXT_ADV` is the actual extended-advertising
  feature — leave it off unless you need it; it costs RAM.
- `nvs_flash_init()` is **required** (NimBLE stores PHY calibration and, if you
  enable it, bonding keys). `ring_wifi_init()` already initializes NVS, so if you
  use both, call `ring_wifi_init()` (or your own `nvs_flash_init()`) before BT.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` is already 8192 here; the NimBLE host runs on
  its **own** task (default 4096 B), not `app_main`.

There is **no** `CONFIG_BTDM_CTRL_MODE_BLE_ONLY` to set on S3 — that symbol is
ESP32-only and the example's `sdkconfig.defaults` carries it for other targets.

## 5. Bring-up sequence (what actually has to happen)

This is the exact lifecycle from `examples/bluetooth/nimble/bleprph/main/main.c`,
annotated for the ring. The critical ordering is **NVS → `nimble_port_init()` →
fill `ble_hs_cfg` → register GATT → set name → `nimble_port_freertos_init()`**;
advertising is kicked off from the host's `sync` callback.

```c
/* 1. NVS first — PHY calibration + (optional) bonding keys live here. */
esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
}
ESP_ERROR_CHECK(err);

/* 2. BT controller + NimBLE host in one call. Idempotent; safe to re-init. */
err = nimble_port_init();
ESP_ERROR_CHECK(err);

/* 3. Configure the host: callbacks are the whole programming model. */
ble_hs_cfg.reset_cb           = ring_bt_on_reset;
ble_hs_cfg.sync_cb            = ring_bt_on_sync;        /* -> start advertising */
ble_hs_cfg.gatts_register_cb  = ring_bt_gatt_register;  /* logs service handles */
ble_hs_cfg.store_status_cb    = ble_store_util_status_rr;

/* Optional security (Just Works is default; see §6 for pairing). */
ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_NO_IO;
ble_hs_cfg.sm_bonding = 1;
ble_hs_cfg.sm_mitm    = 0;
ble_hs_cfg.sm_sc      = 1;

/* 4. Build the GATT database (services/characteristics/descriptors). */
int rc = ring_bt_gatt_svr_init();
assert(rc == 0);

/* 5. Advertised device name (GAP service). */
rc = ble_svc_gap_device_name_set("smartring");
assert(rc == 0);

/* 6. Bonding storage. ble_store_config_init() = NVS-backed key store. */
ble_store_config_init();

/* 7. Start the host task. It runs nimble_port_run() and never returns. */
nimble_port_freertos_init(ring_bt_host_task);
```

`ring_bt_on_sync()` is where you start advertising:

```c
static int ring_bt_on_sync(void)
{
    /* Choose a public/random identity address. */
    uint8_t own_addr_type;
    int rc = ble_hs_util_ensure_addr(0);       /* 0 = prefer public */
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);

    /* Build the advertised payload. */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { return rc; }

    /* Start undirected connectable advertising. */
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                             &adv_params, ring_bt_gap_event, NULL);
}
```

The host task is just:

```c
static void ring_bt_host_task(void *param)
{
    (void)param;
    nimble_port_run();            /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}
```

### The GAP event callback drives the state machine

`ble_gap_adv_start()` returns immediately; **everything** (connection, disconnect,
MTU, subscribe, pairing) arrives as `struct ble_gap_event` in the callback you
passed. The canonical switch (from `bleprph`) is:

| Event | What you do |
|---|---|
| `BLE_GAP_EVENT_CONNECT` | status 0 → connection established; non-zero → resume advertising |
| `BLE_GAP_EVENT_DISCONNECT` | resume advertising (the peer walked away / was dropped) |
| `BLE_GAP_EVENT_SUBSCRIBE` | a peer subscribed to a notify/indicate char — track `cur_notify`/`cur_indicate` |
| `BLE_GAP_EVENT_MTU` | negotiated MTU grew — useful before large transfers |
| `BLE_GAP_EVENT_NOTIFY_TX` | a notification/indication was sent (or failed) |
| `BLE_GAP_EVENT_ENC_CHANGE` | encryption enabled/disabled for the link |
| `BLE_GAP_EVENT_ADV_COMPLETE` | adv window ended (usually means no connection) |
| `BLE_GAP_EVENT_PASSKEY_ACTION` | pairing UI — see §6 |
| `BLE_GAP_EVENT_REPEAT_PAIRING` | return `BLE_GAP_REPEAT_PAIRING_RETRY` (after `ble_store_util_delete_peer`) |

## 6. A minimal GATT server for the ring

NimBLE declares the database as a **static array** of `struct ble_gatt_svc_def`
(services → characteristics → descriptors), each with an `access_cb`. The callback
is invoked for every read/write, keyed on the attribute handle (or UUID).

A compact, ring-appropriate example — a **single custom service** with one readable
characteristic (e.g. firmware version) and one writable/notifiable one (e.g. a
command pipe the phone app writes to):

```c
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* --- Custom service: 32-byte command pipe + firmware version --------------- */

/* Service UUID (16-bit, e.g. vendor-assigned) */
#define RING_BT_SVC_UUID           0x180F  /* example: Battery Service */
#define RING_BT_CHR_FW_UUID        0x2A19  /* Battery Level — read + notify */
#define RING_BT_CHR_CMD_UUID       0x2A1C  /* custom use — write + notify  */

static uint8_t   s_cmd[64];
static uint16_t  s_cmd_len;
static uint16_t  s_cmd_val_handle;

static int ring_bt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* Return the value by appending it to ctxt->om. */
        return os_mbuf_append(ctxt->om, s_cmd, s_cmd_len)
                   ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* Copy the peer's value out of the mbuf, then act on it. */
        s_cmd_len = OS_MBUF_PKTLEN(ctxt->om);
        if (s_cmd_len > sizeof(s_cmd)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int rc = ble_hs_mbuf_to_flat(ctxt->om, s_cmd, sizeof(s_cmd), &s_cmd_len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        /* e.g. dispatch the command, then echo a notification: */
        ble_gatts_chr_updated(attr_handle);
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def ring_bt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(RING_BT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(RING_BT_CHR_CMD_UUID),
                .access_cb = ring_bt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_cmd_val_handle,
            },
            { 0 }, /* end of characteristics */
        },
    },
    { 0 }, /* end of services */
};

int ring_bt_gatt_svr_init(void)
{
    ble_svc_gap_init();            /* mandatory GAP service */
    ble_svc_gatt_init();           /* mandatory GATT service */

    int rc = ble_gatts_count_cfg(ring_bt_svcs);   /* pass 1: count attributes */
    if (rc != 0) { return rc; }
    return ble_gatts_add_svcs(ring_bt_svcs);      /* pass 2: register them */
}
```

**Key API facts:**
- `ble_gatts_count_cfg()` then `ble_gatts_add_svcs()` is always a **two-pass** call.
- Reads: **append** the value to `ctxt->om` with `os_mbuf_append()`.
- Writes: **pull** the value with `ble_hs_mbuf_to_flat()` (or `os_mbuf_copydata`).
- `ble_gatts_chr_updated(handle)` queues a notification/indication to **all**
  subscribed peers — call it whenever a notifiable value changes.
- A value write/read is keyed by `attr_handle` (or compare `ctxt->chr->uuid`).
- 128-bit UUIDs use `BLE_UUID128_INIT(...)` / `ble_uuid128_t`; 16-bit use
  `BLE_UUID16_INIT(...)` / `BLE_UUID16_DECLARE(...)`.

### Pairing / security (optional)

Defaults are **"Just Works", no bonding** — the simplest path that works with any
phone app (nRF Connect, LightBlue). If you need encryption or a PIN:

- Set `ble_hs_cfg.sm_io_cap` to e.g. `BLE_SM_IO_CAP_DISP_YES_NO`, `sm_bonding = 1`,
  `sm_mitm = 1`, `sm_sc = 1`.
- Handle `BLE_GAP_EVENT_PASSKEY_ACTION`: for `BLE_SM_IOACT_DISP` inject the passkey
  with `ble_sm_inject_io()`; for `BLE_SM_IOACT_NUMCMP` answer yes/no.
- For an encrypted GATT attribute, add `BLE_GATT_CHR_F_READ_ENC`/`WRITE_ENC` to the
  characteristic flags, or gate access in the `access_cb` on `ctxt` security state.
- **Bonding persistence is opt-in.** `ble_store_config_init()` registers the
  host's `store_read/write/delete` callbacks and keeps bonds in **RAM**. They only
  survive a reboot if you set `CONFIG_BT_NIMBLE_NVS_PERSIST=y` — that Kconfig maps
  to `MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST`, and without it the `ble_store_config_persist_*`
  functions compile to no-ops (the store writes nothing to NVS). Enable it when you
  need bonded reconnects without re-pairing; the store then writes through the
  `conf` subsystem to NVS (`ble_store_nvs.c`).

## 7. RAM & flash budget (the part that bites on this board)

This board is RAM-constrained in **internal** DRAM even with 8 MB PSRAM, because
the controller, LVGL, and the FreeRTOS heaps all want internal memory.

- **BT controller:** roughly tens of KB, allocated from internal DRAM. Each BLE
  "activity" (connection, scan, sync, adv instance) costs **~828 B** —
  `CONFIG_BT_CTRL_BLE_MAX_ACT=6` default ≈ 5 KB. Trim to 3–4 if you only need one
  link.
- **NimBLE host:** heap (host task stack 4096 B default, ATT/L2CAP buffers,
  transport ACLs). `CONFIG_BT_NIMBLE_MEM_OPTIMIZATION=y` and
  `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` are on by default and already reduce this.
- **Flash:** NimBLE host+controller add on the order of a few hundred KB to the
  app image. The project already uses
  `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (1500K factory) — **keep it**. A
  BLE+LVGL app fits, but if you also add Wi-Fi (esp_wifi + lwip + wpa_supplicant)
  and a big font set, watch the `factory` size (`idf.py size`).
- **PSRAM:** the LVGL display buffer and any Wi-Fi/lwip buffers already use PSRAM
  (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`). **Don't** move BLE host buffers to
  PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`) unless you measure a heap
  failure — controller RAM can't move, and PSRAM access is slower and adds cache
  pressure.

**Practical rule:** with a UI + BLE only, NimBLE + default controller config is
fine. With UI + BLE + Wi-Fi simultaneously, trim `BT_CTRL_BLE_MAX_ACT` (e.g. 3),
keep `BT_NIMBLE_MAX_CONNECTIONS=1` or 3, and reduce Wi-Fi buffers
(`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`, `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM`,
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM`) as documented in the coexistence page.

## 8. Wi-Fi + BLE coexistence (you *will* run both)

The ring has **one** 2.4 GHz radio. Wi-Fi and BLE time-share it; the coexistence
module arbitrates by priority. This is automatic — you don't write coexistence
code — but it shapes your config and expectations:

- **It works.** Every Wi-Fi-STA × BLE (scan/advertising/connected) combination is
  supported with stable performance. The one caveat: Wi-Fi in **SOFTAP / Sniffer /
  ESP-NOW (RX)** modes degrades BLE (marked `C1`/`S` in the docs).
- **Throughput is halved-ish when both are active.** In Wi-Fi-CONNECTED ×
  BLE-CONNECTED the time slice is ~50/50. Expect a BLE connection to feel slower
  while Wi-Fi streams, and vice-versa.
- **Pin the stacks apart.** Put BT host+controller on one core and Wi-Fi on the
  other. Recommended (from the coexistence guide):
  - `CONFIG_BT_CTRL_PINNED_TO_CORE_0=y` and `CONFIG_BT_NIMBLE_PINNED_TO_CORE_0=y`
    (both on core 0), and
  - `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y` (Wi-Fi protocol task on core 1).
  The ring's LVGL task runs alongside; keeping BT on core 0 and Wi-Fi on core 1
  avoids starving the UI. (The BSP's `esp_lvgl_adapter` uses its own task; check
  `CONFIG_ESP_LV_*` / the adapter's core option if you see UI stalls.)
- **LE Coded PHY** (long range) is bad for coexistence — a long BT packet blocks
  Wi-Fi. Leave Coded PHY off, or enable
  `CONFIG_BT_CTRL_COEX_PHY_CODED_TX_RX_TLIM_EN` to cap TX/RX time.
- Keep Wi-Fi connectionless power-save parameters at **defaults**; custom
  Window/Interval can steal RF time and hurt BLE.
- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` is normally not needed for plain Wi-Fi+BLE
  (only BLE-MESH needs the `esp_coex_status_bit_*` APIs).

The **reference example** is
`examples/bluetooth/nimble/bleprph_wifi_coex/` — it runs a GATT server and a Wi-Fi
STA at once, exactly the ring's future shape. Its `sdkconfig.defaults` is the same
BT block as §4 plus `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` and
`CONFIG_ESP_WIFI_IRAM_OPT=n`.

## 9. Where it plugs into `ringkit`

Bluetooth is **not** a BSP concern, so it lives in `ringkit` as a module that
mirrors `ring_wifi` — a thin wrapper the app calls, hiding all NimBLE headers.
It is **implemented** (`components/ringkit/src/ring_bt.c` +
`include/ringkit/ring_bt.h`) and exercised by this example. Public surface:

```c
/* ringkit/ring_bt.h */
typedef enum {
    RING_BT_OFF, RING_BT_IDLE, RING_BT_ADVERTISING, RING_BT_CONNECTED,
} ring_bt_state_t;

#define RING_BT_CHR_READ     (1u << 0)
#define RING_BT_CHR_WRITE    (1u << 1)
#define RING_BT_CHR_NOTIFY   (1u << 2)
#define RING_BT_CHR_INDICATE (1u << 3)

typedef esp_err_t (*ring_bt_access_cb_t)(uint16_t conn, uint16_t chr_uuid,
                                         bool is_read, const uint8_t *data, size_t len,
                                         uint8_t *out, size_t *out_len, void *user);
typedef struct { uint16_t uuid; uint8_t flags; uint16_t max_len;
                 ring_bt_access_cb_t cb; void *user; } ring_bt_chr_t;
typedef struct { uint16_t svc_uuid; const ring_bt_chr_t *chars; } ring_bt_svc_t;
typedef void (*ring_bt_conn_cb_t)(uint16_t conn_handle, void *user);
typedef struct {
    const char *name; const ring_bt_svc_t *svcs; uint16_t adv_uuid;
    bool bonding, mitm, secure_conn; uint8_t io_cap;
    ring_bt_conn_cb_t on_connect, on_disconnect; void *user;
} ring_bt_config_t;

esp_err_t ring_bt_init(const ring_bt_config_t *cfg);
esp_err_t ring_bt_start_advertising(void);
esp_err_t ring_bt_stop_advertising(void);
esp_err_t ring_bt_set_name(const char *name);
esp_err_t ring_bt_notify(uint16_t chr_uuid, const void *data, size_t len);
ring_bt_state_t ring_bt_state(void);
bool ring_bt_is_connected(void);
uint16_t ring_bt_conn_handle(void);
```

The app supplies a **NULL-terminated** table of `ring_bt_svc_t` / `ring_bt_chr_t`
(16-bit UUIDs) plus a per-characteristic access callback; `ring_bt` builds the
NimBLE GATT defs, runs the host task, drives advertising, and reports state
under a mutex. `ring_bt_notify()` is safe from any task (it stores the value
and schedules the host-task notification).

Design notes carried over from `ring_wifi.c`:
- A single mutex guards the state struct; the **GAP event callback** (host task
  context) updates it; app reads it from any task.
- `nimble_port_init()` is idempotent; guard with a once-flag.
- The GATT `access_cb`s and `ble_gatts_chr_updated()` run in the host task —
  keep them short and non-blocking; do UI work on your own LVGL timer.
- Log with `ESP_LOGI/LOGW/LOGE` under a `TAG` (`"ring_bt"`), matching the codebase.
- The app **never** includes `nimble/*` — only `ringkit/ring_bt.h`.

### Build wiring

`ringkit/CMakeLists.txt` adds `ring_bt.c` to `SRCS` and `bt` to `REQUIRES`:

```cmake
idf_component_register(
    SRCS ... src/ring_bt.c ...
    INCLUDE_DIRS "include"
    REQUIRES smartring_plus lvgl esp_lvgl_adapter esp_wifi esp_netif esp_event nvs_flash bt
)
```

`bt` is the in-tree ESP-IDF component that contains **both** the NimBLE host
and the ESP32-S3 controller (there is no separate `nimble` component, and no
`idf_component.yml` entry — Bluetooth is not a managed dependency). It must be
in `REQUIRES` (not `PRIV_REQUIRES`) because NimBLE headers appear in ringkit's
public `ring_bt.h`… actually they do not; `ring_bt.h` is NimBLE-free, so `bt`
could be `PRIV_REQUIRES`. It is kept in `REQUIRES` so the include path is
unambiguous. Enable it globally via `sdkconfig.defaults` (§4) — `sdkconfig`
is shared by every app, so this affects all builds (harmless: the linker
garbage-collects the BT code for apps that never call `ring_bt_init()`).

## 10. Gotchas specific to this board

1. **No Bluetooth Classic.** Don't plan A2DP/SPP/headset. If you see an SDK or
   library wanting `esp_bt_gap_*` / `esp_bt_dev_*`, it won't work on S3.
2. **NVS must be initialized before `nimble_port_init()`** — NimBLE stores PHY
   calibration there. `ring_wifi_init()` already does it; if you only use BT, do it
   yourself. Also: the same "clobbered NVS → erase and retry" dance applies.
3. **Don't move BLE to PSRAM** unless forced. The controller is internal-only; a
   mixed internal/PSRAM host adds latency and cache pressure for no gain here.
4. **`esp_bt_mem_release()`** can reclaim ~20–30 KB of controller text/IRAM after
   init if you enable `CONFIG_BT_RELEASE_IRAM` — but only when you know you won't
   re-init. Optional; measure first.
5. **Console back-pressure is still the #1 "hang" trap.** The NimBLE host task and
   the `ESP_LOG*` you add will **block** the device if the host stops reading
   `/dev/ttyACM0` (USB-Serial-JTAG). Always drain the port continuously (see
   `AGENTS.md`), never `sleep()` before reading in a capture script.
6. **`ble_gatts_chr_updated()`** only notifies peers that **subscribed** — if your
   phone app isn't getting notifications, check you received
   `BLE_GAP_EVENT_SUBSCRIBE` and that `cur_notify`/`cur_indicate` is set.
7. **A BLE app that only scans/advertises and never connects** is the normal
   "idle" state — don't treat repeated `BLE_GAP_EVENT_ADV_COMPLETE` as an error.
8. **The `hello` app is the size ceiling reference.** It uses
   `lv_font_montserrat_48` (~800 KB). BLE + a Montserrat-48 app + large partition
   is fine; BLE + Wi-Fi + Montserrat-48 starts to crowd 1500K — check `idf.py size`
   and drop the 48 font if needed.
9. **Testing on-device.** A BLE connection needs a real phone/app (nRF Connect,
   LightBlue). You can verify advertising from the log
   (`ble_svc_gap_device_name`, the printed address in `on_sync`), but **a
   connection/notification path is a human/phone test** — log evidence alone is
   not proof, same as the touch caveat in `AGENTS.md`.

## 11. Files

1. **`examples/bt-demo/bt_demo.c`** — the app: GATT table, UI, 2 Hz refresh.
2. **`examples/bt-demo/include/ring_example.h`** — `ring_example_start()` /
   `ring_example_name()`.
3. **`examples/bt-demo/CMakeLists.txt`** — fragment listing `RING_EXAMPLE_SRCS`.
4. **`components/ringkit/src/ring_bt.c`** + **`include/ringkit/ring_bt.h`** — the
   NimBLE wrapper (§9).
5. **`sdkconfig.defaults`** — the BLE block from §4.
6. **`main/main.c`** — app-agnostic; calls `ring_board_init()` then
   `ring_example_start()`.

> Remember: `sdkconfig.defaults` is only applied when `sdkconfig` is
> (re)created → run `rm -f sdkconfig && idf.py reconfigure` after editing it.

## 12. Build & verify

```bash
idf.py -DRING_APP=bt-demo -B build/bt-demo build
idf.py -DRING_APP=bt-demo -B build/bt-demo -p /dev/ttyACM0 flash monitor
```

Expected log (TAG `bt-demo`):

```
bt-demo: BLE host up, advertising as 'SmartRing' (svc 0xFFE0)
bt-demo: status: advertising, battery 87%
bt-demo: peer connected (handle 0)
bt-demo: status: connected, battery 87%
bt-demo: command received: 'hello' (5 B)
```

Visual/phone check: scan with nRF Connect / LightBlue → connect to `SmartRing`,
pair (bonding), read `name`/`battery`, write to `command` and observe the `ok`
notification and the on-screen `last:` line. **A connection/notification path is
a human/phone test** — log evidence alone is not proof (see §10.9).

## 13. Reference examples & links

All under `$IDF_PATH/examples/` (v6.1):

| Example | What it teaches |
|---|---|
| `bluetooth/nimble/bleprph` | **The** minimal peripheral: GATT server + advertising + pairing. Copy this. |
| `bluetooth/nimble/bleprph/tutorial/bleprph_walkthrough.md` | Line-by-line explanation of the above. |
| `bluetooth/nimble/bleprph_wifi_coex` | GATT server + Wi-Fi STA together (the ring's future shape). |
| `bluetooth/nimble/blecent` | Central role: scan + connect + read/write. |
| `bluetooth/nimble/ble_multi_adv` | Multiple advertising sets. |
| `bluetooth/nimble/ble_l2cap_coc` | L2CAP Connection-Oriented Channels (bulk data). |
| `bluetooth/nimble/ble_phy` | 1M / 2M / Coded PHY selection. |
| `bluetooth/nimble/ble_spp` | A GATT "serial port" style service. |
| `bluetooth/nimble/ble_gattc_gatts_coex` | GATT client + server in one device. |

Docs:
- BLE overview (S3): `api-guides/ble/overview.html` — architecture, stack table.
- NimBLE host API: `api-reference/bluetooth/nimble/index.html`.
- Bluetooth API index: `api-reference/bluetooth/index.html`.
- RF coexistence (S3): `api-guides/coexist.html` — the Wi-Fi × BLE table & knobs.
- Minimizing RAM: `api-guides/performance/ram-usage.html`.

---

*Compiled from the ESP-IDF v6.1 source tree
(`~/.espressif/v6.1/esp-idf/examples/bluetooth/nimble/bleprph`,
`components/bt/{host/nimble,controller/esp32s3}`), the official v6.1 docs
(ESP32-S3 BLE overview + RF coexistence + NimBLE API), and a real `esp32s3`
`bleprph` configure to confirm the Kconfig values. The `ring_bt` wrapper and the
`bt-demo` example described in §9 are implemented in this repo and verified on
the device (boot log shows advertising as `SmartRing`). No BSP change is required.*
