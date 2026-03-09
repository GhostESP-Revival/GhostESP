#include "esp_log.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_wifi.h>
#include "esp_heap_caps.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "core/callbacks.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"
#include "managers/ble_manager.h"
#include "managers/views/terminal_screen.h"
#include "host/ble_gatt.h"
#include "core/glog.h"
#include "core/utils.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "vendor/pcap.h"
#include <esp_mac.h>
#include <managers/rgb_manager.h>
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "esp_bt.h"
#include "managers/ap_manager.h"
#include "managers/wifi_manager.h"
#include "core/scan_saver.h"
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/gatt_scan.h"
#include "attacks/ble/ble_spam.h"

#define MAX_DEVICES 30
#define MAX_HANDLERS 10
#define MAX_PACKET_SIZE 31
#define NIMBLE_HOST_TASK_STACK_SIZE 6144

// AirTag tracking definitions
#ifdef CONFIG_SPIRAM
#define MAX_AIRTAGS 50
#else
#define MAX_AIRTAGS 16
#endif

static const char *TAG_BLE = "BLE_MANAGER";
static int airTagCount = 0;
static volatile bool ble_initialized = false;
static volatile bool ble_stack_ready = false;
static volatile bool airtag_scanner_active = false;

static esp_timer_handle_t flush_timer = NULL;
static TaskHandle_t nimble_host_task_handle = NULL;
static SemaphoreHandle_t nimble_host_exit_sem = NULL;
static SemaphoreHandle_t ble_disc_complete_sem = NULL;
static volatile bool ble_pending_clear = false;
static volatile bool ble_cb_busy = false;
static uint32_t ble_pcap_packet_count = 0;
static uint32_t ble_pcap_event_total_count = 0;

#ifndef CONFIG_IDF_TARGET_ESP32S2
static bool ble_ap_suspended = false;
static bool ble_wifi_suspended = false;
static wifi_mode_t ble_prev_wifi_mode = WIFI_MODE_NULL;
#endif

// Forward declarations
static void restart_ble_stack(void);
static void ble_prepare_hs_config(void);
static void ble_on_sync(void);
static void ble_on_reset(int reason);
static void ble_suspend_networking(void);
static void ble_resume_networking(void);
static bool wait_for_ble_ready(void);
static bool ble_wait_for_scan_stop(uint32_t timeout_ms);
static bool ble_wait_for_callbacks_idle(uint32_t timeout_ms);

static bool ble_wait_for_scan_stop(uint32_t timeout_ms) {
    if (!ble_gap_disc_active()) {
        return true;
    }

    if (ble_disc_complete_sem != NULL) {
        if (xSemaphoreTake(ble_disc_complete_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return !ble_gap_disc_active();
        }
    }

    uint32_t waited_ms = 0;
    while (ble_gap_disc_active() && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }

    return !ble_gap_disc_active();
}

static bool ble_wait_for_callbacks_idle(uint32_t timeout_ms) {
    uint32_t waited_ms = 0;
    while (ble_cb_busy && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    return !ble_cb_busy;
}

typedef struct {
    ble_data_handler_t handler;
} ble_handler_t;

// Structure to store discovered AirTag information
typedef struct {
    ble_addr_t addr;
    uint8_t payload[BLE_HS_ADV_MAX_SZ]; // Store the full payload
    size_t payload_len;
    int8_t rssi;
    bool selected_for_spoofing;
} AirTagDevice;

#define AIRTAG_RSSI_LOG_INTERVAL_MS 3000
static AirTagDevice discovered_airtags[MAX_AIRTAGS];
static int discovered_airtag_count = 0;
static int selected_airtag_index = -1; // Index of the AirTag selected for spoofing
static TickType_t airtag_last_rssi_log[MAX_AIRTAGS];

static ble_handler_t handlers[MAX_HANDLERS];
static int handler_count = 0;
static int spam_counter = 0;
static bool last_company_id_valid = false;
static uint16_t last_company_id_value = 0;
static TickType_t last_detection_time = 0;
static void ble_pcap_callback(struct ble_gap_event *event, size_t len);

static void notify_handlers(struct ble_gap_event *event, int len) {
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].handler) {
            handlers[i].handler(event, len);
        }
    }
}

int ble_gap_event_general(struct ble_gap_event *event, void *arg) {
    (void)arg;

    if (!event) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC) {
        ble_cb_busy = true;

        if (ble_pending_clear) {
            ble_cb_busy = false;
            return 0;
        }

        static uint32_t disc_log_counter = 0;
        disc_log_counter++;
        if ((disc_log_counter % 50) == 1) {
            ESP_LOGI(TAG_BLE,
                     "ble_gap_event_general: %lu discovery events seen; last RSSI=%d len=%u",
                     (unsigned long)disc_log_counter,
                     event->disc.rssi,
                     (unsigned int)event->disc.length_data);
        }
        notify_handlers(event, event->disc.length_data);
        ble_cb_busy = false;
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (ble_disc_complete_sem != NULL) {
            xSemaphoreGive(ble_disc_complete_sem);
        }
    }

    return 0;
}

void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
    if (nimble_host_exit_sem) {
        xSemaphoreGive(nimble_host_exit_sem);
    }
    vTaskDelete(NULL);
}

