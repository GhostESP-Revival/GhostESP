// meshcore_ble.c
// NimBLE GATT server speaking the MeshCore companion protocol. Same host
// lifecycle as lora_ble.c (ble_init_with_pre_host), but with MeshCore's
// Nordic-UART-style service: RX WRITE (app -> fw) and TX NOTIFY (fw -> app),
// one frame per characteristic value.

#include "managers/meshcore_config.h"
#include "managers/meshcore_ble.h"
#include "sdkconfig.h"

#if defined(CONFIG_HAS_MESHCORE) && !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "managers/ble_manager.h"
#include "managers/meshcore_companion.h"
#include "os/os_mbuf.h"

static const char *TAG = "MeshCoreBLE";

#define MC_BLE_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MC_BLE_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MC_BLE_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define MC_BLE_PREFERRED_MTU 517
#define MC_BLE_RX_DEPTH      4
#define MC_BLE_WORKER_STACK  6144

typedef struct {
    uint8_t data[MC_MAX_FRAME_SIZE];
    uint16_t len;
} mc_ble_item_t;

static bool s_registered;
static bool s_advertising;
static bool s_linked;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_h;
static esp_timer_handle_t s_adv_timer;
static QueueHandle_t s_rx_q;
static TaskHandle_t s_worker;

static ble_uuid128_t s_svc_uuid, s_rx_uuid, s_tx_uuid;
static bool s_uuids_ready;
// Last frame sent on TX (stock exposes TX as READ|NOTIFY; reads return the
// most recent frame).
static uint8_t s_last_tx[MC_MAX_FRAME_SIZE];
static uint16_t s_last_tx_len;

static bool uuid128_le(const char *s, uint8_t out[16]) {
    uint8_t be[16];
    int ni = 0;
    for (int i = 0; s[i] && ni < 16; i++) {
        if (s[i] == '-') continue;
        int hi;
        char c = s[i];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return false;
        i++;
        char d = s[i];
        int lo;
        if (!d) return false;
        if (d >= '0' && d <= '9') lo = d - '0';
        else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
        else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
        else return false;
        be[ni++] = (uint8_t)((hi << 4) | lo);
    }
    if (ni != 16) return false;
    for (int i = 0; i < 16; i++) out[i] = be[15 - i];
    return true;
}

static void dev_name(char *out, size_t n) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) memset(mac, 0, sizeof(mac));
    // Stock name is BLE_NAME_PREFIX + the full 6-byte MAC in uppercase hex.
    snprintf(out, n, MC_BLE_NAME_PREFIX "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr == s_tx_h && s_last_tx_len) {
            int rc = os_mbuf_append(ctxt->om, s_last_tx, s_last_tx_len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0; // empty read
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > MC_MAX_FRAME_SIZE) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        mc_ble_item_t it;
        it.len = len;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, it.data, sizeof(it.data), NULL);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        if (s_rx_q) {
            // Non-blocking; if the app floods writes we prefer to drop rather
            // than stall the NimBLE host task.
            (void)xQueueSend(s_rx_q, &it, 0);
        }
        return 0;
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static void worker_task(void *arg) {
    (void)arg;
    mc_ble_item_t it;
    while (1) {
        if (xQueueReceive(s_rx_q, &it, pdMS_TO_TICKS(20)) == pdTRUE) {
            mc_companion_handle_frame(it.data, it.len);
        }
        // Pump the contacts iterator / pending responses a step at a time so
        // we don't overrun the controller's notification buffers.
        mc_companion_loop();
    }
}

