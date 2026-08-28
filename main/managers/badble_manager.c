#include "sdkconfig.h"

#ifdef CONFIG_HAS_BADBLE

#include "managers/badble_manager.h"

#include "core/glog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "managers/ble_bridge_manager.h"
#include "managers/ble_manager.h"
#include "managers/badusb_builtin_script.h"
#include "managers/hid_script_parser.h"
#include "managers/status_display_manager.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "badble"
#define BADBLE_NVS_NS "badble"
#define BADBLE_NVS_NAME "name"
#define BADBLE_DEFAULT_NAME "GhostESP BadBLE"
#define BADBLE_SCRIPT_DIR "/mnt/ghostesp/badble"
#define BADBLE_MAX_SCRIPT_SIZE 65536
/* BLE HID hosts (Android especially) drop keys when down/up events are sent
 * faster than the ATT round trip. 10 ms is fine for USB but flaky over BLE;
 * 30 ms matches the cadence of typical hardware BLE keyboards. */
#define BADBLE_MIN_KEY_DELAY_MS 30

typedef enum {
    BADBLE_STATE_IDLE = 0,
    BADBLE_STATE_ADVERTISING,
    BADBLE_STATE_WAITING,
    BADBLE_STATE_CONNECTED,
    BADBLE_STATE_RUNNING,
    BADBLE_STATE_DONE,
    BADBLE_STATE_STOPPED,
    BADBLE_STATE_ERROR,
} badble_state_t;

typedef struct {
    char filename[64];
    bool builtin;
} badble_exec_params_t;

static const uint8_t s_keyboard_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0xE0,       /*   Usage Minimum (Left Control) */
    0x29, 0xE7,       /*   Usage Maximum (Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Constant) */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x65,       /*   Logical Maximum (101) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x29, 0x65,       /*   Usage Maximum (101) */
    0x81, 0x00,       /*   Input (Data, Array) */
    0x05, 0x08,       /*   Usage Page (LEDs) */
    0x19, 0x01,       /*   Usage Minimum (Num Lock) */
    0x29, 0x05,       /*   Usage Maximum (Kana) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x05,       /*   Report Count (5) */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x03,       /*   Report Size (3) */
    0x91, 0x01,       /*   Output (Constant) */
    0xC0              /* End Collection */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_keyboard_report_map,
        .len = sizeof(s_keyboard_report_map),
    },
};

static esp_hidd_dev_t *s_hid_dev = NULL;
static SemaphoreHandle_t s_report_lock = NULL;
static SemaphoreHandle_t s_script_done = NULL;
static TaskHandle_t s_script_task = NULL;
static badble_exec_params_t *s_script_params = NULL;

static volatile bool s_initialized = false;
static volatile bool s_profile_ready = false;
static volatile bool s_running = false;
static volatile bool s_connected = false;
static volatile bool s_notify_ready = false;
static volatile bool s_cancel_requested = false;
static volatile bool s_script_running = false;
static volatile bool s_keyboard_mode = false;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile badble_state_t s_state = BADBLE_STATE_IDLE;

static char s_name[BADBLE_NAME_MAX_LEN + 1] = BADBLE_DEFAULT_NAME;
static char s_script_name[64] = {0};
static bool s_hid_deinit_in_progress = false;

/* IDF's NimBLE HID deinit globally stops GATT. The shared BLE manager owns
 * that operation during full host teardown, so suppress HID's duplicate stop
 * while still releasing all HID allocations and process-static state. */
void __real_ble_gatts_stop(void);
void __wrap_ble_gatts_stop(void) {
    if (!s_hid_deinit_in_progress) {
        __real_ble_gatts_stop();
    }
}

static esp_err_t badble_hid_dev_deinit(void) {
    if (!s_hid_dev) {
        return ESP_OK;
    }
    s_hid_deinit_in_progress = true;
    esp_err_t ret = esp_hidd_dev_deinit(s_hid_dev);
    s_hid_deinit_in_progress = false;
    if (ret == ESP_OK) {
        s_hid_dev = NULL;
    }
    return ret;
}