// Function to prepare BLE host config
static void ble_prepare_hs_config(void) {
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.store_read_cb = ble_store_config_read;
    ble_hs_cfg.store_write_cb = ble_store_config_write;
    ble_hs_cfg.store_delete_cb = ble_store_config_delete;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_DISP;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

static void ble_on_sync(void) {
    ble_stack_ready = true;
    ESP_LOGI(TAG_BLE, "BLE host synced");
}

static void ble_on_reset(int reason) {
    ble_stack_ready = false;
    ESP_LOGW(TAG_BLE, "BLE host reset, reason=%d", reason);
}

static void ble_suspend_networking(void) {
    if (ble_ap_suspended || ble_wifi_suspended) {
        return;
    }

    bool server_running = false;
    ap_manager_get_status(&server_running, NULL, NULL);
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur_mode) != ESP_OK) {
        cur_mode = WIFI_MODE_NULL;
    }

    if (server_running) {
        ESP_LOGI(TAG_BLE, "Suspending GhostNet AP before BLE init");
        TERMINAL_VIEW_ADD_TEXT("Suspending AP for BLE\n");
        ap_manager_deinit();
        ble_ap_suspended = true;
        ble_wifi_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_AP;
    } else if (cur_mode != WIFI_MODE_NULL) {
        ESP_LOGI(TAG_BLE, "Stopping Wi-Fi (mode=%d) before BLE init", cur_mode);
        esp_wifi_stop();
        esp_wifi_deinit();
        ble_wifi_suspended = true;
        ble_prev_wifi_mode = cur_mode;
        ble_ap_suspended = false;
    } else {
        ble_ap_suspended = false;
        ble_wifi_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_NULL;
    }
}

static void ble_resume_networking(void) {
    if (ble_ap_suspended) {
        ESP_LOGI(TAG_BLE, "Restoring GhostNet AP after BLE deinit");
        TERMINAL_VIEW_ADD_TEXT("Restoring AP after BLE\n");
        ble_ap_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_AP;
        ble_stack_ready = false;

        if (ble_initialized) {
            return;
        }

        esp_err_t err = ap_manager_init();
        if (err == ESP_OK) {
            (void)ap_manager_start_services();
        } else {
            ESP_LOGE(TAG_BLE, "Failed to reinit AP manager: 0x%X", (unsigned int)err);
        }
    } else if (ble_wifi_suspended) {
        ESP_LOGI(TAG_BLE, "Restoring Wi-Fi (mode=%d) after BLE deinit", ble_prev_wifi_mode);
        ble_wifi_suspended = false;

        vTaskDelay(pdMS_TO_TICKS(200));

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG_BLE, "Pre Wi-Fi init heap: free=%u, largest=%u", (unsigned)free_heap, (unsigned)largest_block);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (largest_block < 40000) {
            cfg.static_rx_buf_num = 4;
            cfg.dynamic_rx_buf_num = 8;
            ESP_LOGW(TAG_BLE, "Heap fragmented, using reduced Wi-Fi buffers");
        }

        esp_err_t err = esp_wifi_init(&cfg);
        if (err == ESP_OK) {
            wifi_mode_t mode = (ble_prev_wifi_mode == WIFI_MODE_NULL) ? WIFI_MODE_STA : ble_prev_wifi_mode;
            if (mode == WIFI_MODE_APSTA) {
                mode = WIFI_MODE_STA;
            }
            err = esp_wifi_set_mode(mode);
            if (err != ESP_OK) {
                ESP_LOGE(TAG_BLE, "Failed to set Wi-Fi mode: %s", esp_err_to_name(err));
            } else {
                err = esp_wifi_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG_BLE, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
                } else if (mode & WIFI_MODE_STA) {
                    wifi_manager_configure_sta_from_settings();
                }
            }
        } else {
            ESP_LOGE(TAG_BLE, "Failed to reinit Wi-Fi driver: %s (heap: free=%u, largest=%u)", 
                     esp_err_to_name(err), (unsigned)free_heap, (unsigned)largest_block);
        }
        ble_prev_wifi_mode = WIFI_MODE_NULL;
    }
}

// Function to restart the NimBLE stack after MAC address change
static void restart_ble_stack(void) {
    if (!ble_initialized) {
        return;
    }

    ble_stack_ready = false;

    // Stop any active advertising
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    
    ble_pending_clear = true;
    if (ble_disc_complete_sem != NULL) {
        (void)xSemaphoreTake(ble_disc_complete_sem, 0);
    }

    if (ble_gap_disc_active()) {
        (void)ble_gap_disc_cancel();
        (void)ble_wait_for_scan_stop(1200);
    }
    (void)ble_wait_for_callbacks_idle(1200);

    // Stop the NimBLE stack and wait for the host task to signal exit via semaphore
    nimble_port_stop();
    if (nimble_host_task_handle != NULL && nimble_host_exit_sem != NULL) {
        if (xSemaphoreTake(nimble_host_exit_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG_BLE, "nimble_host_task did not signal exit in time");
        }
        nimble_host_task_handle = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // Now deinitialize the port
    nimble_port_deinit();
    
    // Small delay before reinitializing
    vTaskDelay(pdMS_TO_TICKS(50));

    // log DMA-capable internal heap info right before NimBLE re-init
    size_t free_internal_dma_re = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    size_t largest_internal_dma_re = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    ESP_LOGI(TAG_BLE, "reinit pre-init dma-ram: free=%d bytes (largest block=%d)", (int)free_internal_dma_re, (int)largest_internal_dma_re);
    TERMINAL_VIEW_ADD_TEXT("reinit pre-init dma-ram: free=%d bytes (largest=%d)\n", (int)free_internal_dma_re, (int)largest_internal_dma_re);

    // Reinitialize the NimBLE stack
    ble_prepare_hs_config();
    int ret = nimble_port_init();
    if (ret != 0) {
        ESP_LOGE(TAG_BLE, "Failed to reinit nimble port: %d", ret);
        size_t free_dma_after_re = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        size_t largest_dma_after_re = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_LOGI(TAG_BLE, "reinit post-fail dma-ram: free=%d bytes (largest block=%d)", (int)free_dma_after_re, (int)largest_dma_after_re);
        ble_pending_clear = false;
        return;
    }

    // Create exit semaphore for safe task synchronization
    if (nimble_host_exit_sem == NULL) {
        nimble_host_exit_sem = xSemaphoreCreateBinary();
    }

    // Restart the NimBLE host task (larger stack)
    xTaskCreate(nimble_host_task, "nimble_host", NIMBLE_HOST_TASK_STACK_SIZE, NULL, 5, &nimble_host_task_handle);

    ble_pending_clear = false;
    
    // Wait for NimBLE stack to be ready
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG_BLE, "BLE stack restarted successfully");
}