static int gap_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_CONNECT) {
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "app connected (handle %u)", (unsigned)s_conn);
        } else {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_advertising = false;
            if (s_registered && s_adv_timer) esp_timer_start_once(s_adv_timer, 500 * 1000);
        }
    } else if (ev->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "app disconnected");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_linked = false;
        mc_companion_set_connected(false);
        if (s_rx_q) xQueueReset(s_rx_q);
        s_advertising = false;
        if (s_registered && s_adv_timer) esp_timer_start_once(s_adv_timer, 500 * 1000);
    } else if (ev->type == BLE_GAP_EVENT_SUBSCRIBE) {
        if (ev->subscribe.attr_handle == s_tx_h) {
            bool sub = ev->subscribe.cur_notify != 0;
            s_linked = sub;
            mc_companion_set_connected(sub);
            ESP_LOGI(TAG, "app %s", sub ? "linked (TX subscribed)" : "unsubscribed");
            if (!sub && s_rx_q) xQueueReset(s_rx_q);
        }
    } else if (ev->type == BLE_GAP_EVENT_MTU) {
        ESP_LOGI(TAG, "app MTU %u", (unsigned)ev->mtu.value);
    } else if (ev->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        s_advertising = false;
    }
    return 0;
}

static void adv_tick(void *arg) {
    (void)arg;
    // The BLE host can be torn down by any other manager (scan, bridge, mode
    // switch) while our service is registered, so never touch NimBLE unless the
    // host is actually up. ble_is_initialized() goes false in ble_deinit().
    if (s_advertising || !s_registered || !ble_is_initialized()) return;
    if (!ble_hs_synced()) {
        esp_timer_start_once(s_adv_timer, 500 * 1000);
        return;
    }
    if (ble_gap_disc_active() || ble_gap_adv_active()) {
        esp_timer_start_once(s_adv_timer, 1000 * 1000);
        return;
    }
    if (!s_uuids_ready) return;
    if (s_tx_h == 0) {
        if (ble_gatts_find_chr(&s_svc_uuid.u, &s_tx_uuid.u, NULL, &s_tx_h) != 0 || s_tx_h == 0) {
            esp_timer_start_once(s_adv_timer, 500 * 1000);
            return;
        }
    }
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &s_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&adv) != 0) return;

    char name[24];
    dev_name(name, sizeof(name));
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) return;

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL) == 0) {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising as %s", name);
    }
}

