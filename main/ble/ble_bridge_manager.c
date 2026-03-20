#include "managers/ble_bridge_manager.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#if CONFIG_BT_HCI_LOG_DEBUG_EN
#include "hci_log/bt_hci_log.h"
#endif
#include "managers/ble_manager.h"
#include "managers/microphone/mic_visualizer.h"
#include "managers/settings_manager.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BRIDGE_TAP_ID 0x42
#define BRIDGE_QUEUE_DEPTH 512
#define BRIDGE_MAX_CHUNK 244
#define BRIDGE_SAFE_MAX_CHUNK 48
#define BRIDGE_LINE_BUFFER 256
#define BRIDGE_CTRL_STATUS_BUFFER 96
#define BRIDGE_FRAME_HEADER_LEN 12
#define BRIDGE_FRAME_VERSION 1
#define BRIDGE_FRAME_MAGIC0 0x47
#define BRIDGE_FRAME_MAGIC1 0x42
#define BRIDGE_FRAME_PAYLOAD_MAX (BRIDGE_MAX_CHUNK - BRIDGE_FRAME_HEADER_LEN)
#define BRIDGE_CMD_INITIAL_TIMEOUT_MS 10000
#define BRIDGE_CMD_IDLE_TIMEOUT_MS 800
#define BRIDGE_DEFAULT_PAIR_WINDOW_MS 60000
#define BRIDGE_NOTIFY_RETRY_MS 20
#define BRIDGE_CONN_UPDATE_DELAY_MS 1500
#define BRIDGE_CONN_UPDATE_RETRY_MAX 2
#define BRIDGE_CONN_SUPERVISION_TIMEOUT 3000
#define BRIDGE_HCI_ERR_TRANSACTION_COLLISION 0x23
#define BRIDGE_OUTPUT_BUF_SIZE 4096
#define BRIDGE_FETCH_DEFAULT_CHUNK_SIZE 240

static const char *TAG = "ble_bridge";

static const ble_uuid128_t s_bridge_service_uuid = BLE_UUID128_INIT(
    0x76, 0x53, 0x65, 0x67, 0x64, 0x69, 0x72, 0x42,
    0x50, 0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47);
static const ble_uuid128_t s_bridge_rx_uuid = BLE_UUID128_INIT(
    0x52, 0x58, 0x67, 0x64, 0x69, 0x72, 0x42, 0x50,
    0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x01);
static const ble_uuid128_t s_bridge_tx_uuid = BLE_UUID128_INIT(
    0x54, 0x58, 0x67, 0x64, 0x69, 0x72, 0x42, 0x50,
    0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x02);
static const ble_uuid128_t s_bridge_ctrl_uuid = BLE_UUID128_INIT(
    0x43, 0x54, 0x52, 0x4c, 0x69, 0x72, 0x42, 0x50,
    0x53, 0x45, 0x74, 0x73, 0x6f, 0x68, 0x47, 0x03);

typedef struct {
    uint16_t len;
    uint8_t data[BRIDGE_MAX_CHUNK];
} bridge_chunk_t;

typedef enum {
    BRIDGE_FRAME_TYPE_CMD = 1,
    BRIDGE_FRAME_TYPE_ACK = 2,
    BRIDGE_FRAME_TYPE_DATA = 3,
    BRIDGE_FRAME_TYPE_END = 4,
    BRIDGE_FRAME_TYPE_ERR = 5,
    BRIDGE_FRAME_TYPE_FETCH = 6,
    BRIDGE_FRAME_TYPE_HAS_DATA = 7,
} bridge_frame_type_t;

typedef enum {
    BRIDGE_FRAME_STATUS_OK = 0,
    BRIDGE_FRAME_STATUS_BAD_FRAME = 1,
    BRIDGE_FRAME_STATUS_EMPTY_COMMAND = 2,
    BRIDGE_FRAME_STATUS_NO_PEER = 3,
    BRIDGE_FRAME_STATUS_FORWARD_FAILED = 4,
    BRIDGE_FRAME_STATUS_BUSY = 5,
    BRIDGE_FRAME_STATUS_CANCELED = 6,
} bridge_frame_status_t;

static QueueHandle_t s_tx_queue;
static StaticQueue_t *s_tx_queue_ctrl;
static uint8_t *s_tx_queue_storage;
static SemaphoreHandle_t s_tx_queue_mutex;
static TimerHandle_t s_conn_update_timer;
static TimerHandle_t s_tx_retry_timer;
static TimerHandle_t s_cmd_idle_timer;
static struct ble_npl_event s_tx_event;
static bool s_tx_event_initialized;
static ble_bridge_state_t s_state = BLE_BRIDGE_STATE_IDLE;
static ble_bridge_mode_t s_mode = BLE_BRIDGE_MODE_DISABLED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_handle;
static uint16_t s_tx_handle;
static uint16_t s_ctrl_handle;
static bool s_notify_enabled;
static volatile bool s_notify_slot_available;
static bool s_service_registered;
static bool s_initialized;
static bool s_started;
static bool s_mic_visualizer_was_running;
static uint16_t s_mtu = 23;
static int64_t s_pairing_open_until_ms;
static char s_line_buffer[BRIDGE_LINE_BUFFER];
static char s_ctrl_status[BRIDGE_CTRL_STATUS_BUFFER] = "READY";
static size_t s_line_len;
static uint8_t s_conn_update_retries;
static uint32_t s_rx_write_count;
static uint32_t s_notify_submit_count;
static uint32_t s_notify_complete_count;
static uint32_t s_active_command_id;
static bool s_active_command_has_output;
static int64_t s_active_command_last_output_ms;
static char s_output_buf[BRIDGE_OUTPUT_BUF_SIZE];
static size_t s_output_len;
static bool s_output_has_more;
static SemaphoreHandle_t s_output_mutex;
static bool s_pending_fetch;
static bool s_tx_draining;
static int64_t s_last_conn_update_ms;
static const uint32_t CONN_UPDATE_MIN_INTERVAL_MS = 5000;

static int bridge_gap_event(struct ble_gap_event *event, void *arg);
static int bridge_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static void bridge_start_advertising(void);
static void bridge_suspend_background_load(void);
static void bridge_resume_background_load(void);
static bool bridge_init_tx_queue(void);
static void bridge_queue_chunk(const uint8_t *data, size_t len);
static size_t bridge_get_tx_payload_limit(void);
static void bridge_set_ctrl_statusf(const char *fmt, ...);
static void bridge_log_bt_snapshot(const char *reason);
static void bridge_queue_frame(bridge_frame_type_t type, uint8_t status, uint32_t command_id,
                               const uint8_t *payload, size_t len);
static void bridge_queue_text_frame(bridge_frame_type_t type, uint8_t status, uint32_t command_id,
                                    const char *text);
static void bridge_schedule_command_timeout(uint32_t delay_ms);
static void bridge_cancel_command_timeout(void);
static void bridge_command_timeout_cb(TimerHandle_t timer);
static void bridge_finish_active_command(uint8_t status);
static void bridge_note_command_output(void);
static bool bridge_command_is_stop_like(const char *command, const char *data);
static void bridge_dispatch_command(uint32_t command_id, const char *line, bool framed);
static bool bridge_handle_frame_bytes(const uint8_t *data, size_t len);
static void bridge_handle_fetch(uint32_t command_id, uint8_t max_bytes);
static void bridge_schedule_tx_event(void);
static void bridge_schedule_tx_retry(uint32_t delay_ms);
static void bridge_tx_retry_timer_cb(TimerHandle_t timer);
static void bridge_tx_event_cb(struct ble_npl_event *event);
static int bridge_send_notification(const uint8_t *data, size_t len);
static const char *bridge_hs_status_name(int status);
static const char *bridge_hci_reason_name(int reason);
static int bridge_hci_reason_code(int reason);
static void bridge_log_conn_desc(uint16_t conn_handle, const char *prefix);
static void bridge_schedule_conn_update(uint16_t conn_handle, uint32_t delay_ms);
static void bridge_cancel_conn_update(void);