void stop_ble_stack() {
    int rc;

    rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error stopping advertisement");
    }

    ble_pending_clear = true;
    if (ble_gap_disc_active()) {
        (void)ble_gap_disc_cancel();
        (void)ble_wait_for_scan_stop(1200);
    }
    (void)ble_wait_for_callbacks_idle(1200);

    rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error stopping NimBLE port");
        ble_pending_clear = false;
        return;
    }

    nimble_port_deinit();

    ble_pending_clear = false;

    ESP_LOGI(TAG_BLE, "NimBLE stack and task deinitialized.");
}

static bool extract_company_id(const uint8_t *payload, size_t length, uint16_t *company_id) {
    size_t index = 0;

    while (index < length) {
        uint8_t field_length = payload[index];

        if (field_length == 0 || index + field_length >= length) {
            break;
        }

        uint8_t field_type = payload[index + 1];

        if (field_type == 0xFF && field_length >= 3) {
            *company_id = payload[index + 2] | (payload[index + 3] << 8);
            return true;
        }

        index += field_length + 1;
    }

    return false;
}

void ble_stop_skimmer_detection(void) {
    glog("Stopping skimmer detection scan...\n");
    status_display_show_status("Skimmer Stopping");

    // Unregister the skimmer detection callback
    ble_unregister_handler(ble_skimmer_scan_callback);
    pcap_flush_buffer_to_file(); // Final flush
    pcap_file_close();           // Close the file after final flush

    /* final capture summary */
    if (!pcap_is_wireshark_mode()) {
        glog("BLE capture summary: captured=%lu filtered=%lu total=%lu\n",
             (unsigned long)ble_pcap_packet_count,
             (unsigned long)((ble_pcap_event_total_count > ble_pcap_packet_count) ? (ble_pcap_event_total_count - ble_pcap_packet_count) : 0),
             (unsigned long)ble_pcap_event_total_count);
    }
    /* reset counters for next capture */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    int rc = ble_gap_disc_cancel();

    if (rc == 0) {
        if (!pcap_is_wireshark_mode()) {
            glog("BLE skimmer detection stopped successfully.\n");
        }
        status_display_show_status("Skimmer Stopped");
    } else {
        if (!pcap_is_wireshark_mode()) {
            glog("Error stopping BLE skimmer detection: %d\n", rc);
        }
        status_display_show_status("Skimmer Stop Fail");
    }
}

void ble_print_raw_packet_callback(struct ble_gap_event *event, size_t len) {
    char advertisementMac[18];
    format_mac_address(event->disc.addr.val, advertisementMac, sizeof(advertisementMac), false);

    // stop logging raw advertisement data
    //
    // printf("Received BLE Advertisement from MAC: %s, RSSI: %d\n",
    // advertisementMac, event->disc.rssi); TERMINAL_VIEW_ADD_TEXT("Received BLE
    // advertisementMac, advertisementRssi); TERMINAL_VIEW_ADD_TEXT("Received BLE
    // Advertisement from MAC: %s, RSSI: %d\n", advertisementMac,
    // advertisementRssi);

    // printf("Raw Advertisement Data (len=%zu): ", event->disc.length_data);
    // TERMINAL_VIEW_ADD_TEXT("Raw Advertisement Data (len=%zu): ",
    // event->disc.length_data); for (size_t i = 0; i < event->disc.length_data;
    // i++) {
    //     printf("%02x ", event->disc.data[i]);
    // }
    // printf("\n");
}

void detect_ble_spam_callback(struct ble_gap_event *event, size_t length) {
    if (length < 4) {
        return;
    }

    TickType_t current_time = xTaskGetTickCount();
    TickType_t time_elapsed = current_time - last_detection_time;

    uint16_t current_company_id;
    if (!extract_company_id(event->disc.data, length, &current_company_id)) {
        return;
    }

    if (time_elapsed > pdMS_TO_TICKS(TIME_WINDOW_MS)) {
        spam_counter = 0;
    }

    if (last_company_id_valid && last_company_id_value == current_company_id) {
        spam_counter++;

        if (spam_counter > MAX_PAYLOADS) {
            ESP_LOGW(TAG_BLE, "BLE Spam detected! Company ID: 0x%04X", current_company_id);
            TERMINAL_VIEW_ADD_TEXT("BLE Spam detected! Company ID: 0x%04X\n", current_company_id);
            spam_counter = 0;
        }
    } else {
        last_company_id_value = current_company_id;
        last_company_id_valid = true;
        spam_counter = 1;
    }

    last_detection_time = current_time;
}