static const char *badble_state_name(badble_state_t state) {
    switch (state) {
        case BADBLE_STATE_ADVERTISING: return "advertising";
        case BADBLE_STATE_WAITING: return "waiting";
        case BADBLE_STATE_CONNECTED: return "connected";
        case BADBLE_STATE_RUNNING: return "running";
        case BADBLE_STATE_DONE: return "done";
        case BADBLE_STATE_STOPPED: return "stopped";
        case BADBLE_STATE_ERROR: return "error";
        case BADBLE_STATE_IDLE:
        default: return "idle";
    }
}

static bool badble_name_valid(const char *name) {
    if (!name || name[0] == '\0' || strlen(name) > BADBLE_NAME_MAX_LEN) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static void badble_load_name(void) {
    strncpy(s_name, BADBLE_DEFAULT_NAME, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(BADBLE_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_name);
    if (nvs_get_str(nvs, BADBLE_NVS_NAME, s_name, &len) != ESP_OK ||
        !badble_name_valid(s_name)) {
        strncpy(s_name, BADBLE_DEFAULT_NAME, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
    nvs_close(nvs);
}

static bool badble_script_name_valid(const char *filename) {
    if (!filename || filename[0] == '\0' || strlen(filename) >= 64 ||
        strstr(filename, "..") != NULL) {
        return false;
    }
    if (strchr(filename, '/') || strchr(filename, '\\')) {
        return false;
    }
    size_t len = strlen(filename);
    return len > 4 && strcmp(filename + len - 4, ".txt") == 0;
}

static bool badble_transport_cancelled(void *ctx) {
    (void)ctx;
    return s_cancel_requested || !s_profile_ready;
}

static bool badble_send_report(const uint8_t report[8]) {
    if (!report) {
        return false;
    }

    /* Diagnose silent send failures: the ESP HID backend logs its own errors
     * once invoked, so if the user sees nothing typed we need to know whether
     * the link, subscription, or backend is the blocker. */
    if (!s_profile_ready || !s_hid_dev) {
        return false;
    }
    if (!s_connected) {
        ESP_LOGW(TAG, "send skipped: no host connected");
        return false;
    }
    if (!s_notify_ready) {
        ESP_LOGW(TAG, "send skipped: host has not enabled HID report notifications");
        return false;
    }

    if (s_report_lock) {
        xSemaphoreTake(s_report_lock, portMAX_DELAY);
    }

    bool can_send = s_profile_ready && s_hid_dev && s_connected && s_notify_ready;
    esp_err_t ret = ESP_FAIL;
    if (can_send) {
        ret = esp_hidd_dev_input_set(s_hid_dev, 0, 1, (uint8_t *)report, 8);
    }

    if (ret != ESP_OK && can_send) {
        /* Transient failures (queue full / ATT flow control) are common on
         * BLE; one retry after a short pause keeps bursts mostly intact. */
        vTaskDelay(pdMS_TO_TICKS(20));
        ret = esp_hidd_dev_input_set(s_hid_dev, 0, 1, (uint8_t *)report, 8);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "HID input report send failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_report_lock) {
        xSemaphoreGive(s_report_lock);
    }
    return ret == ESP_OK;
}

static bool badble_send_key(uint8_t modifiers, uint8_t keycode, void *ctx) {
    (void)ctx;
    uint8_t report[8] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
    if (!badble_send_report(report)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(BADBLE_MIN_KEY_DELAY_MS));
    return !badble_transport_cancelled(NULL);
}

static bool badble_release_keys(void *ctx);

static bool badble_send_string(const char *text, size_t len, void *ctx) {
    (void)ctx;
    if (!text) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (badble_transport_cancelled(NULL)) {
            return false;
        }
        uint8_t keycode;
        uint8_t modifier;
        if (!hid_ascii_to_keycode(text[i], &keycode, &modifier)) {
            continue;
        }
        if (!badble_send_key(modifier, keycode, NULL)) {
            return false;
        }
        if (!badble_release_keys(NULL)) {
            return false;
        }
    }
    return true;
}

static void badble_delay(uint32_t ms, void *ctx) {
    (void)ctx;
    while (ms > 0 && !badble_transport_cancelled(NULL)) {
        uint32_t chunk = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
}

static bool badble_release_keys(void *ctx) {
    (void)ctx;
    const uint8_t release_report[8] = {0};
    return badble_send_report(release_report);
}

static const hid_transport_t s_badble_transport = {
    .send_key = badble_send_key,
    .send_string = badble_send_string,
    .delay = badble_delay,
    .release_keys = badble_release_keys,
    .is_cancelled = badble_transport_cancelled,
    .ctx = NULL,
};

static void badble_hid_event_cb(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_data;
    if (event_id == ESP_HIDD_DISCONNECT_EVENT && s_script_running) {
        s_cancel_requested = true;
    }
}

static int badble_gap_event_cb(struct ble_gap_event *event, void *arg);

static bool badble_start_advertising(void) {
    if (!s_running || !s_profile_ready || !ble_is_stack_ready()) {
        return false;
    }
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return false;
    }

    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BadBLE advertising fields failed: %d", rc);
        return false;
    }

    struct ble_hs_adv_fields response;
    memset(&response, 0, sizeof(response));
    response.name = (const uint8_t *)s_name;
    response.name_len = (uint8_t)strlen(s_name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "BadBLE scan response failed: %d", rc);
        return false;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           badble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BadBLE advertising start failed: %d", rc);
        return false;
    }
    s_state = BADBLE_STATE_ADVERTISING;
    return true;
}

static int badble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (!event) {
        return 0;
    }

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_connected = true;
                s_notify_ready = false;
                s_state = s_script_running ? BADBLE_STATE_WAITING : BADBLE_STATE_CONNECTED;
                ESP_LOGI(TAG, "host connected (handle=%u), waiting for HID subscribe", event->connect.conn_handle);

                /* BLE HID hosts (Android/iOS) ignore input reports on an
                 * unencrypted link even after subscribing. Initiate pairing
                 * right away - Just Works, no passkey - so the link gets
                 * encrypted before the host starts typing. */
                int sec_rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (sec_rc != 0) {
                    ESP_LOGW(TAG, "security initiate failed: %d", sec_rc);
                }
            } else {
                ESP_LOGW(TAG, "connect attempt failed: %d", event->connect.status);
                if (s_running) {
                    (void)badble_start_advertising();
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "host disconnected reason=%d", event->disconnect.reason);
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
                event->disconnect.conn.conn_handle == s_conn_handle) {
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_connected = false;
                s_notify_ready = false;
                if (s_script_running) {
                    s_cancel_requested = true;
                }
                if (s_running) {
                    s_state = BADBLE_STATE_ADVERTISING;
                    (void)badble_start_advertising();
                }
            }
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "subscribe attr_handle=%u cur_notify=%u",
                     event->subscribe.attr_handle, event->subscribe.cur_notify != 0);
            if (s_connected && event->subscribe.conn_handle == s_conn_handle) {
                s_notify_ready = event->subscribe.cur_notify != 0;
                if (!s_script_running) {
                    s_state = BADBLE_STATE_CONNECTED;
                }
                if (s_notify_ready) {
                    ESP_LOGI(TAG, "HID report notifications enabled - keyboard live");
                }
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption change: %s", event->enc_change.status == 0 ? "encrypted" : "not encrypted");
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            /* The peer has a bond but wants a new secure link (common after
             * the phone forgets/re-pairs or firmware restarts). NimBLE will
             * fail the pairing unless the stale bond is dropped: delete it
             * and tell the host to retry, mirroring the IDF HID example. */
            ESP_LOGI(TAG, "repeat pairing - dropping stale bond and retrying");
            {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                    (void)ble_store_util_delete_peer(&desc.peer_id_addr);
                }
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            /* Mirror the IDF HID example: auto-accept every pairing action so
             * phones pair without user friction. */
            ESP_LOGI(TAG, "passkey action %u", event->passkey.params.action);
            {
                struct ble_sm_io pkey = {0};
                if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                    pkey.action = event->passkey.params.action;
                    pkey.passkey = 123456;
                    (void)ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
                    pkey.action = event->passkey.params.action;
                    pkey.numcmp_accept = 1;
                    (void)ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
                    pkey.action = event->passkey.params.action;
                    memset(pkey.oob, 0, sizeof(pkey.oob));
                    (void)ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
                    pkey.action = event->passkey.params.action;
                    pkey.passkey = 123456;
                    (void)ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                }
            }
            break;

        case BLE_GAP_EVENT_NOTIFY_TX:
            if (event->notify_tx.status != 0) {
                ESP_LOGW(TAG, "notify tx failed: attr_handle=%u status=%d",
                         event->notify_tx.attr_handle, event->notify_tx.status);
            }
            break;

        case BLE_GAP_EVENT_MTU:
            break;

        default:
            break;
    }
    return 0;
}