static void bridge_queue_chunk(const uint8_t *data, size_t len) {
    if (!s_started || !s_tx_queue) {
        return;
    }

    if (len > BRIDGE_MAX_CHUNK) {
        ESP_LOGW(TAG, "bridge_queue_chunk: len %u > max %u, truncating", (unsigned)len, (unsigned)BRIDGE_MAX_CHUNK);
        len = BRIDGE_MAX_CHUNK;
    }

    bridge_chunk_t chunk;
    chunk.len = (uint16_t)len;
    memcpy(chunk.data, data, len);

    if (xQueueSend(s_tx_queue, &chunk, 0) != pdPASS) {
        ESP_LOGW(TAG, "bridge_queue_chunk: queue full, dropping %u bytes (queued=%u)",
                 (unsigned)len, (unsigned)uxQueueMessagesWaiting(s_tx_queue));
        return;
    }

    ESP_LOGD(TAG, "queued %u bytes (queued=%u slot=%u)", (unsigned)len,
             (unsigned)uxQueueMessagesWaiting(s_tx_queue), (unsigned)s_notify_slot_available);
    bridge_schedule_tx_event();
}

static void bridge_set_ctrl_statusf(const char *fmt, ...) {
    if (!fmt) {
        s_ctrl_status[0] = '\0';
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(s_ctrl_status, sizeof(s_ctrl_status), fmt, args);
    va_end(args);
}

static void bridge_log_bt_snapshot(const char *reason) {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    size_t psram_free = 0;
    size_t psram_largest = 0;
#endif
    int msys_free = os_msys_num_free();

    ESP_LOGI(TAG,
             "%s: started=%u conn=%u notify=%u slot=%u mtu=%u queued=%u int_free=%u int_largest=%u psram_free=%u psram_largest=%u msys_free=%d",
             reason ? reason : "BLE bridge snapshot",
             (unsigned)s_started,
             (unsigned)s_conn_handle,
             (unsigned)s_notify_enabled,
             (unsigned)s_notify_slot_available,
             (unsigned)s_mtu,
             s_tx_queue ? (unsigned)uxQueueMessagesWaiting(s_tx_queue) : 0,
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             msys_free);
}

static void bridge_queue_frame(bridge_frame_type_t type, uint8_t status, uint32_t command_id,
                               const uint8_t *payload, size_t len) {
    if (!s_started || !s_tx_queue) {
        return;
    }

    if (len == 0) {
        uint8_t frame[BRIDGE_FRAME_HEADER_LEN] = {
            BRIDGE_FRAME_MAGIC0,
            BRIDGE_FRAME_MAGIC1,
            BRIDGE_FRAME_VERSION,
            (uint8_t)type,
            status,
            0,
            (uint8_t)(command_id & 0xFF),
            (uint8_t)((command_id >> 8) & 0xFF),
            (uint8_t)((command_id >> 16) & 0xFF),
            (uint8_t)((command_id >> 24) & 0xFF),
            0,
            0,
        };
        bridge_queue_chunk(frame, sizeof(frame));
        return;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > BRIDGE_FRAME_PAYLOAD_MAX) {
            chunk_len = BRIDGE_FRAME_PAYLOAD_MAX;
        }

        uint8_t frame[BRIDGE_FRAME_HEADER_LEN + BRIDGE_FRAME_PAYLOAD_MAX];
        frame[0] = BRIDGE_FRAME_MAGIC0;
        frame[1] = BRIDGE_FRAME_MAGIC1;
        frame[2] = BRIDGE_FRAME_VERSION;
        frame[3] = (uint8_t)type;
        frame[4] = status;
        frame[5] = 0;
        frame[6] = (uint8_t)(command_id & 0xFF);
        frame[7] = (uint8_t)((command_id >> 8) & 0xFF);
        frame[8] = (uint8_t)((command_id >> 16) & 0xFF);
        frame[9] = (uint8_t)((command_id >> 24) & 0xFF);
        frame[10] = (uint8_t)(chunk_len & 0xFF);
        frame[11] = (uint8_t)((chunk_len >> 8) & 0xFF);
        memcpy(frame + BRIDGE_FRAME_HEADER_LEN, payload + offset, chunk_len);
        bridge_queue_chunk(frame, BRIDGE_FRAME_HEADER_LEN + chunk_len);
        offset += chunk_len;
    }
}

static void bridge_queue_text_frame(bridge_frame_type_t type, uint8_t status, uint32_t command_id,
                                    const char *text) {
    bridge_queue_frame(type,
                       status,
                       command_id,
                       text ? (const uint8_t *)text : NULL,
                       text ? strlen(text) : 0);
}

static void bridge_send_ack_with_pending(uint32_t command_id, uint16_t pending_bytes) {
    uint8_t frame[BRIDGE_FRAME_HEADER_LEN] = {
        BRIDGE_FRAME_MAGIC0,
        BRIDGE_FRAME_MAGIC1,
        BRIDGE_FRAME_VERSION,
        BRIDGE_FRAME_TYPE_ACK,
        BRIDGE_FRAME_STATUS_OK,
        0,
        (uint8_t)(command_id & 0xFF),
        (uint8_t)((command_id >> 8) & 0xFF),
        (uint8_t)((command_id >> 16) & 0xFF),
        (uint8_t)((command_id >> 24) & 0xFF),
        (uint8_t)(pending_bytes & 0xFF),
        (uint8_t)((pending_bytes >> 8) & 0xFF),
    };
    bridge_queue_chunk(frame, sizeof(frame));
}