void airtag_scanner_callback(struct ble_gap_event *event, size_t len) {
    if (!airtag_scanner_active) {
        return;
    }
    
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (!event->disc.data || event->disc.length_data < 4) {
            return;
        }

        const uint8_t *payload = event->disc.data;
        size_t payloadLength = event->disc.length_data;

        bool patternFound = false;
        for (size_t i = 0; i <= payloadLength - 4; i++) {
            if ((payload[i] == 0x1E && payload[i + 1] == 0xFF && payload[i + 2] == 0x4C &&
                 payload[i + 3] == 0x00) || // Pattern 1 (Nearby)
                (payload[i] == 0x4C && payload[i + 1] == 0x00 && payload[i + 2] == 0x12 &&
                 payload[i + 3] == 0x19)) { // Pattern 2 (Offline Finding)
                patternFound = true;
                break;
            }
        }

        if (patternFound) {
            // Check if this AirTag is already discovered
            bool already_discovered = false;
            for (int i = 0; i < discovered_airtag_count; i++) {
                if (memcmp(discovered_airtags[i].addr.val, event->disc.addr.val, 6) == 0) {
                    already_discovered = true;
                    // Update RSSI and maybe payload if needed
                    discovered_airtags[i].rssi = event->disc.rssi;

                    TickType_t now = xTaskGetTickCount();
                    TickType_t elapsed = now - airtag_last_rssi_log[i];
                    if (airtag_last_rssi_log[i] == 0 || elapsed >= pdMS_TO_TICKS(AIRTAG_RSSI_LOG_INTERVAL_MS)) {
                        char macAddress[18];
                        format_mac_address(discovered_airtags[i].addr.val, macAddress, sizeof(macAddress), false);

                        glog("AirTag RSSI update: idx %d MAC %s RSSI %d dBm\n", i, macAddress, event->disc.rssi);
                        airtag_last_rssi_log[i] = now;
                    }

                    // Optionally update payload if it can change
                    // memcpy(discovered_airtags[i].payload, payload, payloadLength);
                    // discovered_airtags[i].payload_len = payloadLength;
                    break;
                }
            }

            if (!already_discovered && discovered_airtag_count < MAX_AIRTAGS) {
                // Add new AirTag
                AirTagDevice *new_tag = &discovered_airtags[discovered_airtag_count];
                memcpy(new_tag->addr.val, event->disc.addr.val, 6);
                new_tag->addr.type = event->disc.addr.type;
                new_tag->rssi = event->disc.rssi;
                memcpy(new_tag->payload, payload, payloadLength);
                new_tag->payload_len = payloadLength;
                new_tag->selected_for_spoofing = false;
                discovered_airtag_count++;
                airTagCount++; // Increment the original counter too, maybe rename it later
                airtag_last_rssi_log[discovered_airtag_count - 1] = xTaskGetTickCount();

            char macAddress[18];
            format_mac_address(event->disc.addr.val, macAddress, sizeof(macAddress), false);

            int rssi = event->disc.rssi;

                glog("New AirTag found! (Total: %d)\n", airTagCount);
                glog("Index: %d\n", discovered_airtag_count - 1); // Index of the newly added tag
                glog("MAC Address: %s\n", macAddress);
                glog("RSSI: %d dBm\n", rssi);
                char payload_line[256];
                size_t payload_off = 0;
                payload_off += snprintf(payload_line + payload_off,
                                        sizeof(payload_line) - payload_off,
                                        "Payload Data: ");
                for (size_t i = 0; i < payloadLength && payload_off + 4 < sizeof(payload_line); i++) {
                    payload_off += snprintf(payload_line + payload_off,
                                            sizeof(payload_line) - payload_off,
                                            "%02X ", payload[i]);
                }
                glog("%s", payload_line);
            }
        }
    }
}

// Function to list discovered AirTags
void ble_list_airtags(void) {
    airtag_scan_print_results();
}

// Function to select an AirTag by index
void ble_select_airtag(int index) {
    airtag_scan_select(index);
}

// Function to start spoofing the selected AirTag (Basic Implementation)
void ble_start_spoofing_selected_airtag(void) {
    airtag_scan_start_spoofing();
}

// Function to stop any ongoing spoofing advertisement
void ble_stop_spoofing(void) {
    airtag_scan_stop_spoofing();
}

static bool wait_for_ble_ready(void) {
    int rc;
    int retry_count = 0;
    const int max_retries = 50; // 5 seconds total timeout

    while (!ble_stack_ready && !ble_hs_synced() && retry_count < max_retries) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for 100ms
        retry_count++;
    }

    if (!ble_stack_ready && !ble_hs_synced()) {
        ESP_LOGE(TAG_BLE, "Timeout waiting for BLE stack sync");
        return false;
    }

    if (ble_hs_synced()) {
        ble_stack_ready = true;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Failed to set BLE address");
        return false;
    }

    return true;
}