static esp_err_t badble_hid_profile_init(void *arg) {
    (void)arg;
    s_profile_ready = false;
    s_connected = false;
    s_notify_ready = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_hid_device_config_t config = {
        .vendor_id = 0x303A,
        .product_id = 0x4001,
        .version = 0x0100,
        .device_name = s_name,
        .manufacturer_name = "GhostESP",
        .serial_number = "GhostESP-BadBLE",
        .report_maps = s_report_maps,
        .report_maps_len = 1,
    };

    esp_err_t ret = esp_hidd_dev_init(&config, ESP_HID_TRANSPORT_BLE,
                                      badble_hid_event_cb, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HID initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ble_svc_gap_device_name_set(s_name) != 0) {
        ESP_LOGE(TAG, "Unable to set GAP device name");
        return ESP_FAIL;
    }

    s_profile_ready = true;
    return ESP_OK;
}

static void badble_wait_for_script(void) {
    if (!s_script_task) {
        return;
    }

    for (int i = 0; i < 250 && s_script_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_script_task) {
        ESP_LOGW(TAG, "BadBLE script task did not stop in time; deleting it");
        vTaskDelete(s_script_task);
        s_script_task = NULL;
        s_script_running = false;
        free(s_script_params);
        s_script_params = NULL;
    }
}