static esp_err_t pre_host_init(void *arg) {
    (void)arg;
    static uint8_t svc_le[16], rx_le[16], tx_le[16];
    if (!uuid128_le(MC_BLE_SVC_UUID, svc_le) ||
        !uuid128_le(MC_BLE_RX_UUID, rx_le) ||
        !uuid128_le(MC_BLE_TX_UUID, tx_le)) {
        ESP_LOGE(TAG, "pre_host: UUID parse failed");
        return ESP_FAIL;
    }
    s_svc_uuid.u.type = BLE_UUID_TYPE_128;
    s_rx_uuid.u.type = BLE_UUID_TYPE_128;
    s_tx_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(s_svc_uuid.value, svc_le, 16);
    memcpy(s_rx_uuid.value, rx_le, 16);
    memcpy(s_tx_uuid.value, tx_le, 16);
    s_uuids_ready = true;

    static struct ble_gatt_chr_def chrs[] = {
        {
            .uuid = &s_rx_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        },
        {
            .uuid = &s_tx_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_tx_h,
        },
        {0},
    };
    static struct ble_gatt_svc_def defs[] = {
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &s_svc_uuid.u,
            .characteristics = chrs,
        },
        {0},
    };
    ble_gatts_count_cfg(defs);
    if (ble_gatts_add_svcs(defs) != 0) {
        ESP_LOGE(TAG, "pre_host: add_svcs failed");
        return ESP_FAIL;
    }
    int mtu_rc = ble_att_set_preferred_mtu(MC_BLE_PREFERRED_MTU);
    if (mtu_rc != 0) ESP_LOGW(TAG, "preferred MTU rc=%d", mtu_rc);

    if (!s_rx_q) s_rx_q = xQueueCreate(MC_BLE_RX_DEPTH, sizeof(mc_ble_item_t));
    if (!s_rx_q) {
        ESP_LOGE(TAG, "pre_host: queue create failed");
        return ESP_FAIL;
    }
    if (!s_worker) {
        /* PSRAM-preferred stack: keeps the 6 KiB worker off the small internal
         * heap on display boards. Held for the boot lifetime (the worker is
         * never deleted), falling back to a plain internal task. */
        static StackType_t *worker_stack;
        static StaticTask_t *worker_tcb;
        if (!worker_stack) {
            worker_stack = heap_caps_malloc(MC_BLE_WORKER_STACK * sizeof(StackType_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!worker_tcb) {
            worker_tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        bool static_ok = worker_stack && worker_tcb;
        if (static_ok) {
            s_worker = xTaskCreateStatic(worker_task, "meshcore_ble", MC_BLE_WORKER_STACK,
                                         NULL, 7, worker_stack, worker_tcb);
        }
        if (!static_ok || !s_worker) {
            ESP_LOGW(TAG, "worker static alloc failed; using internal stack");
            if (worker_stack) heap_caps_free(worker_stack);
            if (worker_tcb) heap_caps_free(worker_tcb);
            worker_stack = NULL;
            worker_tcb = NULL;
            if (xTaskCreate(worker_task, "meshcore_ble", MC_BLE_WORKER_STACK, NULL, 7,
                            &s_worker) != pdPASS) {
                s_worker = NULL;
                return ESP_FAIL;
            }
        }
    }
    if (!s_adv_timer) {
        esp_timer_create_args_t a = {.callback = adv_tick, .name = "mc_adv"};
        if (esp_timer_create(&a, &s_adv_timer) != ESP_OK) return ESP_FAIL;
    }
    s_registered = true;
    ESP_LOGI(TAG, "service staged");
    return ESP_OK;
}

static void pre_host_cleanup(void *arg) {
    (void)arg;
    if (s_adv_timer) esp_timer_stop(s_adv_timer);
    s_registered = false;
    s_advertising = false;
    s_linked = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_tx_h = 0;
}

bool mc_ble_start(void) {
    if (s_advertising) return true;
    if (ble_is_initialized()) {
        if (!s_registered) {
            ESP_LOGW(TAG, "BLE host up without MeshCore service — reboot to switch modes");
            return false;
        }
    } else if (!ble_init_with_pre_host(pre_host_init, pre_host_cleanup, NULL)) {
        ESP_LOGW(TAG, "BLE host init failed");
        return false;
    }
    if (ble_gap_disc_active() || ble_gap_adv_active()) {
        ESP_LOGW(TAG, "BLE busy (scan/adv) — stop it first");
        return false;
    }
    if (s_adv_timer) esp_timer_start_once(s_adv_timer, 100 * 1000);
    return true;
}

void mc_ble_stop(void) {
    // Stop the advert timer first: ble_deinit() below tears down the NimBLE
    // host without calling pre_host_cleanup(), so a pending tick would call
    // ble_hs_synced() on freed state.
    if (s_adv_timer) esp_timer_stop(s_adv_timer);
    s_registered = false;
    s_advertising = false;
    s_linked = false;
    mc_companion_set_connected(false);
    if (s_rx_q) xQueueReset(s_rx_q);
    if (ble_is_initialized()) ble_deinit();
}

bool mc_ble_is_advertising(void) { return s_advertising; }
bool mc_ble_is_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool mc_ble_is_linked(void) { return s_linked; }

bool mc_ble_notify(const uint8_t *frame, size_t len) {
    if (!s_linked || s_conn == BLE_HS_CONN_HANDLE_NONE || !frame || len == 0) return false;
    if (len > MC_MAX_FRAME_SIZE) len = MC_MAX_FRAME_SIZE;
    memcpy(s_last_tx, frame, len);
    s_last_tx_len = (uint16_t)len;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn, s_tx_h, om);
    return rc == 0;
}

#else
// Inert stubs for targets without native BLE or with MeshCore disabled.
bool mc_ble_start(void) { return false; }
void mc_ble_stop(void) {}
bool mc_ble_is_advertising(void) { return false; }
bool mc_ble_is_connected(void) { return false; }
bool mc_ble_is_linked(void) { return false; }
bool mc_ble_notify(const uint8_t *frame, size_t len) { (void)frame; (void)len; return false; }
#endif