bool ble_start_scanning(void) {
    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready");
        TERMINAL_VIEW_ADD_TEXT("BLE stack not ready\n");
        status_display_show_status("BLE Not Ready");
        return false;
    }

    if (ble_gap_disc_active()) {
        if (ble_disc_complete_sem != NULL) {
            (void)xSemaphoreTake(ble_disc_complete_sem, 0);
        }
        (void)ble_gap_disc_cancel();
        (void)ble_wait_for_scan_stop(800);
    }

    if (ble_disc_complete_sem == NULL) {
        ble_disc_complete_sem = xSemaphoreCreateBinary();
    }
    if (ble_disc_complete_sem != NULL) {
        (void)xSemaphoreTake(ble_disc_complete_sem, 0);
    }

    struct ble_gap_disc_params disc_params = {0};
    disc_params.itvl = BLE_HCI_SCAN_ITVL_DEF;
    disc_params.window = BLE_HCI_SCAN_WINDOW_DEF;
    disc_params.filter_duplicates = 0;

    // Infer the correct own address type (Public or Random)
    uint8_t own_addr_type;
    int rc_addr = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc_addr != 0) {
        ESP_LOGE(TAG_BLE, "Failed to infer own address type: %d", rc_addr);
        own_addr_type = BLE_OWN_ADDR_PUBLIC; // Fallback
    }

    // Start a new BLE scan
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event_general,
                          NULL);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error starting BLE scan");
        TERMINAL_VIEW_ADD_TEXT("Error starting BLE scan\n");
        status_display_show_status("BLE Scan Fail");
        return false;
    } else {
        ESP_LOGI(TAG_BLE, "Scanning started...");
        TERMINAL_VIEW_ADD_TEXT("Scanning started...\n");
        status_display_show_status("BLE Scanning");
        return true;
    }
}

esp_err_t ble_register_handler(ble_data_handler_t handler) {
    if (handler_count >= MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    handlers[handler_count].handler = handler;
    handler_count++;
    return ESP_OK;
}

esp_err_t ble_unregister_handler(ble_data_handler_t handler) {
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].handler == handler) {
            for (int j = i; j < handler_count - 1; j++) {
                handlers[j] = handlers[j + 1];
            }
            handler_count--;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

void ble_init(void) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    // --- pre-init ram check ---
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    
    if (free_psram > 0) {
        ESP_LOGI(TAG_BLE, "pre-init ram: internal=%d bytes (largest block=%d), psram=%d bytes (largest block=%d)", 
                 (int)free_internal, (int)largest_internal, (int)free_psram, (int)largest_psram);
        TERMINAL_VIEW_ADD_TEXT("pre-init ram: internal=%d bytes (largest=%d), psram=%d bytes (largest=%d)\n", 
                               (int)free_internal, (int)largest_internal, (int)free_psram, (int)largest_psram);
    } else {
        ESP_LOGI(TAG_BLE, "pre-init ram: internal=%d bytes (largest block=%d), no psram", 
                 (int)free_internal, (int)largest_internal);
        TERMINAL_VIEW_ADD_TEXT("pre-init ram: internal=%d bytes (largest=%d), no psram\n", 
                               (int)free_internal, (int)largest_internal);
    }
    
    // --- Memory check before BLE init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG_BLE, "WARNING: Less than 45KB of free RAM available (%d bytes). BLE may fail to initialize!", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB RAM free (%d bytes). BLE may not initialize!\n", (int)free_heap);
    }

    if (!ble_initialized) {
        ble_stack_ready = false;
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // NVS partition was truncated and needs to be erased
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        ble_suspend_networking();

        memset(handlers, 0, sizeof(handlers));
        handler_count = 0;

        // Release Classic BT controller memory on non-ESP32 targets too to free RAM for NimBLE
        // Safe to call multiple times; ignore return if already released
        (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        // log DMA-capable internal heap info right before NimBLE init
        size_t free_internal_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        size_t largest_internal_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_LOGI(TAG_BLE, "pre-init dma-ram: free=%d bytes (largest block=%d)", (int)free_internal_dma, (int)largest_internal_dma);
        TERMINAL_VIEW_ADD_TEXT("pre-init dma-ram: free=%d bytes (largest=%d)\n", (int)free_internal_dma, (int)largest_internal_dma);

        ble_prepare_hs_config();
        ret = nimble_port_init();
        if (ret != 0) {
            ESP_LOGE(TAG_BLE, "Failed to init nimble port: %d", ret);
            ESP_LOGI(TAG_BLE, "Dumping DMA-capable heap info after failure");
            size_t free_dma_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            size_t largest_dma_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            ESP_LOGI(TAG_BLE, "post-fail dma-ram: free=%d bytes (largest block=%d)", (int)free_dma_after, (int)largest_dma_after);
            ble_resume_networking();
            return;
        }

        // Create exit semaphore for safe task synchronization
        if (nimble_host_exit_sem == NULL) {
            nimble_host_exit_sem = xSemaphoreCreateBinary();
        }

        if (ble_disc_complete_sem == NULL) {
            ble_disc_complete_sem = xSemaphoreCreateBinary();
        }

        // Configure and start the NimBLE host task (larger stack to avoid overflow on S3)
        xTaskCreate(nimble_host_task, "nimble_host", NIMBLE_HOST_TASK_STACK_SIZE, NULL, 5, &nimble_host_task_handle);
        
        // Wait for NimBLE stack to be ready
        vTaskDelay(pdMS_TO_TICKS(100));
        
        ESP_LOGI(TAG_BLE, "BLE configured for random address support");

        ble_initialized = true;
        ESP_LOGI(TAG_BLE, "BLE initialized");
        TERMINAL_VIEW_ADD_TEXT("BLE initialized\n");
    }
#endif
}