static void bridge_schedule_command_timeout(uint32_t delay_ms) {
    if (!s_cmd_idle_timer || s_active_command_id == 0) {
        return;
    }

    ESP_LOGI(TAG, "scheduling command timeout for cmd=%u in %u ms", (unsigned)s_active_command_id, (unsigned)delay_ms);
    xTimerStop(s_cmd_idle_timer, 0);
    if (xTimerChangePeriod(s_cmd_idle_timer, pdMS_TO_TICKS(delay_ms), pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGW(TAG, "failed to schedule command timeout");
        return;
    }
    xTimerStart(s_cmd_idle_timer, pdMS_TO_TICKS(10));
}

static void bridge_cancel_command_timeout(void) {
    if (s_cmd_idle_timer) {
        xTimerStop(s_cmd_idle_timer, 0);
    }
}

static void bridge_command_timeout_cb(TimerHandle_t timer) {
    (void)timer;
    ESP_LOGI(TAG, "command timeout cb: cmd=%u has_output=%u", (unsigned)s_active_command_id, (unsigned)s_active_command_has_output);
    if (s_active_command_id == 0) {
        return;
    }

    if (!s_active_command_has_output) {
        ESP_LOGI(TAG, "command timeout: no output, finishing cmd=%u", (unsigned)s_active_command_id);
        bridge_finish_active_command(BRIDGE_FRAME_STATUS_OK);
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000LL;
    int64_t elapsed_ms = now_ms - s_active_command_last_output_ms;
    if (elapsed_ms >= BRIDGE_CMD_IDLE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "command timeout: idle %lldms, finishing cmd=%u", (long long)elapsed_ms, (unsigned)s_active_command_id);
        bridge_finish_active_command(BRIDGE_FRAME_STATUS_OK);
        return;
    }

    uint32_t remaining_ms = (uint32_t)(BRIDGE_CMD_IDLE_TIMEOUT_MS - elapsed_ms);
    if (remaining_ms == 0) {
        remaining_ms = BRIDGE_CMD_IDLE_TIMEOUT_MS;
    }
    ESP_LOGI(TAG, "command timeout: rescheduling cmd=%u in %u ms", (unsigned)s_active_command_id, (unsigned)remaining_ms);
    bridge_schedule_command_timeout(remaining_ms);
}

static void bridge_finish_active_command(uint8_t status) {
    uint32_t command_id = s_active_command_id;
    if (command_id == 0) {
        return;
    }

    ESP_LOGI(TAG, "finishing active cmd=%u status=%u output_len=%u", (unsigned)command_id, (unsigned)status, (unsigned)s_output_len);
    bridge_cancel_command_timeout();
    s_active_command_id = 0;
    s_active_command_has_output = false;
    s_active_command_last_output_ms = 0;
    s_tx_draining = true;

    if (s_output_mutex) {
        xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    }

    if (s_output_len > 0) {
        bridge_queue_frame(BRIDGE_FRAME_TYPE_DATA, BRIDGE_FRAME_STATUS_OK, command_id,
                          (uint8_t *)s_output_buf, s_output_len);
        s_output_len = 0;
    }
    s_output_has_more = false;
    s_pending_fetch = false;

    if (s_output_mutex) {
        xSemaphoreGive(s_output_mutex);
    }

    bridge_queue_frame(BRIDGE_FRAME_TYPE_END, status, command_id, NULL, 0);
}

static void bridge_note_command_output(void) {
    if (s_active_command_id == 0) {
        return;
    }

    s_active_command_has_output = true;
    s_active_command_last_output_ms = esp_timer_get_time() / 1000LL;
}

static bool bridge_command_is_stop_like(const char *command, const char *data) {
    if (!command) {
        return false;
    }

    if (strcmp(command, "stop") == 0) {
        return true;
    }
    if (strcmp(command, "blescan") == 0 && data && strcmp(data, "-s") == 0) {
        return true;
    }
    if (strcmp(command, "scanap") == 0 && data && strcmp(data, "-stop") == 0) {
        return true;
    }
    return false;
}

static const char *bridge_hs_status_name(int status) {
    if (status >= 0x200 && status < 0x300) {
        return bridge_hci_reason_name(status);
    }

    switch (status) {
        case 0: return "OK";
        case BLE_HS_EAGAIN: return "EAGAIN";
        case BLE_HS_EALREADY: return "EALREADY";
        case BLE_HS_EINVAL: return "EINVAL";
        case BLE_HS_EMSGSIZE: return "EMSGSIZE";
        case BLE_HS_ENOENT: return "ENOENT";
        case BLE_HS_ENOMEM: return "ENOMEM";
        case BLE_HS_ENOTCONN: return "ENOTCONN";
        case BLE_HS_ENOTSUP: return "ENOTSUP";
        case BLE_HS_EAPP: return "EAPP";
        case BLE_HS_EBADDATA: return "EBADDATA";
        case BLE_HS_EOS: return "EOS";
        case BLE_HS_ECONTROLLER: return "ECONTROLLER";
        case BLE_HS_ETIMEOUT: return "ETIMEOUT";
        case BLE_HS_EDONE: return "EDONE";
        case BLE_HS_EBUSY: return "EBUSY";
        case BLE_HS_EREJECT: return "EREJECT";
        case BLE_HS_EUNKNOWN: return "EUNKNOWN";
        case BLE_HS_EROLE: return "EROLE";
        case BLE_HS_ETIMEOUT_HCI: return "ETIMEOUT_HCI";
        case BLE_HS_ENOMEM_EVT: return "ENOMEM_EVT";
        case BLE_HS_ENOADDR: return "ENOADDR";
        case BLE_HS_ENOTSYNCED: return "ENOTSYNCED";
        case BLE_HS_EAUTHEN: return "EAUTHEN";
        case BLE_HS_EAUTHOR: return "EAUTHOR";
        case BLE_HS_EENCRYPT: return "EENCRYPT";
        case BLE_HS_EENCRYPT_KEY_SZ: return "EENCRYPT_KEY_SZ";
        default: return "UNKNOWN";
    }
}

static int bridge_hci_reason_code(int reason) {
    if (reason >= 0x200) {
        return reason - 0x200;
    }
    return reason;
}

static const char *bridge_hci_reason_name(int reason) {
    switch (bridge_hci_reason_code(reason)) {
        case BLE_ERR_UNKNOWN_HCI_CMD: return "UNKNOWN_HCI_CMD";
        case BLE_ERR_UNK_CONN_ID: return "UNKNOWN_CONN_ID";
        case BLE_ERR_HW_FAIL: return "HW_FAIL";
        case BLE_ERR_PAGE_TMO: return "PAGE_TIMEOUT";
        case BLE_ERR_AUTH_FAIL: return "AUTH_FAIL";
        case BLE_ERR_MEM_CAPACITY: return "MEM_CAPACITY";
        case BLE_ERR_CONN_SPVN_TMO: return "CONN_SUPERVISION_TIMEOUT";
        case BLE_ERR_CMD_DISALLOWED: return "CMD_DISALLOWED";
        case BLE_ERR_CONN_REJ_RESOURCES: return "CONN_REJECTED_RESOURCES";
        case BLE_ERR_CONN_ACCEPT_TMO: return "CONN_ACCEPT_TIMEOUT";
        case BLE_ERR_REM_USER_CONN_TERM: return "REMOTE_USER_TERMINATED";
        case BLE_ERR_RD_CONN_TERM_RESRCS: return "REMOTE_LOW_RESOURCES";
        case BLE_ERR_RD_CONN_TERM_PWROFF: return "REMOTE_POWER_OFF";
        case BLE_ERR_CONN_TERM_LOCAL: return "LOCAL_HOST_TERMINATED";
        case BLE_ERR_LMP_LL_RSP_TMO: return "LL_RESPONSE_TIMEOUT";
        case BRIDGE_HCI_ERR_TRANSACTION_COLLISION: return "TRANSACTION_COLLISION";
        case BLE_ERR_CTLR_BUSY: return "CONTROLLER_BUSY";
        case BLE_ERR_CONN_PARMS: return "UNACCEPTABLE_CONN_PARAMS";
        case BLE_ERR_CONN_ESTABLISHMENT: return "CONN_ESTABLISHMENT_FAIL";
        case BLE_ERR_MAC_CONN_FAIL: return "MAC_CONN_FAIL";
        default: return "UNKNOWN";
    }
}

static void bridge_log_conn_desc(uint16_t conn_handle, const char *prefix) {
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s conn_desc unavailable rc=%d (%s)",
                 prefix ? prefix : "BLE bridge",
                 rc,
                 bridge_hs_status_name(rc));
        return;
    }

    ESP_LOGI(TAG,
             "%s handle=%u itvl=%u latency=%u supervision_timeout=%u encrypted=%u authenticated=%u bonded=%u",
             prefix ? prefix : "BLE bridge",
             (unsigned)desc.conn_handle,
             (unsigned)desc.conn_itvl,
             (unsigned)desc.conn_latency,
             (unsigned)desc.supervision_timeout,
             (unsigned)desc.sec_state.encrypted,
             (unsigned)desc.sec_state.authenticated,
             (unsigned)desc.sec_state.bonded);
}

