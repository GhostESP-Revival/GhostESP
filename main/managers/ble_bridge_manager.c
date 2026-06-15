#include "managers/ble_bridge_manager.h"

#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "managers/ble_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#else
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#endif

#define BRIDGE_NAME "GhostESP Bridge"
#define BRIDGE_NVS_NS "blebridge"
#define BRIDGE_NVS_PEER "peer"
#define BRIDGE_NVS_ENABLED "enabled"
#define BRIDGE_TASK_STACK_BYTES 4096
#define BRIDGE_FRAME_HEADER_LEN 12
#define BRIDGE_DEFAULT_MTU 128

#define GB_MAGIC0 0x47
#define GB_MAGIC1 0x42
#define GB_VERSION 0x01

typedef enum {
    GB_TYPE_CMD = 0x01,
    GB_TYPE_ACK = 0x02,
    GB_TYPE_DATA = 0x03,
    GB_TYPE_ERR = 0x05,
    GB_TYPE_HAS_DATA = 0x07,
} bridge_frame_type_t;

typedef struct {
    bool running;
    bool gatt_registered;
    bool ble_connected;
    bool notify_enabled;
    uint16_t conn_handle;
    uint16_t tx_val_handle;
    uint16_t mtu;
    TaskHandle_t task_handle;
    StackType_t *task_stack;
    StaticTask_t *task_tcb;
    SemaphoreHandle_t lock;
    uint32_t active_cmd_id;
    char last_peer[32];
} ble_bridge_state_t;

static const char *TAG = "BLE_BRIDGE";
static ble_bridge_state_t s_bridge = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .mtu = BRIDGE_DEFAULT_MTU,
    .active_cmd_id = 0,
};

static bool bridge_load_enabled(void);
static void bridge_save_enabled(bool enabled);

#ifndef CONFIG_IDF_TARGET_ESP32S2
static int bridge_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int bridge_gap_event_cb(struct ble_gap_event *event, void *arg);

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x76, 0x53, 0x65, 0x67, 0x64, 0x69, 0x72, 0x42,
                     0x50, 0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x52, 0x58, 0x67, 0x64, 0x69, 0x72, 0x42, 0x50,
                     0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x01);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x54, 0x58, 0x67, 0x64, 0x69, 0x72, 0x42, 0x50,
                     0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x02);
static const ble_uuid128_t s_ctrl_uuid =
    BLE_UUID128_INIT(0x43, 0x54, 0x52, 0x4c, 0x69, 0x72, 0x42, 0x50,
                     0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x03);

static struct ble_gatt_chr_def s_bridge_characteristics[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = bridge_gatt_access_cb,
        .arg = (void *)0,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_tx_uuid.u,
        .access_cb = bridge_gatt_access_cb,
        .val_handle = &s_bridge.tx_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &s_ctrl_uuid.u,
        .access_cb = bridge_gatt_access_cb,
        .arg = (void *)1,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {0},
};

static const struct ble_gatt_svc_def s_bridge_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = s_bridge_characteristics,
    },
    {0},
};
#endif

static void bridge_log(const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    msg[sizeof(msg) - 1] = '\0';
    glog("[bridge] %s", msg);
}

static void bridge_lock(void) {
    if (!s_bridge.lock) {
        s_bridge.lock = xSemaphoreCreateMutex();
    }
    if (s_bridge.lock) {
        xSemaphoreTake(s_bridge.lock, portMAX_DELAY);
    }
}

static void bridge_unlock(void) {
    if (s_bridge.lock) {
        xSemaphoreGive(s_bridge.lock);
    }
}

static uint32_t bridge_read_u32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bridge_write_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
    p[2] = (uint8_t)((value >> 16) & 0xff);
    p[3] = (uint8_t)((value >> 24) & 0xff);
}