void ble_deinit(void) {
    if (ble_initialized) {
        ble_spam_stop();
        ble_stop_spoofing();
        if (flipper_scan_is_active()) {
            flipper_scan_stop();
        }
        if (airtag_scan_is_active()) {
            airtag_scan_stop();
        }
        if (gatt_scan_is_active()) {
            gatt_scan_stop();
        }

        handler_count = 0;

        ble_pending_clear = true;
        if (ble_disc_complete_sem != NULL) {
            (void)xSemaphoreTake(ble_disc_complete_sem, 0);
        }

        if (ble_gap_disc_active()) {
            (void)ble_gap_disc_cancel();
            if (!ble_wait_for_scan_stop(1200)) {
                ESP_LOGW(TAG_BLE, "ble_deinit: scan did not stop before timeout");
            }
        }

        if (!ble_wait_for_callbacks_idle(1200)) {
            ESP_LOGW(TAG_BLE, "ble_deinit: callbacks still busy before host stop");
        }

        vTaskDelay(pdMS_TO_TICKS(100));

        if (nimble_host_task_handle != NULL) {
            nimble_port_stop();
            if (nimble_host_exit_sem != NULL) {
                if (xSemaphoreTake(nimble_host_exit_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGW(TAG_BLE, "nimble_host_task did not signal exit in time");
                }
            }
            nimble_host_task_handle = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        nimble_port_deinit();

        if (nimble_host_exit_sem != NULL) {
            vSemaphoreDelete(nimble_host_exit_sem);
            nimble_host_exit_sem = NULL;
        }

        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        size_t post_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t post_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG_BLE, "Post BLE deinit heap: free=%u, largest=%u", (unsigned)post_free, (unsigned)post_largest);

        ble_stack_ready = false;
        ble_initialized = false;
        ble_pending_clear = false;
        ble_cb_busy = false;
        ESP_LOGI(TAG_BLE, "BLE deinitialized successfully.");
        TERMINAL_VIEW_ADD_TEXT("BLE deinitialized successfully.\n");

        ble_resume_networking();
    }
}

bool ble_is_initialized(void) {
    return ble_initialized;
}

bool ble_is_stack_ready(void) {
    return ble_stack_ready;
}

bool ble_wait_for_ready(void) {
    return wait_for_ble_ready();
}

void ble_stop(void) {
    ESP_LOGI(TAG_BLE, "ble_stop called, ble_initialized=%d", ble_initialized);
    if (!ble_initialized) {
        ESP_LOGW(TAG_BLE, "ble_stop: BLE not initialized, skipping");
        return;
    }

    status_display_show_status("BLE Stopping");

    if (!ble_gap_disc_active()) {
        ble_deinit();
        return;
    }

    last_company_id_valid = false;

    // Stop and delete the flush timer if it exists
    if (flush_timer != NULL) {
        esp_timer_stop(flush_timer);
        esp_timer_delete(flush_timer);
        flush_timer = NULL;
    }

    rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false);

    if (airtag_scanner_active && discovered_airtag_count > 0) {
        scan_file_t sf = SCAN_FILE_INIT;
        if (scan_file_open(&sf, "airtag_scan", "txt") == ESP_OK) {
            scan_file_printf(&sf, "--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
            for (int i = 0; i < discovered_airtag_count; i++) {
                char mac[18];
                format_mac_address(discovered_airtags[i].addr.val, mac, sizeof(mac), false);
                scan_file_printf(&sf, "[%d] MAC: %s, RSSI: %d dBm\n",
                                 i, mac, discovered_airtags[i].rssi);
            }
            scan_file_close(&sf);
        }
    }

    airtag_scanner_active = false;
    ble_unregister_handler(airtag_scanner_callback);
    ble_unregister_handler(ble_print_raw_packet_callback);
    ble_unregister_handler(ble_pcap_callback);
    ble_unregister_handler(ble_skimmer_scan_callback);
    ble_unregister_handler(detect_ble_spam_callback);
    pcap_flush_buffer_to_file(); // Final flush
    pcap_file_close();           // Close the file after final flush

    /* final capture summary */
    if (!pcap_is_wireshark_mode()) {
        glog("BLE capture summary: captured=%lu filtered=%lu total=%lu\n",
             (unsigned long)ble_pcap_packet_count,
             (unsigned long)((ble_pcap_event_total_count > ble_pcap_packet_count) ? (ble_pcap_event_total_count - ble_pcap_packet_count) : 0),
             (unsigned long)ble_pcap_event_total_count);
    }
    /* reset counters for next capture */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    ble_spam_stop();
    ble_stop_spoofing();

    if (flipper_scan_is_active()) {
        flipper_scan_stop();
    }
    if (airtag_scan_is_active()) {
        airtag_scan_stop();
    }
    if (gatt_scan_is_active()) {
        gatt_scan_stop();
    }

    ble_pending_clear = true;
    if (ble_disc_complete_sem != NULL) {
        (void)xSemaphoreTake(ble_disc_complete_sem, 0);
    }

    int rc = ble_gap_disc_cancel();
    bool scan_stopped = ble_wait_for_scan_stop(1200);

    if (!ble_wait_for_callbacks_idle(1200)) {
        ESP_LOGW(TAG_BLE, "ble_stop: callbacks still busy after scan cancel");
    }

    switch (rc) {
    case 0:
        if (!pcap_is_wireshark_mode()) {
            if (scan_stopped) {
                glog("BLE scan stopped successfully.\n");
            } else {
                glog("BLE scan cancel requested, but scan still active after timeout.\n");
            }
        }
        status_display_show_status(scan_stopped ? "BLE Stopped" : "BLE Stop Wait");
        break;
    case BLE_HS_EBUSY:
        if (!pcap_is_wireshark_mode()) {
            glog("BLE scan is busy\n");
        }
        status_display_show_status("BLE Busy");
        break;
    case BLE_HS_ETIMEOUT:
        if (!pcap_is_wireshark_mode()) {
            glog("BLE operation timed out.\n");
        }
        status_display_show_status("BLE Timeout");
        break;
    case BLE_HS_ENOTCONN:
        if (!pcap_is_wireshark_mode()) {
            glog("BLE not connected.\n");
        }
        status_display_show_status("BLE No Conn");
        break;
    case BLE_HS_EINVAL:
        if (!pcap_is_wireshark_mode()) {
            glog("BLE invalid parameter.\n");
        }
        status_display_show_status("BLE Invalid");
        break;
    default:
        if (!pcap_is_wireshark_mode()) {
            glog("Error stopping BLE scan: %d\n", rc);
        }
        status_display_show_status("BLE Stop Fail");
    }

    ble_deinit();
}

void ble_start_blespam_detector(void) {
    // Register the skimmer detection callback
    esp_err_t err = ble_register_handler(detect_ble_spam_callback);
    if (err != ESP_OK) {
        ESP_LOGE("BLE", "Failed to register skimmer detection callback");
        return;
    }

    // Start BLE scanning
    ble_start_scanning();
}

void ble_start_raw_ble_packetscan(void) {
    ble_start_capture();
}

void ble_start_airtag_scanner(void) {
    airtag_scan_start();
}

static void ble_pcap_callback(struct ble_gap_event *event, size_t len) {
    if (!event || len == 0)
        return;

    /* count every callback event we receive for filtering stats */
    ble_pcap_event_total_count++;

    uint8_t hci_buffer[258]; // Max HCI packet size
    size_t hci_len = 0;

    if (event->type == BLE_GAP_EVENT_DISC) {
        hci_buffer[0] = 0x04; // HCI packet type (HCI Event)
        hci_buffer[1] = 0x3E; // HCI Event Code (LE Meta Event)

        uint8_t param_len = 10 + event->disc.length_data;
        hci_buffer[2] = param_len;

        hci_buffer[3] = 0x02; // LE Meta Subevent (LE Advertising Report)
        hci_buffer[4] = 0x01; // Number of reports
        hci_buffer[5] = 0x00; // Event type (ADV_IND)
        hci_buffer[6] = event->disc.addr.type;
        memcpy(&hci_buffer[7], event->disc.addr.val, 6);
        hci_buffer[13] = event->disc.length_data;

        if (event->disc.length_data > 0) {
            memcpy(&hci_buffer[14], event->disc.data, event->disc.length_data);
        }

        hci_buffer[14 + event->disc.length_data] = (uint8_t)event->disc.rssi;

        hci_len = 15 + event->disc.length_data;

        /* keep a lightweight counter and occasionally report a summary */

        ble_pcap_packet_count++;
        if ((ble_pcap_packet_count % 50) == 0) {
            uint32_t filtered = ble_pcap_event_total_count - ble_pcap_packet_count;
            if (!pcap_is_wireshark_mode()) {
                glog("BLE: %lu packets captured, %lu filtered (total events %lu)\n",
                     (unsigned long)ble_pcap_packet_count, (unsigned long)filtered, (unsigned long)ble_pcap_event_total_count);
            }
        }

        pcap_write_packet_to_buffer(hci_buffer, hci_len, PCAP_CAPTURE_BLUETOOTH);
    }
}

void ble_start_capture(void) {
    /* ensure BLE stack is initialized and ready before opening PCAP and
       starting scanning; avoids leaving a file open if scan fails */
    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready");
        TERMINAL_VIEW_ADD_TEXT("BLE stack not ready\n");
        return;
    }

    /* reset counters for a fresh capture session */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    // Open PCAP file first
    esp_err_t err = pcap_file_open("ble_capture", PCAP_CAPTURE_BLUETOOTH);
    if (err != ESP_OK) {
        ESP_LOGE("BLE_PCAP", "Failed to open PCAP file");
        return;
    }

    // Register BLE handler only after file is open
    err = ble_register_handler(ble_pcap_callback);
    if (err != ESP_OK) {
        ESP_LOGE("BLE_PCAP", "Failed to register BLE capture handler: %s", esp_err_to_name(err));
        pcap_file_close();
        return;
    }

    // Create a timer to flush the buffer periodically
    esp_timer_create_args_t timer_args = {.callback = (esp_timer_cb_t)pcap_flush_buffer_to_file,
                                          .name = "pcap_flush"};

    if (esp_timer_create(&timer_args, &flush_timer) == ESP_OK) {
        esp_timer_start_periodic(flush_timer, 1000000); // Flush every second
    }

    if (!ble_start_scanning()) {
        ble_unregister_handler(ble_pcap_callback);
        if (flush_timer != NULL) {
            esp_timer_stop(flush_timer);
            esp_timer_delete(flush_timer);
            flush_timer = NULL;
        }
        pcap_file_close();
        return;
    }
    status_display_show_status("BLE Capture On");
}