static void badble_hid_profile_cleanup(void *arg) {
    (void)arg;
    s_cancel_requested = true;
    badble_wait_for_script();
    (void)badble_release_keys(NULL);

    s_profile_ready = false;
    s_running = false;
    s_keyboard_mode = false;
    s_connected = false;
    s_notify_ready = false;

    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t ret = badble_hid_dev_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HID profile cleanup failed: %s", esp_err_to_name(ret));
    }
    s_state = BADBLE_STATE_STOPPED;
}

static void badble_exec_task(void *arg) {
    badble_exec_params_t *params = (badble_exec_params_t *)arg;
    int lines = -1;
    FILE *f = NULL;

    while (s_running && !s_cancel_requested && (!s_connected || !s_notify_ready)) {
        s_state = BADBLE_STATE_WAITING;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_running && !s_cancel_requested) {
        if (params->builtin) {
            s_state = BADBLE_STATE_RUNNING;
            lines = hid_script_execute((char *)badusb_builtin_script, &s_badble_transport);
        } else {
            char path[sizeof(BADBLE_SCRIPT_DIR) + sizeof(params->filename) + 2];
            snprintf(path, sizeof(path), "%s/%s", BADBLE_SCRIPT_DIR, params->filename);
            f = fopen(path, "r");
            if (!f) {
                ESP_LOGE(TAG, "Unable to open BadBLE script '%s'", params->filename);
            } else {
                s_state = BADBLE_STATE_RUNNING;
                lines = hid_script_execute_file(f, &s_badble_transport);
                fclose(f);
            }
        }
    }

    (void)badble_release_keys(NULL);
    bool aborted = s_cancel_requested || lines < 0 || !s_running;
    if (!aborted) {
        s_state = BADBLE_STATE_DONE;
        glog("BadBLE: Done (%d lines)\n", lines);
    } else if (s_running && s_connected) {
        s_state = BADBLE_STATE_CONNECTED;
        glog("BadBLE: Script stopped\n");
    }

    s_script_running = false;
    s_script_name[0] = '\0';
    s_script_params = NULL;
    s_script_task = NULL;
    if (s_script_done) {
        xSemaphoreGive(s_script_done);
    }
    free(params);
    vTaskDelete(NULL);
}