static size_t bridge_notify_payload_cap(void) {
    uint16_t mtu = s_bridge.mtu ? s_bridge.mtu : BRIDGE_DEFAULT_MTU;
    if (mtu <= BRIDGE_FRAME_HEADER_LEN) {
        return 20;
    }
    size_t cap = (size_t)mtu - BRIDGE_FRAME_HEADER_LEN;
    if (cap > 244) {
        cap = 244;
    }
    return cap;
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
static bool bridge_send_frame(uint8_t type, uint8_t status, uint32_t cmd_id,
                              const uint8_t *payload, size_t payload_len) {
    if (payload_len > 0xffff) {
        payload_len = 0xffff;
    }

    bridge_lock();
    bool can_notify = s_bridge.running && s_bridge.ble_connected && s_bridge.notify_enabled &&
                      s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE && s_bridge.tx_val_handle != 0;
    uint16_t conn_handle = s_bridge.conn_handle;
    uint16_t tx_handle = s_bridge.tx_val_handle;
    bridge_unlock();

    if (!can_notify) {
        return false;
    }

    size_t frame_len = BRIDGE_FRAME_HEADER_LEN + payload_len;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (!frame) {
        return false;
    }

    frame[0] = GB_MAGIC0;
    frame[1] = GB_MAGIC1;
    frame[2] = GB_VERSION;
    frame[3] = type;
    frame[4] = status;
    frame[5] = 0x00;
    bridge_write_u32_le(frame + 6, cmd_id);
    frame[10] = (uint8_t)(payload_len & 0xff);
    frame[11] = (uint8_t)((payload_len >> 8) & 0xff);
    if (payload && payload_len > 0) {
        memcpy(frame + BRIDGE_FRAME_HEADER_LEN, payload, payload_len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_len);
    free(frame);
    if (!om) {
        return false;
    }

    int rc = ble_gatts_notify_custom(conn_handle, tx_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: %d", rc);
        return false;
    }
    return true;
}

static bool bridge_send_data_chunked(uint32_t cmd_id, const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        return true;
    }
    size_t cap = bridge_notify_payload_cap();
    if (cap == 0) {
        cap = 20;
    }
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining < cap ? remaining : cap;
        if (!bridge_send_frame(GB_TYPE_DATA, 0, cmd_id, data, chunk)) {
            return false;
        }
        data += chunk;
        remaining -= chunk;
    }
    return true;
}
#else
static bool bridge_send_frame(uint8_t type, uint8_t status, uint32_t cmd_id,
                              const uint8_t *payload, size_t payload_len) {
    (void)type;
    (void)status;
    (void)cmd_id;
    (void)payload;
    (void)payload_len;
    return false;
}
static bool bridge_send_data_chunked(uint32_t cmd_id, const uint8_t *data, size_t len) {
    (void)cmd_id;
    (void)data;
    (void)len;
    return false;
}
#endif

static void bridge_send_ack(uint32_t cmd_id) {
    (void)bridge_send_frame(GB_TYPE_ACK, 0, cmd_id, NULL, 0);
}

static void bridge_send_err(uint32_t cmd_id, const char *msg) {
    const uint8_t *payload = (const uint8_t *)(msg ? msg : "error");
    size_t len = strlen((const char *)payload);
    (void)bridge_send_frame(GB_TYPE_ERR, 1, cmd_id, payload, len);
}

static void bridge_trim_command(char *cmd) {
    if (!cmd) {
        return;
    }
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n' || isspace((unsigned char)cmd[len - 1]))) {
        cmd[--len] = '\0';
    }
    char *start = cmd;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != cmd) {
        memmove(cmd, start, strlen(start) + 1);
    }
}

static bool bridge_is_local_ctrl(const char *cmd) {
    if (!cmd) {
        return false;
    }
    char lower[64];
    size_t len = strlen(cmd);
    if (len >= sizeof(lower)) {
        len = sizeof(lower) - 1;
    }
    for (size_t i = 0; i < len; ++i) {
        lower[i] = (char)tolower((unsigned char)cmd[i]);
    }
    lower[len] = '\0';
    return strcmp(lower, "stop") == 0;
}

static void bridge_stop_locked(void) {
    s_bridge.running = false;
    esp_comm_manager_set_response_callback(NULL, NULL);
    esp_comm_manager_set_data_callback(NULL, NULL);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_bridge.ble_connected && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_bridge.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    s_bridge.ble_connected = false;
    s_bridge.notify_enabled = false;
    s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
#endif
    esp_comm_manager_disconnect();
}