void ble_start_capture_wireshark(void) {
    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready");
        return;
    }

    /* reset counters for a fresh capture session */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    if (flush_timer != NULL) {
        esp_timer_stop(flush_timer);
        esp_timer_delete(flush_timer);
        flush_timer = NULL;
    }

    /* Register BLE handler to emit HCI packets into the PCAP buffer */
    esp_err_t err = ble_register_handler(ble_pcap_callback);
    if (err != ESP_OK) {
        ESP_LOGE("BLE_PCAP", "Failed to register BLE wireshark handler: %s", esp_err_to_name(err));
        return;
    }

    /* Flush more frequently for better live capture latency */
    esp_timer_create_args_t timer_args = {.callback = (esp_timer_cb_t)pcap_flush_buffer_to_file,
                                          .name = "pcap_flush"};

    if (esp_timer_create(&timer_args, &flush_timer) == ESP_OK) {
        esp_timer_start_periodic(flush_timer, 200000);
    }

    if (!ble_start_scanning()) {
        ble_unregister_handler(ble_pcap_callback);
        if (flush_timer != NULL) {
            esp_timer_stop(flush_timer);
            esp_timer_delete(flush_timer);
            flush_timer = NULL;
        }
        return;
    }
    status_display_show_status("BLE Wireshark");
}