esp_err_t badble_manager_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NO_FREE_PAGES &&
        ret != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return ret;
    }
    badble_load_name();

    s_report_lock = xSemaphoreCreateMutex();
    s_script_done = xSemaphoreCreateBinary();
    if (!s_report_lock || !s_script_done) {
        if (s_report_lock) {
            vSemaphoreDelete(s_report_lock);
            s_report_lock = NULL;
        }
        if (s_script_done) {
            vSemaphoreDelete(s_script_done);
            s_script_done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t badble_manager_start(void) {
    esp_err_t ret = badble_manager_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_running) {
        return ESP_OK;
    }
    if (ble_bridge_is_running()) {
        ESP_LOGW(TAG, "BadBLE cannot start while BLE bridge is active");
        return ESP_ERR_INVALID_STATE;
    }

    if (ble_is_initialized()) {
        ble_deinit();
    }

    s_cancel_requested = false;
    s_state = BADBLE_STATE_IDLE;
    if (!ble_init_with_pre_host(badble_hid_profile_init,
                                badble_hid_profile_cleanup, NULL)) {
        s_state = BADBLE_STATE_ERROR;
        return ESP_FAIL;
    }
    if (!ble_wait_for_ready()) {
        badble_manager_stop();
        s_state = BADBLE_STATE_ERROR;
        return ESP_FAIL;
    }

    /* HID host pairing must be frictionless: NoInputNoOutput gives a plain
     * "Just Works" exchange (no passkey dialog) on Android and iOS. Applied
     * after host init; ble_hs_cfg is consulted at pairing time, so this
     * configures this session's pairings. The next ble_init() re-applies the
     * project defaults from ble_prepare_hs_config(). */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    s_running = true;
    if (!badble_start_advertising()) {
        badble_manager_stop();
        s_state = BADBLE_STATE_ERROR;
        return ESP_FAIL;
    }
    status_display_show_status("BadBLE Advertising");
    return ESP_OK;
}

void badble_manager_stop_profile(void) {
    if (!s_hid_dev && !s_running && !s_script_task) {
        return;
    }
    badble_hid_profile_cleanup(NULL);
}

esp_err_t badble_manager_stop(void) {
    badble_manager_stop_profile();
    if (ble_is_initialized()) {
        ble_deinit();
    }
    return ESP_OK;
}

/* Shared start for file and built-in payload execution: arms the run state
 * and spawns the exec task (the task itself waits for a connected host). */
static esp_err_t badble_spawn_exec(badble_exec_params_t *params) {
    strncpy(s_script_name, params->filename, sizeof(s_script_name) - 1);
    s_script_name[sizeof(s_script_name) - 1] = '\0';
    s_script_params = params;
    s_cancel_requested = false;
    s_script_running = true;
    s_state = BADBLE_STATE_WAITING;
    if (s_script_done) {
        (void)xSemaphoreTake(s_script_done, 0);
    }

    if (xTaskCreate(badble_exec_task, "badble_exec", 8192, params, 5,
                    &s_script_task) != pdPASS) {
        s_script_running = false;
        s_script_params = NULL;
        s_script_name[0] = '\0';
        free(params);
        s_state = BADBLE_STATE_ERROR;
        return ESP_FAIL;
    }
    glog("BadBLE: Waiting for host for '%s'\n", params->filename);
    return ESP_OK;
}

esp_err_t badble_manager_run_script(const char *filename) {
    if (!badble_script_name_valid(filename)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_script_task || s_script_running) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[sizeof(BADBLE_SCRIPT_DIR) + 64 + 2];
    snprintf(path, sizeof(path), "%s/%s", BADBLE_SCRIPT_DIR, filename);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > BADBLE_MAX_SCRIPT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = badble_manager_start();
    if (ret != ESP_OK) {
        return ret;
    }

    badble_exec_params_t *params = calloc(1, sizeof(*params));
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(params->filename, filename, sizeof(params->filename) - 1);
    return badble_spawn_exec(params);
}

esp_err_t badble_manager_run_builtin(void) {
    if (s_script_task || s_script_running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = badble_manager_start();
    if (ret != ESP_OK) {
        return ret;
    }

    badble_exec_params_t *params = calloc(1, sizeof(*params));
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    params->builtin = true;
    strncpy(params->filename, BADUSB_BUILTIN_SCRIPT_NAME, sizeof(params->filename) - 1);
    return badble_spawn_exec(params);
}

esp_err_t badble_manager_keyboard_start(void) {
    if (s_script_task || s_script_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = badble_manager_start();
    if (ret != ESP_OK) {
        return ret;
    }
    s_cancel_requested = false;
    s_keyboard_mode = true;
    if (s_connected) {
        s_state = BADBLE_STATE_CONNECTED;
    }
    return ESP_OK;
}

esp_err_t badble_manager_keyboard_stop(void) {
    if (s_script_task || s_script_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_keyboard_mode = false;
    (void)badble_release_keys(NULL);
    if (s_connected) {
        s_state = BADBLE_STATE_CONNECTED;
    }
    return ESP_OK;
}

bool badble_manager_is_running(void) {
    return s_running;
}

bool badble_manager_is_connected(void) {
    return s_connected;
}

bool badble_manager_is_script_running(void) {
    return s_script_running;
}

bool badble_manager_notifications_ready(void) {
    return s_notify_ready;
}

bool badble_manager_send_keypress(uint8_t modifier, uint8_t keycode) {
    if (!badble_send_key(modifier, keycode, NULL)) {
        return false;
    }
    return badble_release_keys(NULL);
}

bool badble_manager_send_text(const char *text) {
    if (!text || !s_running || !s_connected || !s_notify_ready || s_script_running) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        uint8_t keycode;
        uint8_t modifier;
        if (!hid_ascii_to_keycode(text[i], &keycode, &modifier)) {
            continue;
        }
        if (!badble_send_key(modifier, keycode, NULL) ||
            !badble_release_keys(NULL)) {
            return false;
        }
    }
    return true;
}

esp_err_t badble_manager_set_name(const char *name) {
    if (!badble_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = badble_manager_init();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(BADBLE_NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(nvs, BADBLE_NVS_NAME, name);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret == ESP_OK) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
    return ret;
}

const char *badble_manager_get_name(void) {
    (void)badble_manager_init();
    return s_name;
}

void badble_manager_print_status(void) {
    glog("BadBLE: state=%s name=\"%s\" connected=%u ready=%u script=\"%s\"\n",
         badble_state_name(s_state), badble_manager_get_name(),
         s_connected ? 1U : 0U, s_notify_ready ? 1U : 0U,
         s_script_name);
}

int badble_manager_list_scripts(char scripts[][64], int max_scripts) {
    if (!scripts || max_scripts <= 0) {
        return 0;
    }
    DIR *dir = opendir(BADBLE_SCRIPT_DIR);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while (count < max_scripts && (entry = readdir(dir)) != NULL) {
        if (badble_script_name_valid(entry->d_name)) {
            strncpy(scripts[count], entry->d_name, 63);
            scripts[count][63] = '\0';
            count++;
        }
    }
    closedir(dir);
    return count;
}

#endif // CONFIG_HAS_BADBLE