static void bridge_request_stable_params(uint16_t conn_handle, const char *source) {
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if (now_ms - s_last_conn_update_ms < CONN_UPDATE_MIN_INTERVAL_MS) {
        ESP_LOGD(TAG, "bridge_request_stable_params: skipped (%s) - only %lldms since last update",
                 source ? source : "unknown", (long long)(now_ms - s_last_conn_update_ms));
        return;
    }

    struct ble_gap_upd_params conn_params = {
        .itvl_min = 36,
        .itvl_max = 48,
        .latency = 0,
        .supervision_timeout = BRIDGE_CONN_SUPERVISION_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    if (!s_started || conn_handle == BLE_HS_CONN_HANDLE_NONE || s_conn_handle != conn_handle) {
        return;
    }

    int rc = ble_gap_update_params(conn_handle, &conn_params);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE bridge conn param update (%s) failed: %d (%s)",
                 source ? source : "unknown",
                 rc,
                 bridge_hs_status_name(rc));
        glog("BLE bridge conn param update (%s) failed: %d (%s)\n",
             source ? source : "unknown",
             rc,
             bridge_hs_status_name(rc));
    } else {
        s_last_conn_update_ms = now_ms;
        ESP_LOGI(TAG,
                 "BLE bridge requested stable params (%s): itvl=%u-%u latency=%u timeout=%u",
                 source ? source : "unknown",
                 (unsigned)conn_params.itvl_min,
                 (unsigned)conn_params.itvl_max,
                 (unsigned)conn_params.latency,
                 (unsigned)conn_params.supervision_timeout);
    }
}

static void bridge_conn_update_timer_cb(TimerHandle_t timer) {
    uint16_t conn_handle = (uint16_t)(uintptr_t)pvTimerGetTimerID(timer);
    bridge_request_stable_params(conn_handle, "delayed");
}

static void bridge_schedule_conn_update(uint16_t conn_handle, uint32_t delay_ms) {
    if (!s_conn_update_timer || conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    if (xTimerIsTimerActive(s_conn_update_timer)) {
        return;
    }

    xTimerStop(s_conn_update_timer, 0);
    vTimerSetTimerID(s_conn_update_timer, (void *)(uintptr_t)conn_handle);
    if (xTimerChangePeriod(s_conn_update_timer, pdMS_TO_TICKS(delay_ms), 0) != pdPASS) {
        ESP_LOGW(TAG, "failed to schedule delayed conn param update");
        return;
    }

    ESP_LOGI(TAG, "BLE bridge scheduling conn param update in %u ms", (unsigned)delay_ms);
}

static void bridge_cancel_conn_update(void) {
    if (!s_conn_update_timer) {
        return;
    }

    xTimerStop(s_conn_update_timer, 0);
    vTimerSetTimerID(s_conn_update_timer, (void *)(uintptr_t)BLE_HS_CONN_HANDLE_NONE);
}

static size_t bridge_get_tx_payload_limit(void) {
    size_t mtu_payload = (s_mtu > 3) ? (size_t)(s_mtu - 3) : 20;
    if (mtu_payload > BRIDGE_SAFE_MAX_CHUNK) {
        mtu_payload = BRIDGE_SAFE_MAX_CHUNK;
    }
    if (mtu_payload == 0) {
        mtu_payload = 20;
    }
    return mtu_payload;
}

static bool bridge_init_tx_queue(void) {
    if (s_tx_queue) {
        return true;
    }

    size_t storage_size = BRIDGE_QUEUE_DEPTH * sizeof(bridge_chunk_t);

#if CONFIG_SPIRAM
    s_tx_queue_storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_tx_queue_storage) {
        s_tx_queue_storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_tx_queue_storage) {
        ESP_LOGE(TAG, "failed to allocate BLE bridge queue storage (%u bytes)", (unsigned)storage_size);
        return false;
    }

    s_tx_queue_ctrl = (StaticQueue_t *)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_tx_queue_ctrl) {
        ESP_LOGE(TAG, "failed to allocate BLE bridge queue control block");
        heap_caps_free(s_tx_queue_storage);
        s_tx_queue_storage = NULL;
        return false;
    }

    s_tx_queue = xQueueCreateStatic(BRIDGE_QUEUE_DEPTH,
                                    sizeof(bridge_chunk_t),
                                    s_tx_queue_storage,
                                    s_tx_queue_ctrl);
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "failed to create BLE bridge queue");
        heap_caps_free(s_tx_queue_ctrl);
        heap_caps_free(s_tx_queue_storage);
        s_tx_queue_ctrl = NULL;
        s_tx_queue_storage = NULL;
        return false;
    }

#if CONFIG_SPIRAM
    if (esp_ptr_external_ram(s_tx_queue_storage)) {
        ESP_LOGI(TAG, "BLE bridge queue storage allocated in PSRAM (%u bytes)", (unsigned)storage_size);
    } else {
        ESP_LOGI(TAG, "BLE bridge queue storage allocated in internal RAM (%u bytes)", (unsigned)storage_size);
    }
#else
    ESP_LOGI(TAG, "BLE bridge queue storage allocated in internal RAM (%u bytes)", (unsigned)storage_size);
#endif

    return true;
}

static void bridge_suspend_background_load(void) {
#ifdef CONFIG_HAS_MIC
    s_mic_visualizer_was_running = mic_visualizer_is_running();
    if (s_mic_visualizer_was_running) {
        mic_visualizer_stop();
        ESP_LOGI(TAG, "Paused mic visualizer for BLE bridge");
    }
#endif
}

static void bridge_resume_background_load(void) {
#ifdef CONFIG_HAS_MIC
    if (s_mic_visualizer_was_running) {
        mic_visualizer_start();
        s_mic_visualizer_was_running = false;
        ESP_LOGI(TAG, "Resumed mic visualizer after BLE bridge");
    }
#endif
}

static const struct ble_gatt_svc_def s_bridge_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_bridge_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_bridge_rx_uuid.u,
                .access_cb = bridge_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_handle,
            },
            {
                .uuid = &s_bridge_tx_uuid.u,
                .access_cb = bridge_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                .val_handle = &s_tx_handle,
            },
            {
                .uuid = &s_bridge_ctrl_uuid.u,
                .access_cb = bridge_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ctrl_handle,
            },
            {0}
        },
    },
    {0}
};

static void bridge_free_resources(void) {
    if (s_tx_queue_mutex) {
        vSemaphoreDelete(s_tx_queue_mutex);
        s_tx_queue_mutex = NULL;
    }
    if (s_output_mutex) {
        vSemaphoreDelete(s_output_mutex);
        s_output_mutex = NULL;
    }
    if (s_tx_retry_timer) {
        xTimerDelete(s_tx_retry_timer, 0);
        s_tx_retry_timer = NULL;
    }
    if (s_cmd_idle_timer) {
        xTimerDelete(s_cmd_idle_timer, 0);
        s_cmd_idle_timer = NULL;
    }
    if (s_conn_update_timer) {
        xTimerDelete(s_conn_update_timer, 0);
        s_conn_update_timer = NULL;
    }
    if (s_tx_queue_ctrl) {
        heap_caps_free(s_tx_queue_ctrl);
        s_tx_queue_ctrl = NULL;
    }
    if (s_tx_queue_storage) {
        heap_caps_free(s_tx_queue_storage);
        s_tx_queue_storage = NULL;
    }
    s_tx_queue = NULL;
}

static bool bridge_is_pairing_allowed(void) {
    if (!G_Settings.ble_bridge_bonding_required) {
        return true;
    }
    if (s_pairing_open_until_ms == 0) {
        return false;
    }
    return esp_timer_get_time() / 1000LL <= s_pairing_open_until_ms;
}