static void bridge_request_stop(const char *reason) {
    ESP_LOGI(TAG, "CTRL stop: %s", reason ? reason : "stopping bridge");
    bridge_save_enabled(false);
    s_bridge.running = false;
    bridge_stop_locked();
}

static bool bridge_load_enabled(void) {
    nvs_handle_t nvs;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t enabled = 0;
    esp_err_t err = nvs_get_u8(nvs, BRIDGE_NVS_ENABLED, &enabled);
    nvs_close(nvs);
    return err == ESP_OK && enabled != 0;
}

static void bridge_save_enabled(bool enabled) {
    nvs_handle_t nvs;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(nvs, BRIDGE_NVS_ENABLED, enabled ? 1 : 0);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static bool bridge_load_last_peer(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(nvs, BRIDGE_NVS_PEER, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && out[0] != '\0';
}

static void bridge_save_last_peer(const char *peer) {
    if (!peer || peer[0] == '\0') {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(BRIDGE_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(nvs, BRIDGE_NVS_PEER, peer);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    strncpy(s_bridge.last_peer, peer, sizeof(s_bridge.last_peer) - 1);
    s_bridge.last_peer[sizeof(s_bridge.last_peer) - 1] = '\0';
}

static void bridge_data_callback(const uint8_t *data, size_t length, void *user_data) {
    (void)user_data;
    if (!data || length == 0) {
        return;
    }
    bridge_lock();
    uint32_t cmd_id = s_bridge.active_cmd_id;
    bridge_unlock();
    (void)bridge_send_data_chunked(cmd_id, data, length);
}

static void bridge_response_callback(const uint8_t *data, size_t length, void *user_data) {
    bridge_data_callback(data, length, user_data);
}

static void bridge_send_cmd_to_peer(uint32_t cmd_id, const char *command) {
    if (!command || command[0] == '\0') {
        bridge_send_err(cmd_id, "empty command");
        return;
    }

    bridge_lock();
    s_bridge.active_cmd_id = cmd_id;
    bridge_unlock();

    bridge_send_ack(cmd_id);

    if (!esp_comm_manager_is_connected()) {
        bridge_send_err(cmd_id, "comm link down");
        return;
    }

    char cmd[40];
    char data[220];
    size_t idx = 0;
    while (command[idx] && !isspace((unsigned char)command[idx])) {
        idx++;
    }
    size_t cmd_len = idx;
    if (cmd_len == 0 || cmd_len >= sizeof(cmd)) {
        bridge_send_err(cmd_id, "command too long");
        return;
    }
    memcpy(cmd, command, cmd_len);
    cmd[cmd_len] = '\0';
    while (command[idx] && isspace((unsigned char)command[idx])) {
        idx++;
    }
    strncpy(data, command + idx, sizeof(data) - 1);
    data[sizeof(data) - 1] = '\0';

    const char *data_arg = data[0] ? data : NULL;
    if (!esp_comm_manager_send_command(cmd, data_arg)) {
        bridge_send_err(cmd_id, "comm send failed");
        return;
    }
    bridge_log("cmd 0x%08lx -> '%s'", (unsigned long)cmd_id, command);
}

static void bridge_task(void *arg) {
    (void)arg;
    while (s_bridge.running) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bridge_stop_locked();
    s_bridge.task_handle = NULL;
    vTaskDelete(NULL);
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
static bool bridge_start_advertising(void) {
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        bridge_log("adv fields failed: %d", rc);
        return false;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)BRIDGE_NAME;
    rsp.name_len = strlen(BRIDGE_NAME);
    rsp.name_is_complete = 1;
    rsp.tx_pwr_lvl_is_present = 1;
    rsp.tx_pwr_lvl = 0;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, bridge_gap_event_cb, NULL);
    if (rc != 0) {
        bridge_log("adv start failed: %d", rc);
        return false;
    }
    bridge_log("advertising as %s", BRIDGE_NAME);
    return true;
}

static bool bridge_register_gatt(void) {
    if (s_bridge.gatt_registered) {
        return true;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set(BRIDGE_NAME);

    int rc = ble_gatts_count_cfg(s_bridge_services);
    if (rc != 0) {
        bridge_log("GATT count failed: %d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(s_bridge_services);
    if (rc != 0) {
        bridge_log("GATT add failed: %d", rc);
        return false;
    }
    rc = ble_gatts_start();
    if (rc != 0) {
        bridge_log("GATT start failed: %d", rc);
        return false;
    }
    s_bridge.gatt_registered = true;
    return true;
}

static int bridge_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                bridge_lock();
                s_bridge.ble_connected = true;
                s_bridge.conn_handle = event->connect.conn_handle;
                s_bridge.mtu = ble_att_mtu(event->connect.conn_handle);
                bridge_unlock();
                bridge_log("GATT connected, MTU=%u", (unsigned)s_bridge.mtu);
                if (s_bridge.last_peer[0] != '\0' && esp_comm_manager_get_state() == COMM_STATE_SCANNING) {
                    if (esp_comm_manager_connect_to_peer(s_bridge.last_peer)) {
                        bridge_log("peer resolved: '%s'", s_bridge.last_peer);
                    }
                }
            } else if (s_bridge.running) {
                (void)bridge_start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            bridge_lock();
            s_bridge.ble_connected = false;
            s_bridge.notify_enabled = false;
            s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            bridge_unlock();
            bridge_log("GATT disconnected");
            if (s_bridge.running) {
                (void)bridge_start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_bridge.tx_val_handle) {
                bridge_lock();
                s_bridge.notify_enabled = event->subscribe.cur_notify;
                bridge_unlock();
                bridge_log("TX notify %s", event->subscribe.cur_notify ? "enabled" : "disabled");
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            bridge_lock();
            s_bridge.mtu = event->mtu.value;
            bridge_unlock();
            bridge_log("MTU=%u", (unsigned)event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

static void bridge_handle_payload(bool ctrl, const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    if (ctrl && (len < BRIDGE_FRAME_HEADER_LEN || data[0] != GB_MAGIC0 || data[1] != GB_MAGIC1)) {
        char raw[64];
        size_t copy = len < sizeof(raw) - 1 ? len : sizeof(raw) - 1;
        memcpy(raw, data, copy);
        raw[copy] = '\0';
        bridge_trim_command(raw);
        if (bridge_is_local_ctrl(raw)) {
            bridge_request_stop("CTRL raw stop");
            return;
        }
    }

    if (len < BRIDGE_FRAME_HEADER_LEN || data[0] != GB_MAGIC0 || data[1] != GB_MAGIC1 || data[2] != GB_VERSION) {
        bridge_log("bad frame (%u bytes)", (unsigned)len);
        return;
    }
    uint8_t type = data[3];
    uint32_t cmd_id = bridge_read_u32_le(data + 6);

    if (type != GB_TYPE_CMD) {
        return;
    }
    uint16_t payload_len = (uint16_t)(((uint16_t)data[10]) | ((uint16_t)data[11] << 8));
    if ((size_t)payload_len + BRIDGE_FRAME_HEADER_LEN > len) {
        bridge_send_err(cmd_id, "bad frame length");
        return;
    }

    char command[256];
    size_t copy = payload_len < sizeof(command) - 1 ? payload_len : sizeof(command) - 1;
    memcpy(command, data + BRIDGE_FRAME_HEADER_LEN, copy);
    command[copy] = '\0';
    bridge_trim_command(command);

    if (ctrl && bridge_is_local_ctrl(command)) {
        bridge_request_stop("CTRL stop");
        bridge_send_ack(cmd_id);
        return;
    }

    bridge_send_cmd_to_peer(cmd_id, command);
}

static int bridge_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    bool ctrl = arg != NULL;
    if (!ctxt || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !ctxt->om) {
        return 0;
    }
    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len == 0) {
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc(om_len);
    if (!buf) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    size_t copy_len = om_len;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, copy_len, &copy_len);
    if (rc == 0) {
        bridge_handle_payload(ctrl, buf, copy_len);
    }
    free(buf);
    return rc == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}
#endif

static bool bridge_ensure_runtime(void) {
    if (!s_bridge.lock) {
        s_bridge.lock = xSemaphoreCreateMutex();
    }
    return s_bridge.lock != NULL;
}

static bool bridge_create_task(void) {
    if (s_bridge.task_handle) {
        return true;
    }

    size_t stack_words = (BRIDGE_TASK_STACK_BYTES + sizeof(StackType_t) - 1) / sizeof(StackType_t);
    if (!s_bridge.task_stack) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        s_bridge.task_stack = (StackType_t *)heap_caps_malloc(stack_words * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!s_bridge.task_stack) {
            s_bridge.task_stack = (StackType_t *)heap_caps_malloc(stack_words * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_bridge.task_tcb) {
        s_bridge.task_tcb = (StaticTask_t *)heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_bridge.task_stack || !s_bridge.task_tcb) {
        return false;
    }

    s_bridge.task_handle = xTaskCreateStatic(bridge_task, "ble_bridge", stack_words, NULL, 4,
                                             s_bridge.task_stack, s_bridge.task_tcb);
    return s_bridge.task_handle != NULL;
}

static void bridge_wait_and_send_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 30; i++) {
        if (esp_comm_manager_is_connected()) {
            bool ok = esp_comm_manager_send_command("blebridge", "start");
            ESP_LOGI(TAG, "peer connected: sent blebridge start: %s", ok ? "ok" : "fail");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "wait_and_send: peer did not connect within 30s, giving up");
    vTaskDelete(NULL);
}

static void bridge_kick_wait_and_send(void) {
    static StaticTask_t s_wait_tcb;
    static StackType_t s_wait_stack[2048];
    if (!bridge_load_enabled()) {
        return;
    }
    (void)xTaskCreateStatic(bridge_wait_and_send_task, "ble_bridge_wait",
                            sizeof(s_wait_stack) / sizeof(s_wait_stack[0]),
                            NULL, 3, s_wait_stack, &s_wait_tcb);
}

bool ble_bridge_start(void) {
#ifdef CONFIG_IDF_TARGET_ESP32S2
    glog("blebridge is not supported on ESP32-S2.\n");
    return false;
#else
    if (s_bridge.running) {
        bridge_log("already running");
        return true;
    }
    (void)nvs_flash_init();
    (void)bridge_load_last_peer(s_bridge.last_peer, sizeof(s_bridge.last_peer));

    ble_init();
    if (!ble_wait_for_ready()) {
        bridge_log("BLE stack not ready");
        return false;
    }
    if (!bridge_register_gatt()) {
        return false;
    }
    if (!bridge_ensure_runtime()) {
        glog("[bridge] failed to allocate runtime objects\n");
        return false;
    }

    s_bridge.mtu = BRIDGE_DEFAULT_MTU;
    s_bridge.active_cmd_id = 0;
    s_bridge.running = true;
    esp_comm_manager_set_response_callback(bridge_response_callback, NULL);
    esp_comm_manager_set_data_callback(bridge_data_callback, NULL);

    if (!bridge_create_task()) {
        s_bridge.running = false;
        esp_comm_manager_set_response_callback(NULL, NULL);
        esp_comm_manager_set_data_callback(NULL, NULL);
        bridge_log("failed to create bridge task");
        return false;
    }

    if (!bridge_start_advertising()) {
        s_bridge.running = false;
        esp_comm_manager_set_response_callback(NULL, NULL);
        esp_comm_manager_set_data_callback(NULL, NULL);
        return false;
    }

    if (s_bridge.last_peer[0] != '\0' && esp_comm_manager_get_state() == COMM_STATE_SCANNING) {
        if (esp_comm_manager_connect_to_peer(s_bridge.last_peer)) {
            bridge_log("peer resolved: '%s'", s_bridge.last_peer);
        }
    } else if (esp_comm_manager_get_state() == COMM_STATE_IDLE) {
        (void)esp_comm_manager_start_discovery();
    }

    status_display_show_status("BLE Bridge");
    return true;
#endif
}

bool ble_bridge_get_enabled(void) {
    (void)nvs_flash_init();
    return bridge_load_enabled();
}

bool ble_bridge_set_enabled(bool enabled) {
    (void)nvs_flash_init();
    bridge_save_enabled(enabled);

    if (enabled) {
        if (esp_comm_manager_is_connected()) {
            bool ok = esp_comm_manager_send_command("blebridge", "start");
            ESP_LOGI(TAG, "set_enabled: sent start to peer: %s", ok ? "ok" : "fail");
            return ok;
        }
        ESP_LOGI(TAG, "set_enabled: peer not connected, spawning wait task");
        bridge_kick_wait_and_send();
        return true;
    }

    if (esp_comm_manager_is_connected()) {
        (void)esp_comm_manager_send_command("blebridge", "stop");
    }
    return true;
}

void ble_bridge_apply_saved_enabled(void) {
    (void)nvs_flash_init();
    bool enabled = bridge_load_enabled();
    ESP_LOGI(TAG, "apply_saved_enabled: stored=%s", enabled ? "true" : "false");
    if (!enabled) {
        return;
    }
    if (esp_comm_manager_is_connected()) {
        bool ok = esp_comm_manager_send_command("blebridge", "start");
        ESP_LOGI(TAG, "apply_saved_enabled: sent start to peer: %s", ok ? "ok" : "fail");
        return;
    }
    ESP_LOGI(TAG, "apply_saved_enabled: peer not connected, spawning wait task");
    bridge_kick_wait_and_send();
}

void ble_bridge_stop(void) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (!s_bridge.running) {
        return;
    }
    bridge_log("stopping");
    bridge_stop_locked();
    vTaskDelay(pdMS_TO_TICKS(200));
    bridge_log("stopped");
#endif
}

bool ble_bridge_is_running(void) {
    return s_bridge.running;
}

static void bridge_print_status(bool machine) {
    gpio_num_t tx = GPIO_NUM_NC;
    gpio_num_t rx = GPIO_NUM_NC;
    char peer[32] = {0};
    (void)esp_comm_manager_get_pins(&tx, &rx);
    bool has_peer = esp_comm_manager_get_peer_name(peer, sizeof(peer));
    if (!has_peer && s_bridge.last_peer[0] != '\0') {
        strncpy(peer, s_bridge.last_peer, sizeof(peer) - 1);
        has_peer = true;
    }

    const char *ble_state = "idle";
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_bridge.ble_connected) {
        ble_state = "connected 1";
    } else if (s_bridge.running && ble_gap_adv_active()) {
        ble_state = "advertising";
    } else if (s_bridge.running) {
        ble_state = "idle";
    }
#endif
    if (machine) {
        glog("BLE:%s,Peer:%s,UART_TX:%d,UART_RX:%d,CmdId:0x%08lx\n",
             ble_state, has_peer ? peer : "none", (int)tx, (int)rx,
             (unsigned long)s_bridge.active_cmd_id);
    } else {
        glog("BLE: %s, Peer: %s, UART TX/RX: %d/%d, CmdId: 0x%08lx\n",
             ble_state, has_peer ? peer : "none", (int)tx, (int)rx,
             (unsigned long)s_bridge.active_cmd_id);
    }
}

void ble_bridge_handle_command(int argc, char **argv) {
    if (argc <= 1) {
        bridge_print_status(false);
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        bridge_save_enabled(true);
        if (ble_bridge_start()) {
            glog("BLE bridge started.\n");
        } else {
            glog("Failed to start BLE bridge.\n");
        }
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        bridge_save_enabled(false);
        ble_bridge_stop();
        glog("BLE bridge stopped.\n");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        bridge_print_status(true);
        return;
    }

    if (strcmp(argv[1], "pair") == 0) {
        if (argc != 3) {
            glog("Usage: blebridge pair <peer_name>\n");
            return;
        }
        bridge_save_last_peer(argv[2]);
        if (esp_comm_manager_get_state() == COMM_STATE_IDLE) {
            (void)esp_comm_manager_start_discovery();
        }
        if (esp_comm_manager_get_state() == COMM_STATE_SCANNING && esp_comm_manager_connect_to_peer(argv[2])) {
            glog("BLE bridge pairing with peer: %s\n", argv[2]);
        } else {
            glog("Saved BLE bridge peer: %s\n", argv[2]);
        }
        return;
    }

    glog("Usage: blebridge [start|stop|status|pair <peer_name>]\n");
}