void ble_start_skimmer_detection(void) {
    // Register the skimmer detection callback
    esp_err_t err = ble_register_handler(ble_skimmer_scan_callback);
    if (err != ESP_OK) {
        ESP_LOGE("BLE", "Failed to register skimmer detection callback");
        return;
    }

    // Start BLE scanning
    ble_start_scanning();
}

// GATT scan wrapper functions - delegate to gatt_scan module
int ble_get_gatt_device_count(void) {
    return gatt_scan_get_device_count();
}

int ble_get_gatt_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len) {
    return gatt_scan_get_device_data(index, mac, rssi, name, name_len);
}

// BLE spam wrapper functions - delegate to ble_spam module
void ble_start_ble_spam(ble_spam_type_t type) {
    ble_spam_start(type);
}

void ble_stop_ble_spam(void) {
    ble_spam_stop();
}

static void gatt_uuid_to_str(const ble_uuid_any_t *uuid, char *buf, size_t buf_len) {
    if (uuid->u.type == BLE_UUID_TYPE_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->u16.value);
    } else if (uuid->u.type == BLE_UUID_TYPE_32) {
        snprintf(buf, buf_len, "0x%08lX", (unsigned long)uuid->u32.value);
    } else if (uuid->u.type == BLE_UUID_TYPE_128) {
        snprintf(buf, buf_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->u128.value[15], uuid->u128.value[14], uuid->u128.value[13], uuid->u128.value[12],
                 uuid->u128.value[11], uuid->u128.value[10], uuid->u128.value[9], uuid->u128.value[8],
                 uuid->u128.value[7], uuid->u128.value[6], uuid->u128.value[5], uuid->u128.value[4],
                 uuid->u128.value[3], uuid->u128.value[2], uuid->u128.value[1], uuid->u128.value[0]);
    } else {
        snprintf(buf, buf_len, "Unknown");
    }
}

static const char* gatt_svc_uuid_to_name(const ble_uuid_any_t *uuid) {
    if (uuid->u.type != BLE_UUID_TYPE_16) {
        return NULL;
    }
    
    switch (uuid->u16.value) {
        case 0x1800: return "Generic Access";
        case 0x1801: return "Generic Attribute";
        case 0x1802: return "Immediate Alert";
        case 0x1803: return "Link Loss";
        case 0x1804: return "Tx Power";
        case 0x1805: return "Current Time";
        case 0x1806: return "Reference Time Update";
        case 0x1807: return "Next DST Change";
        case 0x1808: return "Glucose";
        case 0x1809: return "Health Thermometer";
        case 0x180A: return "Device Information";
        case 0x180D: return "Heart Rate";
        case 0x180E: return "Phone Alert Status";
        case 0x180F: return "Battery";
        case 0x1810: return "Blood Pressure";
        case 0x1811: return "Alert Notification";
        case 0x1812: return "Human Interface Device";
        case 0x1813: return "Scan Parameters";
        case 0x1814: return "Running Speed and Cadence";
        case 0x1815: return "Automation IO";
        case 0x1816: return "Cycling Speed and Cadence";
        case 0x1818: return "Cycling Power";
        case 0x1819: return "Location and Navigation";
        case 0x181A: return "Environmental Sensing";
        case 0x181B: return "Body Composition";
        case 0x181C: return "User Data";
        case 0x181D: return "Weight Scale";
        case 0x181E: return "Bond Management";
        case 0x181F: return "Continuous Glucose Monitoring";
        case 0x1820: return "Internet Protocol Support";
        case 0x1821: return "Indoor Positioning";
        case 0x1822: return "Pulse Oximeter";
        case 0x1823: return "HTTP Proxy";
        case 0x1824: return "Transport Discovery";
        case 0x1825: return "Object Transfer";
        case 0x1826: return "Fitness Machine";
        case 0x1827: return "Mesh Provisioning";
        case 0x1828: return "Mesh Proxy";
        case 0x1829: return "Reconnection Configuration";
        // Common Vendor Specific
        case 0xFEAA: return "Google Eddystone";
        case 0xFE9F: return "Google Nearby";
        case 0xFEE0: return "Xiaomi/Amazfit";
        case 0xFE95: return "Xiaomi";
        case 0xFE07: return "Sonos";
        case 0xFEB8: return "Meta/Oculus";
        case 0xFDAC: return "Tencent";
        case 0xFE2C: return "Google";
        case 0xFD6F: return "Exposure Notification (COVID-19)";
        default: return NULL;
    }
}

// GATT scan wrapper functions - delegate to gatt_scan module
void ble_start_gatt_scan(void) {
    gatt_scan_start();
}

void ble_list_gatt_devices(void) {
    gatt_scan_print_devices();
}

void ble_select_gatt_device(int index) {
    gatt_scan_select_device(index);
}

void ble_enumerate_gatt_services(void) {
    gatt_scan_enumerate_services();
}

void ble_track_gatt_device(void) {
    gatt_scan_track_device();
}

void ble_stop_tracking(void) {
    gatt_scan_stop_tracking();
}

void ble_stop_gatt_scan(void) {
    gatt_scan_stop();
}

#endif