static void bridge_emit_local(const char *line) {
    if (!line) {
        return;
    }
    bridge_queue_text_frame(BRIDGE_FRAME_TYPE_DATA, BRIDGE_FRAME_STATUS_OK, s_active_command_id, line);
}

static void bridge_dispatch_command(uint32_t command_id, const char *line, bool framed) {
    char buffer[BRIDGE_LINE_BUFFER];
    char *command = buffer;
    char *sep = NULL;
    char *data = NULL;

    if (!line) {
        return;
    }

    snprintf(buffer, sizeof(buffer), "%s", line);
    while (*command == ' ') {
        ++command;
    }
    if (*command == '\0') {
        if (framed) {
            bridge_queue_text_frame(BRIDGE_FRAME_TYPE_ERR,
                                    BRIDGE_FRAME_STATUS_EMPTY_COMMAND,
                                    command_id,
                                    "empty-command");
        }
        return;
    }

    sep = strchr(command, ' ');
    if (sep) {
        *sep = '\0';
        data = sep + 1;
        while (data && *data == ' ') {
            ++data;
        }
    }

    if (framed && s_tx_draining && s_active_command_id != 0 && s_active_command_id != command_id) {
        if (!bridge_command_is_stop_like(command, data)) {
            ESP_LOGW(TAG, "rejecting cmd=%u '%s': tx draining (queued=%u)",
                     (unsigned)command_id, command,
                     s_tx_queue ? (unsigned)uxQueueMessagesWaiting(s_tx_queue) : 0);
            bridge_queue_text_frame(BRIDGE_FRAME_TYPE_ERR,
                                    BRIDGE_FRAME_STATUS_BUSY,
                                    command_id,
                                    "tx-draining");
            return;
        }
        bridge_finish_active_command(BRIDGE_FRAME_STATUS_CANCELED);
    }

    if (!esp_comm_manager_is_connected()) {
        bridge_set_ctrl_statusf("READY");
        if (framed) {
            bridge_queue_text_frame(BRIDGE_FRAME_TYPE_ERR,
                                    BRIDGE_FRAME_STATUS_NO_PEER,
                                    command_id,
                                    "no-peer");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "-ERR %lu no-peer\n", (unsigned long)command_id);
            bridge_emit_local(msg);
        }
        return;
    }

    if (framed) {
        s_active_command_id = command_id;
        s_active_command_has_output = false;
        s_active_command_last_output_ms = 0;
    }

    if (!esp_comm_manager_send_command(command, (data && *data) ? data : NULL)) {
        bridge_set_ctrl_statusf("READY");
        if (framed) {
            bridge_cancel_command_timeout();
            s_active_command_id = 0;
            s_active_command_has_output = false;
            s_active_command_last_output_ms = 0;
            bridge_queue_text_frame(BRIDGE_FRAME_TYPE_ERR,
                                    BRIDGE_FRAME_STATUS_FORWARD_FAILED,
                                    command_id,
                                    "forward-failed");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "-ERR %lu forward-failed\n", (unsigned long)command_id);
            bridge_emit_local(msg);
        }
        return;
    }

    bridge_set_ctrl_statusf("READY");
    if (framed) {
        s_output_len = 0;
        s_output_has_more = false;
        s_pending_fetch = false;
        bridge_send_ack_with_pending(command_id, 0);
        bridge_schedule_command_timeout(BRIDGE_CMD_INITIAL_TIMEOUT_MS);
        return;
    }
    if (!framed) {
        char msg[64];
        snprintf(msg, sizeof(msg), "+OK %lu\n", (unsigned long)command_id);
        bridge_emit_local(msg);
    }
}

static void bridge_handle_line(char *line) {
    uint32_t command_id = 0;
    while (*line == ' ') {
        ++line;
    }
    if (*line == '\0') {
        return;
    }

    if (strncmp(line, "CMD ", 4) == 0) {
        char *id_start = line + 4;
        char *id_end = NULL;
        unsigned long parsed_id = strtoul(id_start, &id_end, 10);
        if (id_end == id_start) {
            bridge_set_ctrl_statusf("READY");
            bridge_emit_local("-ERR 0 bad-frame\n");
            return;
        }
        command_id = (uint32_t)parsed_id;
        line = id_end;
        while (*line == ' ') {
            ++line;
        }
        if (*line == '\0') {
            bridge_set_ctrl_statusf("READY");
            char msg[64];
            snprintf(msg, sizeof(msg), "-ERR %lu empty-command\n", (unsigned long)command_id);
            bridge_emit_local(msg);
            return;
        }
    }
    bridge_dispatch_command(command_id, line, false);
}

static void bridge_consume_bytes(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char ch = (char)data[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            s_line_buffer[s_line_len] = '\0';
            bridge_handle_line(s_line_buffer);
            s_line_len = 0;
            continue;
        }
        if (s_line_len + 1 < sizeof(s_line_buffer)) {
            s_line_buffer[s_line_len++] = ch;
        }
    }
}

static bool bridge_handle_frame_bytes(const uint8_t *data, size_t len) {
    if (!data || len < BRIDGE_FRAME_HEADER_LEN) {
        return false;
    }
    if (data[0] != BRIDGE_FRAME_MAGIC0 || data[1] != BRIDGE_FRAME_MAGIC1 || data[2] != BRIDGE_FRAME_VERSION) {
        return false;
    }

    uint8_t type = data[3];
    uint32_t command_id = (uint32_t)data[6] |
                          ((uint32_t)data[7] << 8) |
                          ((uint32_t)data[8] << 16) |
                          ((uint32_t)data[9] << 24);
    uint16_t payload_len = (uint16_t)data[10] | ((uint16_t)data[11] << 8);

    if (type == BRIDGE_FRAME_TYPE_FETCH) {
        uint8_t max_bytes = payload_len > 0 ? data[BRIDGE_FRAME_HEADER_LEN] : 0;
        bridge_handle_fetch(command_id, max_bytes);
        return true;
    }

    if (type != BRIDGE_FRAME_TYPE_CMD) {
        return true;
    }
    if ((size_t)payload_len + BRIDGE_FRAME_HEADER_LEN != len || payload_len == 0 || payload_len >= BRIDGE_LINE_BUFFER) {
        bridge_queue_text_frame(BRIDGE_FRAME_TYPE_ERR,
                                BRIDGE_FRAME_STATUS_BAD_FRAME,
                                command_id,
                                "bad-frame");
        return true;
    }

    char line[BRIDGE_LINE_BUFFER];
    memcpy(line, data + BRIDGE_FRAME_HEADER_LEN, payload_len);
    line[payload_len] = '\0';
    bridge_dispatch_command(command_id, line, true);
    return true;
}

static void bridge_handle_fetch(uint32_t command_id, uint8_t max_bytes) {
    if (!s_started || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_active_command_id == 0) {
        return;
    }

    if (command_id != s_active_command_id) {
        return;
    }

    if (s_output_mutex) {
        xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    }

    if (s_output_len == 0) {
        if (s_output_mutex) {
            xSemaphoreGive(s_output_mutex);
        }
        bridge_queue_frame(BRIDGE_FRAME_TYPE_END, BRIDGE_FRAME_STATUS_OK, command_id, NULL, 0);
        s_output_has_more = false;
        s_pending_fetch = false;
        return;
    }

    size_t chunk_size = max_bytes > 0 ? max_bytes : BRIDGE_FETCH_DEFAULT_CHUNK_SIZE;
    if (chunk_size > s_output_len) {
        chunk_size = s_output_len;
    }
    if (chunk_size > BRIDGE_FRAME_PAYLOAD_MAX) {
        chunk_size = BRIDGE_FRAME_PAYLOAD_MAX;
    }

    bridge_queue_frame(BRIDGE_FRAME_TYPE_DATA, BRIDGE_FRAME_STATUS_OK, command_id,
                      (uint8_t *)s_output_buf, chunk_size);

    size_t remaining = s_output_len - chunk_size;
    if (remaining > 0) {
        memmove(s_output_buf, s_output_buf + chunk_size, remaining);
    }
    s_output_len = remaining;
    s_output_has_more = (s_output_len > 0);
    s_pending_fetch = false;

    if (s_output_mutex) {
        xSemaphoreGive(s_output_mutex);
    }
}

static void bridge_response_tap(const uint8_t *data, size_t length, void *user_data) {
    (void)user_data;
    if (s_active_command_id == 0) {
        return;
    }

    if (s_output_mutex) {
        xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    }

    size_t original_len = s_output_len;
    size_t to_copy = length;
    if (s_output_len + to_copy > BRIDGE_OUTPUT_BUF_SIZE) {
        to_copy = BRIDGE_OUTPUT_BUF_SIZE - s_output_len;
    }

    if (to_copy > 0) {
        memcpy(s_output_buf + s_output_len, data, to_copy);
        s_output_len += to_copy;
        s_output_has_more = true;
    }

    if (s_output_mutex) {
        xSemaphoreGive(s_output_mutex);
    }

    if (original_len == 0 && to_copy > 0) {
        ESP_LOGI(TAG, "response tap: first data for cmd=%u, sending HAS_DATA", (unsigned)s_active_command_id);
        uint8_t frame[BRIDGE_FRAME_HEADER_LEN] = {
            BRIDGE_FRAME_MAGIC0,
            BRIDGE_FRAME_MAGIC1,
            BRIDGE_FRAME_VERSION,
            BRIDGE_FRAME_TYPE_HAS_DATA,
            BRIDGE_FRAME_STATUS_OK,
            0,
            (uint8_t)(s_active_command_id & 0xFF),
            (uint8_t)((s_active_command_id >> 8) & 0xFF),
            (uint8_t)((s_active_command_id >> 16) & 0xFF),
            (uint8_t)((s_active_command_id >> 24) & 0xFF),
            0,
            0,
        };
        bridge_queue_chunk(frame, sizeof(frame));
    }

    bridge_note_command_output();
}

static void bridge_schedule_tx_event(void) {
    if (!s_started || !s_tx_queue || !s_tx_event_initialized) {
        return;
    }

    if (ble_npl_event_is_queued(&s_tx_event)) {
        return;
    }

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_event);
}

static void bridge_schedule_tx_retry(uint32_t delay_ms) {
    if (!s_tx_retry_timer) {
        return;
    }

    xTimerStop(s_tx_retry_timer, 0);
    if (xTimerChangePeriod(s_tx_retry_timer, pdMS_TO_TICKS(delay_ms), pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGW(TAG, "failed to schedule tx retry");
        return;
    }
    xTimerStart(s_tx_retry_timer, pdMS_TO_TICKS(10));
}

static void bridge_tx_retry_timer_cb(TimerHandle_t timer) {
    (void)timer;
    bridge_schedule_tx_event();
}

static int bridge_send_notification(const uint8_t *data, size_t len) {
    if (!s_started || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled) {
        return BLE_HS_EBUSY;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGW(TAG, "notify alloc failed for %u-byte chunk", (unsigned)len);
        bridge_log_bt_snapshot("notify alloc failed");
        return BLE_HS_ENOMEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    if (rc == 0) {
        s_notify_submit_count++;
        s_notify_slot_available = true;
        return 0;
    }

    return rc;
}

static void bridge_tx_event_cb(struct ble_npl_event *event) {
    (void)event;

    if (!s_started || !s_tx_queue || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled) {
        return;
    }

    if (!s_notify_slot_available) {
        ESP_LOGW(TAG, "tx event: notify slot busy, re-queuing (queued=%u)",
                 s_tx_queue ? (unsigned)uxQueueMessagesWaiting(s_tx_queue) : 0);
        bridge_schedule_tx_retry(BRIDGE_NOTIFY_RETRY_MS);
        return;
    }

    bridge_chunk_t chunk = {0};
    if (xQueueReceive(s_tx_queue, &chunk, 0) != pdPASS) {
        if (s_tx_draining) {
            ESP_LOGI(TAG, "tx queue drained, clearing draining flag");
        }
        s_tx_draining = false;
        return;
    }

    int rc = bridge_send_notification(chunk.data, chunk.len);
    if (rc == 0) {
        return;
    }

    if (rc == BLE_HS_EBUSY || rc == BLE_HS_ENOMEM) {
        if (xQueueSendToFront(s_tx_queue, &chunk, 0) != pdPASS) {
            ESP_LOGW(TAG, "bridge tx retry requeue failed for %u-byte chunk", (unsigned)chunk.len);
            bridge_log_bt_snapshot("notify retry requeue failed");
            return;
        }
        bridge_schedule_tx_retry(BRIDGE_NOTIFY_RETRY_MS);
        return;
    }

    ESP_LOGW(TAG, "notify failed: %d (%s)", rc, bridge_hs_status_name(rc));
    bridge_log_bt_snapshot("notify failed");
}

static int bridge_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)arg;

    if (attr_handle == s_rx_handle || attr_handle == s_ctrl_handle) {
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                uint8_t buf[BRIDGE_MAX_CHUNK];
                int len = OS_MBUF_PKTLEN(ctxt->om);
                if (len < 0) {
                    return BLE_ATT_ERR_UNLIKELY;
            }
            s_rx_write_count++;
            ESP_LOGI(TAG,
                     "rx write #%lu handle=0x%04x len=%d notify=%u slot=%u queued=%u line_len=%u",
                     (unsigned long)s_rx_write_count,
                     (unsigned)attr_handle,
                     len,
                     (unsigned)s_notify_enabled,
                     (unsigned)s_notify_slot_available,
                     s_tx_queue ? (unsigned)uxQueueMessagesWaiting(s_tx_queue) : 0,
                     (unsigned)s_line_len);
            if (len > (int)sizeof(buf)) {
                len = sizeof(buf);
            }
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
                if (G_Settings.ble_bridge_bonding_required && !bridge_is_pairing_allowed()) {
                    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
                }
                if (attr_handle == s_ctrl_handle) {
                    if (len >= 4 && memcmp(buf, "PING", 4) == 0) {
                        return 0;
                    }
                    if (!bridge_handle_frame_bytes(buf, (size_t)len)) {
                        bridge_consume_bytes(buf, (size_t)len);
                    }
                    return 0;
                }
            if (!bridge_handle_frame_bytes(buf, (size_t)len)) {
                bridge_consume_bytes(buf, (size_t)len);
            }
            return 0;
        }
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == s_ctrl_handle) {
            os_mbuf_append(ctxt->om, s_ctrl_status, strlen(s_ctrl_status));
            return 0;
        }

        char status[160];
        snprintf(status, sizeof(status),
                 "state=%d peer=%s mtu=%u bond=%s notify=%s",
                 (int)s_state,
                 esp_comm_manager_is_connected() ? "connected" : "disconnected",
                 (unsigned)s_mtu,
                 G_Settings.ble_bridge_bonding_required ? "required" : "open",
                 s_notify_enabled ? "on" : "off");
        os_mbuf_append(ctxt->om, status, strlen(status));
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void bridge_start_advertising(void) {
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields scan_rsp_fields;
    struct ble_gap_adv_params params;
    uint8_t own_addr_type;
    int rc;

    memset(&fields, 0, sizeof(fields));
    memset(&scan_rsp_fields, 0, sizeof(scan_rsp_fields));
    memset(&params, 0, sizeof(params));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_bridge_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    scan_rsp_fields.name = (const uint8_t *)G_Settings.ble_bridge_name;
    scan_rsp_fields.name_len = strlen(G_Settings.ble_bridge_name);
    scan_rsp_fields.name_is_complete = 1;

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ble_gap_adv_set_data(NULL, 0);
    ble_gap_adv_rsp_set_data(NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising fields: %d", rc);
        s_state = BLE_BRIDGE_STATE_IDLE;
        return;
    }
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response fields: %d", rc);
        s_state = BLE_BRIDGE_STATE_IDLE;
        return;
    }
    own_addr_type = BLE_OWN_ADDR_PUBLIC;

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, bridge_gap_event, NULL);
    if (rc == 0) {
        s_state = BLE_BRIDGE_STATE_ADVERTISING;
        ESP_LOGI(TAG, "BLE bridge advertising as '%s'", G_Settings.ble_bridge_name);
        glog("BLE bridge advertising as '%s'.\n", G_Settings.ble_bridge_name);
    } else {
        ESP_LOGE(TAG, "failed to start advertising: %d", rc);
        s_state = BLE_BRIDGE_STATE_IDLE;
    }
}

static int bridge_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_notify_slot_available = false;
                s_conn_update_retries = 0;
                if (s_tx_retry_timer) {
                    xTimerStop(s_tx_retry_timer, 0);
                }
                bridge_cancel_command_timeout();
                s_rx_write_count = 0;
                s_notify_submit_count = 0;
                s_notify_complete_count = 0;
                s_active_command_id = 0;
                s_active_command_has_output = false;
                s_active_command_last_output_ms = 0;
                s_state = esp_comm_manager_is_connected() ? BLE_BRIDGE_STATE_PEER_CONNECTED : BLE_BRIDGE_STATE_CONNECTED;
                bridge_set_ctrl_statusf("READY");
                ESP_LOGI(TAG, "BLE bridge connected, handle=%u", (unsigned)s_conn_handle);
                bridge_log_bt_snapshot("bridge connected");
                glog("BLE bridge connected.\n");
                bridge_log_conn_desc(s_conn_handle, "BLE bridge connected");
                bridge_suspend_background_load();
                bridge_schedule_conn_update(s_conn_handle, BRIDGE_CONN_UPDATE_DELAY_MS);
            } else {
                ESP_LOGW(TAG, "BLE bridge connect failed: %d", event->connect.status);
                bridge_start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "BLE bridge disconnected, reason=%d hci_reason=%d (%s)",
                     event->disconnect.reason,
                     bridge_hci_reason_code(event->disconnect.reason),
                     bridge_hci_reason_name(event->disconnect.reason));
            glog("BLE bridge disconnected (reason=%d, hci_reason=%d, %s).\n",
                 event->disconnect.reason,
                 bridge_hci_reason_code(event->disconnect.reason),
                 bridge_hci_reason_name(event->disconnect.reason));
            bridge_resume_background_load();
            bridge_cancel_conn_update();
            if (s_tx_retry_timer) {
                xTimerStop(s_tx_retry_timer, 0);
            }
            bridge_cancel_command_timeout();
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_notify_enabled = false;
            s_notify_slot_available = false;
            s_mtu = 23;
            s_conn_update_retries = 0;
            s_active_command_id = 0;
            s_active_command_has_output = false;
            s_active_command_last_output_ms = 0;
            s_tx_draining = false;
            bridge_log_bt_snapshot("bridge disconnect");
#if CONFIG_BT_HCI_LOG_DEBUG_EN
            ESP_LOGI(TAG, "Dumping BT HCI debug buffer on bridge disconnect");
            bt_hci_log_hci_data_show();
#endif
            bridge_set_ctrl_statusf("DISCONNECTED");
            if (s_tx_queue) {
                xQueueReset(s_tx_queue);
            }
            s_state = BLE_BRIDGE_STATE_ADVERTISING;
            if (s_started) {
                bridge_start_advertising();
            }
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_tx_handle) {
                s_notify_enabled = event->subscribe.cur_notify;
                s_notify_slot_available = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "BLE bridge subscribe: notify=%u slot=%u cur_notify=%u prev_notify=%u",
                         (unsigned)s_notify_enabled, (unsigned)s_notify_slot_available,
                         (unsigned)event->subscribe.cur_notify, (unsigned)event->subscribe.prev_notify);
                if (s_notify_enabled) {
                    bridge_schedule_tx_event();
                }
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_TX:
            if (event->notify_tx.attr_handle == s_tx_handle &&
                event->notify_tx.conn_handle == s_conn_handle) {
                s_notify_complete_count++;
                if (event->notify_tx.status != 0) {
                    ESP_LOGI(TAG, "notify tx completed with status=%d (%s)",
                             event->notify_tx.status,
                             bridge_hs_status_name(event->notify_tx.status));
                    bridge_log_bt_snapshot("notify tx completion error");
                } else if ((s_notify_complete_count % 32U) == 0U) {
                    ESP_LOGI(TAG,
                             "notify tx complete #%lu submitted=%lu queued=%u msys_free=%d",
                             (unsigned long)s_notify_complete_count,
                             (unsigned long)s_notify_submit_count,
                             s_tx_queue ? (unsigned)uxQueueMessagesWaiting(s_tx_queue) : 0,
                             os_msys_num_free());
                }
                s_notify_slot_available = true;
                bridge_schedule_tx_event();
            }
            break;
        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            ESP_LOGI(TAG, "BLE bridge MTU updated: mtu=%u payload_limit=%u",
                     (unsigned)s_mtu,
                     (unsigned)bridge_get_tx_payload_limit());
            glog("BLE bridge MTU updated: mtu=%u payload_limit=%u\n",
                 (unsigned)s_mtu,
                 (unsigned)bridge_get_tx_payload_limit());
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
            ESP_LOGI(TAG, "BLE bridge conn update status=%d (%s)",
                     event->conn_update.status,
                     bridge_hs_status_name(event->conn_update.status));
            if (event->conn_update.status != 0) {
                glog("BLE bridge conn update failed: status=%d (%s)\n",
                     event->conn_update.status,
                     bridge_hs_status_name(event->conn_update.status));
                if (event->conn_update.conn_handle == s_conn_handle &&
                    bridge_hci_reason_code(event->conn_update.status) == BRIDGE_HCI_ERR_TRANSACTION_COLLISION &&
                    s_conn_update_retries < BRIDGE_CONN_UPDATE_RETRY_MAX) {
                    s_conn_update_retries++;
                    bridge_schedule_conn_update(s_conn_handle, BRIDGE_CONN_UPDATE_DELAY_MS);
                }
            }
            bridge_log_conn_desc(event->conn_update.conn_handle, "BLE bridge conn update");
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                s_state = esp_comm_manager_is_connected() ? BLE_BRIDGE_STATE_PEER_CONNECTED : BLE_BRIDGE_STATE_BONDED;
                ESP_LOGI(TAG, "BLE bridge link encrypted");
                glog("BLE bridge bonded/encrypted.\n");
            } else {
                ESP_LOGW(TAG, "BLE bridge encryption change failed: %d", event->enc_change.status);
                glog("BLE bridge encryption attempt failed: %d\n", event->enc_change.status);
            }
            break;
        default:
            break;
    }
    return 0;
}

void ble_bridge_init(void) {
    if (s_initialized) {
        return;
    }
    if (!bridge_init_tx_queue()) {
        return;
    }
    if (!s_conn_update_timer) {
        s_conn_update_timer = xTimerCreate("bridge_conn_upd",
                                           pdMS_TO_TICKS(BRIDGE_CONN_UPDATE_DELAY_MS),
                                           pdFALSE,
                                           (void *)(uintptr_t)BLE_HS_CONN_HANDLE_NONE,
                                           bridge_conn_update_timer_cb);
        if (!s_conn_update_timer) {
            ESP_LOGE(TAG, "failed to create BLE bridge conn update timer");
            return;
        }
    }
    if (!s_tx_queue_mutex) {
        s_tx_queue_mutex = xSemaphoreCreateMutex();
        if (!s_tx_queue_mutex) {
            ESP_LOGE(TAG, "failed to create BLE bridge tx queue mutex");
            return;
        }
    }
    if (!s_output_mutex) {
        s_output_mutex = xSemaphoreCreateMutex();
        if (!s_output_mutex) {
            ESP_LOGE(TAG, "failed to create BLE bridge output mutex");
            return;
        }
    }
    if (!s_tx_retry_timer) {
        s_tx_retry_timer = xTimerCreate("bridge_tx_retry",
                                        pdMS_TO_TICKS(BRIDGE_NOTIFY_RETRY_MS),
                                        pdFALSE,
                                        NULL,
                                        bridge_tx_retry_timer_cb);
        if (!s_tx_retry_timer) {
            ESP_LOGE(TAG, "failed to create BLE bridge tx retry timer");
            return;
        }
    }
    if (!s_cmd_idle_timer) {
        s_cmd_idle_timer = xTimerCreate("bridge_cmd_idle",
                                        pdMS_TO_TICKS(BRIDGE_CMD_IDLE_TIMEOUT_MS),
                                        pdFALSE,
                                        NULL,
                                        bridge_command_timeout_cb);
        if (!s_cmd_idle_timer) {
            ESP_LOGE(TAG, "failed to create BLE bridge command timer");
            return;
        }
    }
    s_initialized = true;
}

void ble_bridge_deinit(void) {
    ble_bridge_stop();
}

bool ble_bridge_start(ble_bridge_mode_t mode) {
    if (mode != BLE_BRIDGE_MODE_PERIPHERAL) {
        return false;
    }
    ble_bridge_init();
    if (!s_initialized || !s_tx_queue) {
        ESP_LOGE(TAG, "BLE bridge init incomplete");
        return false;
    }
    if (!ble_acquire_mode(BLE_MODE_BRIDGE)) {
        glog("BLE bridge unavailable: BLE is busy.\n");
        return false;
    }
    if (!ble_is_initialized()) {
        ble_init();
    }
    if (!ble_wait_for_ready()) {
        ble_release_mode(BLE_MODE_BRIDGE);
        return false;
    }

    if (!s_tx_event_initialized) {
        ble_npl_event_init(&s_tx_event, bridge_tx_event_cb, NULL);
        s_tx_event_initialized = true;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(G_Settings.ble_bridge_name);

    if (!s_service_registered) {
        int rc = ble_gatts_count_cfg(s_bridge_svcs);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(s_bridge_svcs);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to register GATT services: %d", rc);
            ble_release_mode(BLE_MODE_BRIDGE);
            return false;
        }
        s_service_registered = true;
    }

    int rc = ble_gatts_start();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "failed to start GATT server: %d", rc);
        ble_release_mode(BLE_MODE_BRIDGE);
        return false;
    }

    esp_comm_manager_register_response_tap(BRIDGE_TAP_ID, bridge_response_tap, NULL);
    s_pairing_open_until_ms = (esp_timer_get_time() / 1000LL) + BRIDGE_DEFAULT_PAIR_WINDOW_MS;
    s_mode = mode;
    s_started = true;
    bridge_set_ctrl_statusf("READY");
    bridge_start_advertising();
    return true;
}

void ble_bridge_stop(void) {
    if (!s_started) {
        return;
    }
    esp_comm_manager_unregister_response_tap(BRIDGE_TAP_ID);
    bridge_cancel_conn_update();
    if (s_tx_retry_timer) {
        xTimerStop(s_tx_retry_timer, 0);
    }
    bridge_cancel_command_timeout();
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_notify_enabled = false;
    s_notify_slot_available = false;
    s_started = false;
    s_mode = BLE_BRIDGE_MODE_DISABLED;
    s_state = BLE_BRIDGE_STATE_IDLE;
    s_line_len = 0;
    s_active_command_id = 0;
    s_active_command_has_output = false;
    s_active_command_last_output_ms = 0;
    s_last_conn_update_ms = 0;
    bridge_set_ctrl_statusf("STOPPED");
    if (s_tx_queue) {
        xQueueReset(s_tx_queue);
    }
    bridge_resume_background_load();
    if (ble_is_initialized()) {
        ble_deinit();
    }
    s_tx_event_initialized = false;
    ble_release_mode(BLE_MODE_BRIDGE);
}

ble_bridge_state_t ble_bridge_get_state(void) {
    return s_state;
}

bool ble_bridge_is_active(void) {
    return s_started;
}

void ble_bridge_set_name(const char *name) {
    if (!name || !*name) {
        return;
    }
    settings_set_ble_bridge_name(&G_Settings, name);
    settings_save(&G_Settings);
}

const char *ble_bridge_get_name(void) {
    return settings_get_ble_bridge_name(&G_Settings);
}

void ble_bridge_set_bonding_required(bool required) {
    settings_set_ble_bridge_bonding_required(&G_Settings, required);
    settings_persist_setting(SETTING_BLE_BRIDGE_BONDING);
}

bool ble_bridge_is_bonding_required(void) {
    return settings_get_ble_bridge_bonding_required(&G_Settings);
}

void ble_bridge_forget_bonds(void) {
    bridge_emit_local("bridge:bond reset requires reboot\n");
}

void ble_bridge_open_pairing_window(uint32_t duration_ms) {
    if (duration_ms == 0) {
        duration_ms = BRIDGE_DEFAULT_PAIR_WINDOW_MS;
    }
    s_pairing_open_until_ms = (esp_timer_get_time() / 1000LL) + duration_ms;
}

void ble_bridge_on_ghostlink_output(const uint8_t *data, size_t len) {
    bridge_queue_chunk(data, len);
}

void ble_bridge_on_app_input(const uint8_t *data, size_t len) {
    bridge_consume_bytes(data, len);
}

#else

void ble_bridge_init(void) {}
void ble_bridge_deinit(void) {}
bool ble_bridge_start(ble_bridge_mode_t mode) { (void)mode; return false; }
void ble_bridge_stop(void) {}
ble_bridge_state_t ble_bridge_get_state(void) { return BLE_BRIDGE_STATE_IDLE; }
bool ble_bridge_is_active(void) { return false; }
void ble_bridge_set_name(const char *name) { (void)name; }
const char *ble_bridge_get_name(void) { return "Unsupported"; }
void ble_bridge_set_bonding_required(bool required) { (void)required; }
bool ble_bridge_is_bonding_required(void) { return false; }
void ble_bridge_forget_bonds(void) {}
void ble_bridge_open_pairing_window(uint32_t duration_ms) { (void)duration_ms; }
void ble_bridge_on_ghostlink_output(const uint8_t *data, size_t len) { (void)data; (void)len; }
void ble_bridge_on_app_input(const uint8_t *data, size_t len) { (void)data; (void)len; }

#endif
