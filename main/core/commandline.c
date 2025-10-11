 // command.c

#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/serial_manager.h"
#include "esp_sntp.h"
#include "managers/ap_manager.h"
#include "sdkconfig.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include "managers/dial_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#include "vendor/pcap.h"
#include "vendor/printer.h"
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
#include "managers/zigbee_manager.h"
#endif
#include <esp_timer.h>
#include <managers/gps_manager.h>
#include <managers/views/terminal_screen.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <vendor/dial_client.h>
#include "esp_wifi.h"
#include "managers/default_portal.h"
#include "core/glog.h"
#include <time.h>
#include <dirent.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"

static const char *TAG = "Commandline";

#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

#ifndef DISCOVER_TASK_STACK
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
#define DISCOVER_TASK_STACK 4096
#else
#define DISCOVER_TASK_STACK 6144
#endif
#endif

static Command *command_list_head = NULL;
TaskHandle_t VisualizerHandle = NULL;
TaskHandle_t gps_info_task_handle = NULL;

// Forward declarations for command handlers
void cmd_wifi_scan_stop(int argc, char **argv);
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv);
void handle_select_airtag(int argc, char **argv);
void handle_spoof_airtag(int argc, char **argv);
void handle_stop_spoof(int argc, char **argv);
void handle_ble_spam_cmd(int argc, char **argv);
#endif

#define MAX_PORTAL_PATH_LEN 128 // reasonable i guess?

void command_init() { command_list_head = NULL; }

void register_command(const char *name, CommandFunction function) {
    // Check if the command already exists
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Command already registered
            return;
        }
        current = current->next;
    }

    // Create a new command
    Command *new_command = (Command *)malloc(sizeof(Command));
    if (new_command == NULL) {
        // Handle memory allocation failure
        return;
    }
    new_command->name = strdup(name);
    new_command->function = function;
    new_command->next = command_list_head;
    command_list_head = new_command;
}

void unregister_command(const char *name) {
    Command *current = command_list_head;
    Command *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Found the command to remove
            if (previous == NULL) {
                command_list_head = current->next;
            } else {
                previous->next = current->next;
            }
            free(current->name);
            free(current);
            return;
        }
        previous = current;
        current = current->next;
    }
}

CommandFunction find_command(const char *name) {
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current->function;
        }
        current = current->next;
    }
    return NULL;
}

void handle_unknown_command(const char *cmd) {
    glog("Unknown command: %s\n", cmd);
}

void cmd_wifi_scan_start(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-stop") == 0) {
            cmd_wifi_scan_stop(argc, argv);
            return;
        }
        if (strcmp(argv[1], "-live") == 0) {
            glog("Starting live AP scan...\n");
            wifi_manager_start_live_ap_scan();
            return;
        }
        int seconds = atoi(argv[1]);
        wifi_manager_start_scan_with_time(seconds);
    } else {
        wifi_manager_start_scan();
    }
    wifi_manager_print_scan_results_with_oui();
    status_display_show_status("Scan Started");
}

void cmd_wifi_scan_stop(int argc, char **argv) {
    // Properly stop any ongoing WiFi scan
    wifi_manager_stop_scan();

    // Stop monitor mode
    wifi_manager_stop_monitor_mode();

    // Close pcap file
    pcap_file_close();

    // Reset WiFi to a good state
    esp_wifi_stop();
    esp_wifi_start();

    glog("WiFi scan stopped.\n");
    status_display_show_status("Scan Stopped");
}

// settings registry to avoid ridiculously long strcmp chains, fuck that lmaooo.
typedef enum {
    ST_I32,
    ST_U32,
    ST_U16,
    ST_U8,
    ST_BOOL,
    ST_FLOAT,
    ST_ENUM8,
    ST_STRING,
    ST_COLOR_HEX
} SettingType;

typedef struct {
    const char *name;
    SettingType type;
    size_t offset;
    const char *category;
    uint16_t str_capacity; // only for ST_STRING
    int min_i; // for *_U8/_U16/_I32/_ENUM8 bounds (simple)
    int max_i;
} SettingDescriptor;

#define OFF(field) offsetof(FSettings, field)

static const SettingDescriptor k_settings_desc[] = {
    {"rgb_mode", ST_ENUM8, OFF(rgb_mode), "RGB", 0, 0, 2},
    {"rgb_speed", ST_U8, OFF(rgb_speed), "RGB", 0, 0, 255},
    {"rgb_data_pin", ST_I32, OFF(rgb_data_pin), "RGB", 0, 0, 0},
    {"rgb_red_pin", ST_I32, OFF(rgb_red_pin), "RGB", 0, 0, 0},
    {"rgb_green_pin", ST_I32, OFF(rgb_green_pin), "RGB", 0, 0, 0},
    {"rgb_blue_pin", ST_I32, OFF(rgb_blue_pin), "RGB", 0, 0, 0},
    {"neopixel_bright", ST_U8, OFF(neopixel_max_brightness), "RGB", 0, 0, 100},

    {"ap_ssid", ST_STRING, OFF(ap_ssid), "WiFi", 33, 0, 0},
    {"ap_password", ST_STRING, OFF(ap_password), "WiFi", 65, 0, 0},
    {"ap_enabled", ST_BOOL, OFF(ap_enabled), "WiFi", 0, 0, 0},
    {"sta_ssid", ST_STRING, OFF(sta_ssid), "WiFi", 65, 0, 0},
    {"sta_password", ST_STRING, OFF(sta_password), "WiFi", 65, 0, 0},

    {"portal_url", ST_STRING, OFF(portal_url), "Portal", 129, 0, 0},
    {"portal_ssid", ST_STRING, OFF(portal_ssid), "Portal", 33, 0, 0},
    {"portal_password", ST_STRING, OFF(portal_password), "Portal", 65, 0, 0},
    {"portal_ap_ssid", ST_STRING, OFF(portal_ap_ssid), "Portal", 33, 0, 0},
    {"portal_domain", ST_STRING, OFF(portal_domain), "Portal", 65, 0, 0},
    {"portal_offline", ST_BOOL, OFF(portal_offline_mode), "Portal", 0, 0, 0},

    {"printer_ip", ST_STRING, OFF(printer_ip), "Printer", 16, 0, 0},
    {"printer_text", ST_STRING, OFF(printer_text), "Printer", 257, 0, 0},
    {"printer_font_size", ST_U8, OFF(printer_font_size), "Printer", 0, 1, 255},
    {"printer_alignment", ST_ENUM8, OFF(printer_alignment), "Printer", 0, 0, 4},

    {"display_timeout", ST_U32, OFF(display_timeout_ms), "Display", 0, 0, 0},
    {"max_bright", ST_U8, OFF(max_screen_brightness), "Display", 0, 0, 100},
    {"invert_colors", ST_BOOL, OFF(invert_colors), "Display", 0, 0, 0},
    {"terminal_color", ST_COLOR_HEX, OFF(terminal_text_color), "Display", 0, 0, 0},
    {"menu_theme", ST_U8, OFF(menu_theme), "Display", 0, 0, 255},

    {"channel_delay", ST_FLOAT, OFF(channel_delay), "System", 0, 0, 0},
    {"broadcast_speed", ST_U16, OFF(broadcast_speed), "System", 0, 0, 65535},
    {"gps_rx_pin", ST_I32, OFF(gps_rx_pin), "System", 0, 0, 0},
    {"power_save", ST_BOOL, OFF(power_save_enabled), "System", 0, 0, 0},
    {"zebra_menus", ST_BOOL, OFF(zebra_menus_enabled), "System", 0, 0, 0},
    {"nav_buttons", ST_BOOL, OFF(nav_buttons_enabled), "System", 0, 0, 0},
    {"menu_layout", ST_U8, OFF(menu_layout), "System", 0, 0, 2},
    {"infrared_easy", ST_BOOL, OFF(infrared_easy_mode), "System", 0, 0, 0},
    {"web_auth", ST_BOOL, OFF(web_auth_enabled), "System", 0, 0, 0},
    {"rts_enabled", ST_BOOL, OFF(rts_enabled), "System", 0, 0, 0},
    {"third_ctrl", ST_BOOL, OFF(third_control_enabled), "System", 0, 0, 0},

    {"flappy_name", ST_STRING, OFF(flappy_ghost_name), "Custom", 65, 0, 0},
    {"timezone", ST_STRING, OFF(selected_timezone), "Custom", 25, 0, 0},
    {"accent_color", ST_STRING, OFF(selected_hex_accent_color), "Custom", 25, 0, 0},
};

static const SettingDescriptor *find_setting_desc(const char *name) {
    for (size_t i = 0; i < (sizeof(k_settings_desc)/sizeof(k_settings_desc[0])); ++i) {
        if (strcmp(k_settings_desc[i].name, name) == 0) return &k_settings_desc[i];
    }
    return NULL;
}

static void print_setting_value(const SettingDescriptor *d, const FSettings *s) {
    const uint8_t *base = (const uint8_t *)s;
    const void *ptr = base + d->offset;
    if (d->type == ST_STRING) {
        glog("%s = \"%s\"\n", d->name, (const char *)ptr);
        return;
    }
    switch (d->type) {
        case ST_BOOL: {
            bool v = *(const bool *)ptr;
            glog("%s = %s\n", d->name, v ? "true" : "false");
        } break;
        case ST_U8: {
            glog("%s = %d\n", d->name, *(const uint8_t *)ptr);
        } break;
        case ST_U16: {
            glog("%s = %d\n", d->name, *(const uint16_t *)ptr);
        } break;
        case ST_U32: {
            glog("%s = %lu\n", d->name, (unsigned long)*(const uint32_t *)ptr);
        } break;
        case ST_I32: {
            glog("%s = %ld\n", d->name, (long)*(const int32_t *)ptr);
        } break;
        case ST_FLOAT: {
            glog("%s = %.2f\n", d->name, *(const float *)ptr);
        } break;
        case ST_ENUM8: {
            glog("%s = %d\n", d->name, *(const uint8_t *)ptr);
        } break;
        case ST_COLOR_HEX: {
            unsigned long v = (unsigned long)*(const uint32_t *)ptr;
            glog("%s = 0x%06lX\n", d->name, v);
        } break;
        default: {
            glog("%s = ?\n", d->name);
        } break;
    }
}

static bool set_setting_value(const SettingDescriptor *d, FSettings *s, const char *value) {
    uint8_t *base = (uint8_t *)s;
    void *ptr = base + d->offset;
    switch (d->type) {
        case ST_STRING: {
            if (d->str_capacity == 0) return false;
            strncpy((char *)ptr, value, d->str_capacity - 1);
            ((char *)ptr)[d->str_capacity - 1] = '\0';
            return true;
        }
        case ST_BOOL: {
            if (strcmp(value, "true") == 0) {
                *(bool *)ptr = true; return true;
            } else if (strcmp(value, "false") == 0) {
                *(bool *)ptr = false; return true;
            }
            return false;
        }
        case ST_U8: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint8_t *)ptr = (uint8_t)v; return true;
        }
        case ST_U16: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint16_t *)ptr = (uint16_t)v; return true;
        }
        case ST_U32: {
            unsigned long v = strtoul(value, NULL, 10);
            *(uint32_t *)ptr = (uint32_t)v; return true;
        }
        case ST_I32: {
            long v = strtol(value, NULL, 10);
            *(int32_t *)ptr = (int32_t)v; return true;
        }
        case ST_FLOAT: {
            float v = atof(value);
            *(float *)ptr = v; return true;
        }
        case ST_ENUM8: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint8_t *)ptr = (uint8_t)v; return true;
        }
        case ST_COLOR_HEX: {
            unsigned long v = strtoul(value, NULL, 16);
            *(uint32_t *)ptr = (uint32_t)v; return true;
        }
        default:
            return false;
    }
}

static void reset_setting_value(const SettingDescriptor *d, FSettings *s, const FSettings *defaults) {
    const uint8_t *db = (const uint8_t *)defaults;
    const void *src = db + d->offset;
    uint8_t *sb = (uint8_t *)s;
    void *dst = sb + d->offset;
    switch (d->type) {
        case ST_STRING:
            strncpy((char *)dst, (const char *)src, d->str_capacity - 1), ((char *)dst)[d->str_capacity - 1] = '\0';
            break;
        case ST_BOOL:
            *(bool *)dst = *(const bool *)src; break;
        case ST_U8:
            *(uint8_t *)dst = *(const uint8_t *)src; break;
        case ST_U16:
            *(uint16_t *)dst = *(const uint16_t *)src; break;
        case ST_U32:
            *(uint32_t *)dst = *(const uint32_t *)src; break;
        case ST_I32:
            *(int32_t *)dst = *(const int32_t *)src; break;
        case ST_FLOAT:
            *(float *)dst = *(const float *)src; break;
        case ST_ENUM8:
            *(uint8_t *)dst = *(const uint8_t *)src; break;
        case ST_COLOR_HEX:
            *(uint32_t *)dst = *(const uint32_t *)src; break;
        default: break;
    }
}

static void log_set_confirmation(const SettingDescriptor *d, const FSettings *s) {
    const uint8_t *base = (const uint8_t *)s;
    const void *ptr = base + d->offset;
    switch (d->type) {
        case ST_STRING:
            glog("Set %s to \"%s\"\n", d->name, (const char *)ptr);
            break;
        case ST_BOOL:
            glog("Set %s to %s\n", d->name, (*(const bool *)ptr) ? "true" : "false");
            break;
        case ST_U8:
            glog("Set %s to %d\n", d->name, *(const uint8_t *)ptr);
            break;
        case ST_U16:
            glog("Set %s to %d\n", d->name, *(const uint16_t *)ptr);
            break;
        case ST_U32:
            glog("Set %s to %lu\n", d->name, (unsigned long)*(const uint32_t *)ptr);
            break;
        case ST_I32:
            glog("Set %s to %ld\n", d->name, (long)*(const int32_t *)ptr);
            break;
        case ST_FLOAT:
            glog("Set %s to %.2f\n", d->name, *(const float *)ptr);
            break;
        case ST_ENUM8:
            glog("Set %s to %d\n", d->name, *(const uint8_t *)ptr);
            break;
        case ST_COLOR_HEX: {
            unsigned long v = (unsigned long)*(const uint32_t *)ptr;
            glog("Set %s to 0x%06lX\n", d->name, v);
        } break;
        default:
            glog("Set %s\n", d->name);
            break;
    }
}

void cmd_wifi_scan_results(int argc, char **argv) {
    glog("WiFi scan results displaying with OUI matching.\n");
    wifi_manager_print_scan_results_with_oui();
    status_display_show_status("Showing Results");
}

void handle_list(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        cmd_wifi_scan_results(argc, argv);
        return;
    } else if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        wifi_manager_list_stations();
        glog("Listed Stations...\n");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    else if (argc > 1 && strcmp(argv[1], "-airtags") == 0) {
        ble_list_airtags();
        return;
    }
#endif
    else {
        glog("Usage: list -a (for Wi-Fi scan results)\n");
    }
}

void handle_beaconspam(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        glog("Starting Random beacon spam...\n");
        wifi_manager_start_beacon(NULL);
        status_display_show_status("Beacon Random");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-rr") == 0) {
        glog("Starting Rickroll beacon spam...\n");
        wifi_manager_start_beacon("RICKROLL");
        status_display_show_status("Beacon Rickroll");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        glog("Starting AP List beacon spam...\n");
        wifi_manager_start_beacon("APLISTMODE");
        status_display_show_status("Beacon AP List");
        return;
    }

    if (argc > 1) {
        wifi_manager_start_beacon(argv[1]);
        status_display_show_status("Custom Beacon");
        return;
    } else {
        glog("Usage: beaconspam -r (for Beacon Spam Random)\n");
        status_display_show_status("Beacon Usage");
    }
}

void handle_stop_spam(int argc, char **argv) {
    wifi_manager_stop_beacon();
    glog("Beacon Spam Stopped...\n");
    status_display_show_status("Beacon Stopped");
}

void handle_sta_scan(int argc, char **argv) {
    wifi_manager_start_station_scan();
    status_display_show_status("Station Scan");
}

void handle_attack_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-d") == 0) {
            glog("Deauthentication starting...\n");
            wifi_manager_deauth_station();
            status_display_show_status("Deauth Start");
            return;
        } else if (strcmp(argv[1], "-e") == 0) {
            glog("EAPOL Logoff attack starting...\n");
            wifi_manager_start_eapollogoff_attack();
            status_display_show_status("EAPOL Start");
            return;
        } else if (strcmp(argv[1], "-s") == 0) {
            if (argc < 3) {
                glog("Usage: attack -s <password>\n");
                status_display_show_status("Need Password");
                return;
            }
            glog("SAE flood attack starting...\n");
            wifi_manager_start_sae_flood(argv[2]);
            status_display_show_status("SAE Start");
            return;
        }
    }
    glog("Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s <password> (SAE flood)\n");
    status_display_show_status("Attack Usage");
}

void handle_sae_flood_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: saeflood <password>\n");
        return;
    }
    glog("Starting SAE flood attack...\n");
    wifi_manager_start_sae_flood(argv[1]);
    status_display_show_status("SAE Flood On");
}

void handle_stop_sae_flood_cmd(int argc, char **argv) {
    glog("Stopping SAE flood attack...\n");
    wifi_manager_stop_sae_flood();
    status_display_show_status("SAE Flood Off");
}

void handle_sae_flood_help_cmd(int argc, char **argv) {
    wifi_manager_sae_flood_help();
    status_display_show_status("SAE Help");
}

void handle_stop_deauth(int argc, char **argv) {
    wifi_manager_stop_deauth();
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
    glog("Deauth/EAPOL/SAE attacks stopped...\n");
    status_display_show_status("Attacks Off");
}

void handle_select_cmd(int argc, char **argv) {
    if (argc != 3) {
        glog("Usage: select -a <number[,number,...]> or select -s <number>\n");
        return;
    }

    if (strcmp(argv[1], "-a") == 0) {
        char *input = argv[2];
        char *comma = strchr(input, ',');
        
        if (comma == NULL) {
            char *endptr;
            int num = (int)strtol(input, &endptr, 10);
            if (*endptr == '\0') {
                wifi_manager_select_ap(num);
            } else {
                glog("Error: is not a valid number.\n");
            }
        } else {
            int indices[32];
            int count = 0;
            char *token = strtok(input, ",");
            
            while (token != NULL && count < 32) {
                char *endptr;
                int num = (int)strtol(token, &endptr, 10);
                if (*endptr == '\0') {
                    indices[count++] = num;
                } else {
                    glog("Error: '%s' is not a valid number.\n", token);
                    return;
                }
                token = strtok(NULL, ",");
            }
            
            if (count > 0) {
                wifi_manager_select_multiple_aps(indices, count);
            } else {
                glog("Error: No valid indices found.\n");
            }
        }
    } else if (strcmp(argv[1], "-s") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            wifi_manager_select_station(num);
        } else {
            glog("Error: is not a valid number.\n");
        }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    } else if (strcmp(argv[1], "-airtag") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            ble_select_airtag(num);
        } else {
            glog("Error: '%s' is not a valid number.\n", argv[2]);
        }
#endif
    } else {
        glog("Invalid option. Usage: select -a <number[,number,...]> or select -s <number>\n");
    }
}

void discover_task(void *pvParameter) {
    DIALClient client;
    DIALManager manager;

    if (dial_client_init(&client) == ESP_OK) {

        dial_manager_init(&manager, &client);

        explore_network(&manager);

        dial_client_deinit(&client);
    } else {
        glog("Failed to init DIAL client.\n");
        status_display_show_status("DIAL Failed");
    }

    {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        glog("discover_task min stack free: %u words\n", (unsigned)hwm);
    }
    vTaskDelete(NULL);
}

void handle_stop_flipper(int argc, char **argv) {
    wifi_manager_stop_deauth();
#ifndef CONFIG_IDF_TARGET_ESP32S2
    ble_stop();
    ble_stop_ble_spam();
#endif
    if (buffer_offset > 0) { // Only flush if there's data in buffer
        csv_flush_buffer_to_file();
    }
    csv_file_close();                  // Close any open CSV files
    gps_manager_deinit(&g_gpsManager); // Clean up GPS if active

    // also stop the gps info display task if it is running
    if (gps_info_task_handle != NULL) {
        vTaskDelete(gps_info_task_handle);
        gps_info_task_handle = NULL;
    }

    wifi_manager_stop_monitor_mode();  // Stop any active monitoring
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_deauth();
    wifi_manager_stop_dhcpstarve();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    // ensure zigbee capture is stopped when using generic stop
    zigbee_manager_stop_capture();
#endif
    // ensure pcap is properly flushed and closed
    pcap_file_close();
    glog("Stopped activities.\nClosed files.\n");
    status_display_show_status("All Stopped");

    // kill any feature tasks we spawned that may still be around
    if (VisualizerHandle != NULL) {
        vTaskDelete(VisualizerHandle);
        VisualizerHandle = NULL;
    }
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }
}

void handle_dial_command(int argc, char **argv) {
    // Usage: dial [device_name]
    if (argc > 2) {
        glog("Usage: %s [device_name]\n", argv[0]);
        return;
    }
    // If a device name is provided, set it before discovery
    if (argc == 2) {
        dial_manager_set_device_name(argv[1]);
    }
    xTaskCreate(&discover_task, "discover_task", DISCOVER_TASK_STACK, NULL, 5, NULL);
}

static void dump_task_stacks(void) {
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    UBaseType_t num = uxTaskGetNumberOfTasks();
    TaskStatus_t *list = (TaskStatus_t *)pvPortMalloc(num * sizeof(TaskStatus_t));
    if (!list) return;
    UBaseType_t out = uxTaskGetSystemState(list, num, NULL);
    for (UBaseType_t i = 0; i < out; i++) {
        printf("task=%s min_free_stack=%u words\n", list[i].pcTaskName, (unsigned)list[i].usStackHighWaterMark);
    }
    vPortFree(list);
#else
    glog("task stack snapshot unavailable: enable CONFIG_FREERTOS_USE_TRACE_FACILITY in sdkconfig\n");
#endif
}

void handle_mem_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        ESP_LOGI(TAG, "heap(8bit) free=%u, largest=%u, min_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        heap_caps_dump(MALLOC_CAP_8BIT);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "trace") == 0) {
#if defined(CONFIG_HEAP_TRACING) || defined(CONFIG_HEAP_TRACING_STANDALONE)
        static heap_trace_record_t recs[256];
        if (argc > 2 && strcmp(argv[2], "start") == 0) {
            esp_err_t e = heap_trace_init_standalone(recs, 256);
            if (e == ESP_OK) heap_trace_start(HEAP_TRACE_ALL);
            glog("heap trace start: %s\n", e == ESP_OK ? "ok" : "err");
            return;
        }
        if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            heap_trace_stop();
            glog("heap trace stop\n");
            return;
        }
        if (argc > 2 && strcmp(argv[2], "dump") == 0) {
            heap_trace_dump();
            return;
        }
        glog("usage: mem trace <start|stop|dump>\n");
        return;
#else
        glog("heap tracing not enabled\n");
        return;
#endif
    }

    ESP_LOGI(TAG, "heap(8bit) free=%u, largest=%u, min_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    dump_task_stacks();
}

void handle_wifi_connection(int argc, char **argv) {
    const char *ssid;
    const char *password;
    if (argc == 1) {
        // No args: use saved NVS credentials
        ssid = settings_get_sta_ssid(&G_Settings);
        password = settings_get_sta_password(&G_Settings);
        if (ssid == NULL || strlen(ssid) == 0) {
            glog("No saved SSID. Usage: %s \"<SSID>\" [\"<PASSWORD>\"]\n", argv[0]);
            return;
        }
        glog("Connecting using saved credentials: %s\n", ssid);
    } else {
        char ssid_buffer[128] = {0};
        char password_buffer[128] = {0};
        int i = 1;
        // SSID parsing
        if (argv[1][0] == '"') {
            char *dest = ssid_buffer;
            bool found_end = false;
            strncpy(dest, &argv[1][1], sizeof(ssid_buffer) - 1);
            dest += strlen(&argv[1][1]);
            if (argv[1][strlen(argv[1]) - 1] == '"') {
                ssid_buffer[strlen(ssid_buffer) - 1] = '\0';
                found_end = true;
            }
            i = 2;
            while (!found_end && i < argc) {
                *dest++ = ' ';
                if (strchr(argv[i], '"')) {
                    size_t len = strchr(argv[i], '"') - argv[i];
                    strncpy(dest, argv[i], len);
                    dest[len] = '\0';
                    found_end = true;
                } else {
                    strncpy(dest, argv[i], sizeof(ssid_buffer) - (dest - ssid_buffer) - 1);
                    dest += strlen(argv[i]);
                }
                i++;
            }
            if (!found_end) {
                glog("Error: Missing closing quote for SSID\n");
                return;
            }
            ssid = ssid_buffer;
        } else {
            ssid = argv[1];
            i = 2;
        }
        // Password parsing
        if (i < argc) {
            if (argv[i][0] == '"') {
                char *dest = password_buffer;
                bool found_end = false;
                strncpy(dest, &argv[i][1], sizeof(password_buffer) - 1);
                dest += strlen(&argv[i][1]);
                if (argv[i][strlen(argv[i]) - 1] == '"') {
                    password_buffer[strlen(password_buffer) - 1] = '\0';
                    found_end = true;
                }
                i++;
                while (!found_end && i < argc) {
                    *dest++ = ' ';
                    if (strchr(argv[i], '"')) {
                        size_t len = strchr(argv[i], '"') - argv[i];
                        strncpy(dest, argv[i], len);
                        dest[len] = '\0';
                        found_end = true;
                    } else {
                        strncpy(dest, argv[i], sizeof(password_buffer) - (dest - password_buffer) - 1);
                        dest += strlen(argv[i]);
                    }
                    i++;
                }
                if (!found_end) {
                    glog("Error: Missing closing quote for password\n");
                    return;
                }
                password = password_buffer;
            } else {
                password = argv[i];
            }
        } else {
            password = "";
        }
        // Save provided credentials to NVS
        settings_set_sta_ssid(&G_Settings, ssid);
        settings_set_sta_password(&G_Settings, password);
        settings_save(&G_Settings);
    }
    wifi_manager_set_manual_disconnect(false);
    wifi_manager_connect_wifi(ssid, password);

    if (VisualizerHandle == NULL) {
#ifdef WITH_SCREEN
        xTaskCreate(screen_music_visualizer_task, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#else
        xTaskCreate(animate_led_based_on_amplitude, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#endif
    }

#ifdef CONFIG_HAS_RTC_CLOCK
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
#endif
}

void handle_wifi_disconnect(int argc, char **argv)
{
    wifi_manager_set_manual_disconnect(true);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        glog("WiFi disconnect command sent successfully\n");
    } else {
        glog("Failed to send disconnect command: %s\n", esp_err_to_name(err));
    }

    // kill any lingering visualizer task started on connect
    if (VisualizerHandle != NULL) {
        vTaskDelete(VisualizerHandle);
        VisualizerHandle = NULL;
    }
}

#ifndef CONFIG_IDF_TARGET_ESP32S2

void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        glog("Starting Find the Flippers.\n");
        ble_start_find_flippers();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-ds") == 0) {
        glog("Starting BLE Spam Detector.\n");
        ble_start_blespam_detector();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        glog("Starting AirTag Scanner.\n");
        ble_start_airtag_scanner();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        glog("Scanning for Raw Packets\n");
        ble_start_raw_ble_packetscan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping BLE Scan.\n");
        ble_stop();
        return;
    }

    glog("Invalid Command Syntax.\n");
}

#endif

void handle_start_portal(int argc, char **argv) {
    if (argc < 3 || argc > 4) { // Accept 3 or 4 arguments
        glog("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        glog("PSK is optional for an open AP.\n");
        return;
    }
    const char *url = argv[1];
    const char *ap_ssid = argv[2];
    const char *psk = (argc == 4) ? argv[3] : ""; // Set PSK to empty if not provided
    if (strlen(url) >= MAX_PORTAL_PATH_LEN) {
        glog("Error: Provided Path is too long.\n");
        return;
    }
    char final_url_or_path[MAX_PORTAL_PATH_LEN];
    strcpy(final_url_or_path, url);

    // Only prepend /mnt/ if it's not the default portal and doesn't already start with /mnt/
    if (strcmp(url, "default") != 0 && strncmp(final_url_or_path, "/mnt/ghostesp/evil_portal/portals/", 5) != 0) {
        const char *prefix = "/mnt/ghostesp/evil_portal/portals/";
        size_t prefix_len = strlen(prefix);
        size_t current_len = strlen(final_url_or_path);
        if (current_len + prefix_len >= MAX_PORTAL_PATH_LEN) {
            glog("Error: Path too long after prepending %s.\n", prefix);
            return;
        }
        memmove(final_url_or_path + prefix_len, final_url_or_path, current_len + 1);
        memcpy(final_url_or_path, prefix, prefix_len);
        glog("Prepended %s to path: %s\n", prefix, final_url_or_path);
    }
    const char *domain = settings_get_portal_domain(&G_Settings);
    glog("Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, psk, domain ? domain : "(default)");
    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), "Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, (strlen(psk) > 0 ? psk : "<Open>"), domain ? domain : "(default)");
    TERMINAL_VIEW_ADD_TEXT(log_buf);
    wifi_manager_start_evil_portal(final_url_or_path, NULL, psk, ap_ssid, domain);
}

bool ip_str_to_bytes(const char *ip_str, uint8_t *ip_bytes) {
    int ip[4];
    if (sscanf(ip_str, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (ip[i] < 0 || ip[i] > 255)
                return false;
            ip_bytes[i] = (uint8_t)ip[i];
        }
        return true;
    }
    return false;
}

bool mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes) {
    int mac[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            if (mac[i] < 0 || mac[i] > 255)
                return false;
            mac_bytes[i] = (uint8_t)mac[i];
        }
        return true;
    }
    return false;
}

void encrypt_tp_link_command(const char *input, uint8_t *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = output[i];
    }
}

void decrypt_tp_link_response(const uint8_t *input, char *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = input[i];
    }
}

void handle_tp_link_test(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: tp_link_test <on|off|loop>\n");
        status_display_show_status("TP Link Usage");
        return;
    }

    bool isloop = false;

    if (strcmp(argv[1], "loop") == 0) {
        isloop = true;
    } else if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        glog("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        status_display_show_status("TP Arg Invalid");
        return;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9999);

    int iterations = isloop ? 10 : 1;

    for (int i = 0; i < iterations; i++) {
        const char *command;
        if (isloop) {
            command = (i % 2 == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : // "on"
                          "{\"system\":{\"set_relay_state\":{\"state\":0}}}";             // "off"
        } else {

            command = (strcmp(argv[1], "on") == 0)
                          ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}"
                          : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        }

        uint8_t encrypted_command[128];
        memset(encrypted_command, 0, sizeof(encrypted_command));

        size_t command_len = strlen(command);
        if (command_len >= sizeof(encrypted_command)) {
            glog("Command too large to encrypt\n");
            status_display_show_status("TP Cmd Too Big");
            return;
        }

        encrypt_tp_link_command(command, encrypted_command, command_len);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            glog("Failed to create socket: errno %d\n", errno);
            status_display_show_status("TP Sock Error");
            return;
        }

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr,
                         sizeof(dest_addr));
        if (err < 0) {
            glog("Error occurred during sending: errno %d\n", errno);
            close(sock);
            status_display_show_status("TP Send Error");
            return;
        }

        glog("Broadcast message sent: %s\n", command);
        status_display_show_status("TP Packet Sent");

        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr,
                           &addr_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                glog("No response from any device\n");
                status_display_show_status("No TP Reply");
            } else {
                glog("Error receiving response: errno %d\n", errno);
                status_display_show_status("TP Recv Error");
            }
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            glog("Response: %s\n", decrypted_response);
            status_display_show_status("TP Reply Recv");
        }

        close(sock);

        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}

void handle_ip_lookup(int argc, char **argv) {
        glog("Starting IP lookup...\n");
    wifi_manager_start_ip_lookup();
    status_display_show_status("IP Lookup");
}

void handle_capture_scan(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        glog("Error: Incorrect number of arguments.\n");
        status_display_show_status("Capture Usage");
        return;
    }

    char *capturetype = argv[1];

    if (capturetype == NULL || capturetype[0] == '\0') {
        glog("Error: Capture Type cannot be empty.\n");
        status_display_show_status("Capture Empty");
        return;
    }

    if (strcmp(capturetype, "-probe") == 0) {
        glog("Starting probe request\npacket capture...\n");
        int err = pcap_file_open("probescan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_probe_scan_callback);
        status_display_show_status("Capture Probe");
    }

    if (strcmp(capturetype, "-deauth") == 0) {
        int err = pcap_file_open("deauthscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_deauth_scan_callback);
        status_display_show_status("Capture Deauth");
    }

    if (strcmp(capturetype, "-beacon") == 0) {
        glog("Starting beacon\npacket capture...\n");
        int err = pcap_file_open("beaconscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_beacon_scan_callback);
        status_display_show_status("Capture Beacon");
    }

    if (strcmp(capturetype, "-raw") == 0) {
        glog("Starting raw\npacket capture...\n");
        int err = pcap_file_open("rawscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
        status_display_show_status("Capture Raw");
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (strcmp(capturetype, "-802154") == 0) {
        glog("Starting IEEE 802.15.4 packet capture...\n");
        int err = pcap_file_open("802154", PCAP_CAPTURE_IEEE802154);
        if (err != ESP_OK) {
            glog("Warning: PCAP failed to open (will stream to UART)\n");
            status_display_show_status("PCAP Warn");
        }
        uint8_t ch = 0; // 0 means hopping by default
        if (argc == 3 && argv[2]) {
            const char *arg = argv[2];
            if (strncmp(arg, "ch", 2) == 0) arg += 2;
            int parsed = atoi(arg);
            if (parsed >= 11 && parsed <= 26) ch = (uint8_t)parsed; // fixed channel
        }
        zigbee_manager_start_capture(ch);
        status_display_show_status("Capture 802154");
    }
#endif

    if (strcmp(capturetype, "-eapol") == 0) {
        glog("Starting EAPOL\npacket capture...\n");
        int err = pcap_file_open("eapolscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
        status_display_show_status("Capture EAPOL");
    }

    if (strcmp(capturetype, "-pwn") == 0) {
        glog("Starting PWN\npacket capture...\n");
        int err = pcap_file_open("pwnscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_pwn_scan_callback);
        status_display_show_status("Capture PWN");
    }

    if (strcmp(capturetype, "-wps") == 0) {
        glog("Starting WPS\npacket capture...\n");
        int err = pcap_file_open("wpsscan", PCAP_CAPTURE_WIFI);

        should_store_wps = 0;

        if (err != ESP_OK) {
            glog("Error: pcap failed to open\n");
            status_display_show_status("PCAP Fail");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_wps_detection_callback);
        status_display_show_status("Capture WPS");
    }

    if (strcmp(capturetype, "-stop") == 0) {
        glog("Stopping packet capture...\n");
        wifi_manager_stop_monitor_mode();
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        zigbee_manager_stop_capture();
#endif
        pcap_file_close();
        status_display_show_status("Capture Stop");
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
        status_display_show_status("Capture BLE");
    }

    if (strcmp(capturetype, "-skimmer") == 0) {
        printf("Skimmer detection started.\n");
        TERMINAL_VIEW_ADD_TEXT("Skimmer detection started.\n");
        int err = pcap_file_open("skimmer_scan", PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            printf("Warning: PCAP capture failed to start\n");
            TERMINAL_VIEW_ADD_TEXT("Warning: PCAP capture failed to start\n");
            status_display_show_status("PCAP Warn");
        } else {
            printf("PCAP capture started\nMonitoring devices\n");
            TERMINAL_VIEW_ADD_TEXT("PCAP capture started\nMonitoring devices\n");
            status_display_show_status("Capture Skimmer");
        }
        // Start skimmer detection
        ble_start_skimmer_detection();

    }
#endif
}

void stop_portal(int argc, char **argv) {
    wifi_manager_stop_evil_portal();
    glog("Stopping evil portal...\n");
    status_display_show_status("Portal Stop");
}

void handle_reboot(int argc, char **argv) {
    glog("Rebooting system...\n");
    esp_restart();
}

void handle_startwd(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        gps_manager_deinit(&g_gpsManager);
        wifi_manager_stop_monitor_mode();
        csv_flush_buffer_to_file();
        csv_file_close();
        glog("Wardriving stopped.\n");
        status_display_show_status("Wardrive Stop");
    } else {
        gps_manager_init(&g_gpsManager);
        if (sd_card_exists("/mnt/ghostesp/gps")) {
            esp_err_t err = csv_file_open("wardriving");
            if (err != ESP_OK) {
                glog("Failed to open CSV for wardriving\n");
                status_display_show_status("CSV Open Fail");
            }
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        glog("Wardriving started.\n");
        status_display_show_status("Wardrive Start");
    }
}

void handle_timezone_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: timezone <TZ_STRING>\n");
        status_display_show_status("Timezone Usage");
        return;
    }
    const char *tz = argv[1];
    settings_set_timezone_str(&G_Settings, tz);
    settings_save(&G_Settings);
    setenv("TZ", tz, 1);
    tzset();
    glog("Timezone set to: %s\n", tz);
    status_display_show_status("Timezone Set");
}

void handle_scan_ports(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage:\n");
        glog("  scanports local\n");
        glog("  scanports <IP> [all | start-end]\n");
        status_display_show_status("Ports Usage");
        return;
    }

    // Handle local subnet scan
    if (strcmp(argv[1], "local") == 0) {
        if (argc > 2) {
            glog("Info: 'local' scan does not take arguments.\n");
            status_display_show_status("Ports Local");
        }
        glog("Starting local subnet scan...\n");
        wifi_manager_scan_subnet();
        status_display_show_status("Ports Local");
        return;
    }

    // Handle remote IP scan
    const char *target_ip = argv[1];
    int start_port = 0, end_port = 0;

    // Default to common ports if no range is specified
    if (argc < 3) {
        host_result_t result;
        glog("Scanning common tcp ports on %s...\n", target_ip);
        scan_ports_on_host(target_ip, &result);

        if (result.num_open_ports > 0) {
            glog("Found %d open ports on %s:\n", result.num_open_ports, target_ip);
            for (int i = 0; i < result.num_open_ports; i++) {
                glog("  Port %d\n", result.open_ports[i]);
            }
        } else {
            glog("No common open ports found.\n");
        }

        host_result_t udp_result;
        glog("Scanning common udp ports on %s...\n", target_ip);
        scan_udp_ports_on_host(target_ip, &udp_result);
        if (udp_result.num_open_ports > 0) {
            glog("Found %d udp ports responding on %s:\n", udp_result.num_open_ports, target_ip);
            for (int i = 0; i < udp_result.num_open_ports; i++) {
                glog("  UDP %d\n", udp_result.open_ports[i]);
            }
        } else {
            glog("No common udp responses found.\n");
        }
        status_display_show_status("Ports Common");
        return;
    }

    // Parse port range argument
    const char *port_arg = argv[2];
    if (strcmp(port_arg, "all") == 0) {
        start_port = 1;
        end_port = 65535;
    } else if (sscanf(port_arg, "%d-%d", &start_port, &end_port) != 2 || start_port < 1 ||
               end_port > 65535 || start_port > end_port) {
        glog("Error: Invalid port range. Use 'all' or 'start-end'.\n");
        status_display_show_status("Range Invalid");
        return;
    }

    glog("Scanning %s tcp ports %d-%d...\n", target_ip, start_port, end_port);
    scan_ip_port_range(target_ip, start_port, end_port);

    glog("Scanning %s udp ports %d-%d...\n", target_ip, start_port, end_port);
    scan_ip_udp_port_range(target_ip, start_port, end_port);
    status_display_show_status("Ports Custom");
}

void handle_scan_arp(int argc, char **argv) {
    glog("Starting ARP scan on local network...\n");
    wifi_manager_arp_scan_subnet();
    status_display_show_status("ARP Scan");
}

void handle_scan_ssh(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: scanssh <IP>\n");
        status_display_show_status("SSH Usage");
        return;
    }

    const char *target_ip = argv[1];
    host_result_t result;
    char msg_buf[64];
    
    glog("Starting SSH scan on %s...\n", target_ip);
    
    scan_ssh_on_host(target_ip, &result);
    
    if (result.num_open_ports > 0) {
        glog("Found %d SSH service(s) on %s\n", result.num_open_ports, target_ip);
        status_display_show_status("SSH Found");
    } else {
        glog("No SSH services found.\n");
        status_display_show_status("SSH None");
    }
}

void handle_crash(int argc, char **argv) {
    int *ptr = NULL;
    *ptr = 42;
}


// Help command
void handle_help(int argc, char **argv) {
    const char *category = (argc > 1) ? argv[1] : "unknown"; // Default to "unknown" if no category is provided to fall through ifs

    // List of all categories to print in order
    const char *all_categories[] = {
        "wifi", "ble", "comm", "sd", "led", "gps", "misc", "portal", "printer", "cast", "capture", "beacon", "attack"
    };
    int num_categories = sizeof(all_categories) / sizeof(all_categories[0]);

    if (strcmp(category, "all") == 0) {
        for (int i = 0; i < num_categories; ++i) {
            // Recursively call this function for each category
            char *fake_argv[] = { "help", (char *)all_categories[i] };
            handle_help(2, fake_argv);
        }
        return;
    }


    if (strcmp(category, "wifi") == 0) {
        glog("\nWi-Fi Commands:\n\n");
        printf("scanap\n");
        printf("    Description: Start a Wi-Fi access point (AP) scan.\n");
        printf("    Usage: scanap [seconds]\n\n");
        printf("scansta\n");
        printf("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
        printf("    Usage: scansta\n\n");
        printf("stopscan\n");
        printf("    Description: Stop any ongoing Wi-Fi scan.\n");
        printf("    Usage: stopscan\n\n");
        printf("attack\n");
        printf("    Description: Launch an attack (e.g., deauthentication attack).\n");
        printf("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
        printf("    Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
        printf("    Arguments:\n");
        printf("        -d  : Start deauth attack (supports multiple APs)\n");
        printf("        -e  : Start EAPOL logoff attack\n");
        printf("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n\n");
        printf("list\n");
        printf("    Description: List Wi-Fi scan results or connected stations.\n");
        printf("    Usage: list -a | list -s | list -airtags\n");
        printf("    Arguments:\n");
        printf("        -a  : Show access points from Wi-Fi scan\n");
        printf("        -s  : List connected stations\n");
        printf("        -airtags: List discovered AirTags\n\n");
        printf("beaconspam\n");
        printf("    Description: Start beacon spam with different modes.\n");
        printf("    Usage: beaconspam [OPTION]\n");
        printf("    Arguments:\n");
        printf("        -r   : Start random beacon spam\n");
        printf("        -rr  : Start Rickroll beacon spam\n");
        printf("        -l   : Start AP List beacon spam\n");
        printf("        [SSID]: Use specified SSID for beacon spam\n\n");
        printf("stopspam\n");
        printf("    Description: Stop ongoing beacon spam.\n");
        printf("    Usage: stopspam\n\n");
        printf("stopdeauth\n");
        printf("    Description: Stop ongoing deauthentication attack.\n");
        printf("    Usage: stopdeauth\n\n");
        printf("select\n");
        printf("    Description: Select access point(s), station, or AirTag by index from the scan results.\n");
        printf("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
        printf("    Arguments:\n");
        printf("        -a      : AP selection index (supports multiple: 1,3,5)\n");
        printf("        -s      : Station selection index\n");
        printf("        -airtag : AirTag selection index\n");
        printf("    Examples:\n");
        printf("        select -a 4      : Select single AP at index 4\n");
        printf("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
        printf("scanall\n");
        printf("    Description: Perform combined AP and Station scan, display results.\n");
        printf("    Usage: scanall [seconds]\n\n");
        printf("congestion\n");
        printf("    Description: Display Wi-Fi channel congestion chart.\n");
        printf("    Usage: congestion\n\n");
        printf("connect\n");
        printf("    Description: Connects to Specific WiFi Network and saves credentials.\n");
        printf("    Usage: connect <SSID> [Password]\n\n");
        printf("apcred\n");
        printf("    Description: Change or reset the GhostNet AP credentials\n");
        printf("    Usage: apcred <ssid> <password>\n");
        printf("           apcred -r (reset to defaults)\n");
        printf("    Arguments:\n");
        printf("        <ssid>     : New SSID for the AP\n");
        printf("        <password> : New password (min 8 characters)\n");
        printf("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
        printf("apenable\n");
        printf("    Description: Enable or disable the Access Point across reboots\n");
        printf("    Usage: apenable <on|off>\n");
        printf("    Arguments:\n");
        printf("        on  : Enable the Access Point (requires restart)\n");
        printf("        off : Disable the Access Point (requires restart)\n\n");
        printf("listenprobes\n");
        printf("    Description: Listen for and log probe requests.\n");
        printf("    Usage: listenprobes [channel] [stop]\n");
        printf("    Arguments:\n");
        printf("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
        printf("        stop      : Stop probe request listening\n\n");
        printf("karma\n");
        printf("    Description: Start or stop the Karma attack (responds to probe requests with specified or all SSIDs).\n");
        printf("    Usage: karma start [ssid1 ssid2 ...]\n");
        printf("           karma stop\n");
        printf("    Arguments:\n");
        printf("        start : Begin Karma attack. Optionally specify SSIDs to respond with (default: all known SSIDs).\n");
        printf("        stop  : Stop Karma attack.\n");
        printf("    Examples:\n");
        printf("        karma start\n");
        printf("        karma start FreeWiFi Starbucks\n");
        printf("        karma stop\n\n");
#if CONFIG_IDF_TARGET_ESP32C5
        printf("setcountry\n");
        printf("    Description: Set the Wi-Fi country code.\n");
        printf("    Usage: setcountry <CC>\n");
        printf("    Arguments:\n");
        printf("        <CC> : Country code (\"01\" world-safe) or two-letter ISO (e.g., US)\n");
        printf("    Supported: 01, AT, AU, BE, BG, BR, CA, CH, CN, CY, CZ, DE, DK, EE, ES, FI, FR, GB, GR, HK, HR, HU,\n");
        printf("               IE, IN, IS, IT, JP, KR, LI, LT, LU, LV, MT, MX, NL, NO, NZ, PL, PT, RO, SE, SI, SK, TW, US\n\n");
#endif
        TERMINAL_VIEW_ADD_TEXT("scanap, scansta, stopscan, attack, list, beaconspam, stopspam, stopdeauth, select, scanall, congestion, connect, apcred, apenable, listenprobes");
#if CONFIG_IDF_TARGET_ESP32C5
        TERMINAL_VIEW_ADD_TEXT(", setcountry");
#endif
        TERMINAL_VIEW_ADD_TEXT(", karma\n");
        return;
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(category, "ble") == 0) {
        glog("\nBLE Commands:\n\n");
        printf("blescan\n");
        printf("    Description: Handle BLE scanning with various modes.\n");
        printf("    Usage: blescan [OPTION]\n");
        printf("    Arguments:\n");
        printf("        -f   : Start 'Find the Flippers' mode\n");
        printf("        -ds  : Start BLE spam detector\n");
        printf("        -a   : Start AirTag scanner\n");
        printf("        -r   : Scan for raw BLE packets\n");
        printf("        -s   : Stop BLE scanning\n\n");
        printf("blespam\n");
        printf("    Description: Start BLE advertisement spam attacks.\n");
        printf("    Usage: blespam [OPTION]\n");
        printf("    Arguments:\n");
        printf("        -apple     : Apple device spam (AirPods, Apple TV, etc.)\n");
        printf("        -ms        : Microsoft Swift Pair spam\n");
        printf("        -samsung   : Samsung Galaxy Watch spam\n");
        printf("        -google    : Google Fast Pair spam\n");
        printf("        -random    : Random spam (cycles through all types)\n");
        printf("        -s         : Stop BLE spam\n\n");
        printf("blewardriving\n");
        printf("    Description: Start/Stop BLE wardriving with GPS logging\n");
        printf("    Usage: blewardriving [-s]\n");
        printf("    Arguments:\n");
        printf("        -s  : Stop BLE wardriving\n\n");
        printf("list -airtags\n");
        printf("    Description: List discovered AirTags\n");
        printf("    Usage: list -airtags\n\n");
        printf("select -airtag <index>\n\n");
        printf("blescan\n");
        printf("    Description: Start Bluetooth Low Energy (BLE) scan.\n");
        printf("    Usage: blescan [seconds]\n\n");
        TERMINAL_VIEW_ADD_TEXT("blescan, blespam, blewardriving, list -airtags, select -airtag\n");
        return;
    }
#endif

    if (strcmp(category, "comm") == 0) {
        glog("\nCommunication Commands:\n\n");
        printf("commdiscovery\n    Check discovery status.\n    Usage: commdiscovery\n\n");
        printf("commconnect\n    Connect to a discovered peer ESP32.\n    Usage: commconnect <peer_name>\n    Example: commconnect ESP_A1B2C3\n\n");
        printf("commsend\n    Send a command to connected peer ESP32.\n    Usage: commsend <command> [data]\n    Example: commsend scanap\n    Example: commsend hello world\n\n");
        printf("commstatus\n    Show communication status.\n    Usage: commstatus\n\n");
        printf("commdisconnect\n    Disconnect from current peer.\n    Usage: commdisconnect\n\n");
        printf("commsetpins\n    Change communication GPIO pins at runtime.\n    Usage: commsetpins <tx_pin> <rx_pin>\n    Example: commsetpins 4 5\n\n");
        TERMINAL_VIEW_ADD_TEXT("commdiscovery, commconnect, commsend, commstatus, commdisconnect, commsetpins\n");
        return;
    }

    if (strcmp(category, "sd") == 0) {
        glog("\nSD Card Commands:\n\n");
        printf("-- SD Card Pin Configuration --\n");
        printf("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
        printf("These commands configure pins for the *active* mode.\n");
        printf("Changing the mode requires recompiling firmware.\n");
        TERMINAL_VIEW_ADD_TEXT("-- SD Card Pin Configuration --\n");
        TERMINAL_VIEW_ADD_TEXT("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
        TERMINAL_VIEW_ADD_TEXT("These commands configure pins for the *active* mode.\n");
        TERMINAL_VIEW_ADD_TEXT("Changing the mode requires recompiling firmware.\n");
        printf("sd_config\n    Show current SD GPIO pin configuration.\n    Usage: sd_config\n\n");
        printf("sd_pins_mmc\n    Description: Set GPIO pins for SDMMC mode (1 or 4 bit). Requires restart/reinit.\n                 Only effective if firmware compiled for SDMMC mode.\n    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n    Example: sd_pins_mmc 19 18 20 21 22 23\n\n");
        printf("sd_pins_spi\n    Description: Set GPIO pins for SPI mode. Requires restart/reinit.\n                 Only effective if firmware compiled for SPI mode.\n    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n    Example: sd_pins_spi 5 18 19 23\n\n");
        printf("sd_save_config\n    Description: Save the current SD pin configuration (both modes) to the SD card.\n                 Requires SD card to be mounted.\n    Usage: sd_save_config\n\n");
        TERMINAL_VIEW_ADD_TEXT("sd_config, sd_pins_mmc, sd_pins_spi, sd_save_config\n");
        return;
    }

    if (strcmp(category, "led") == 0) {
        glog("\nLED & RGB Commands:\n\n");
        printf("rgbmode\n    Control LED effects (rainbow, police, strobe, off)\n    Usage: rgbmode <rainbow|police|strobe|off|color>\n\n");
        printf("setrgbpins\n    Change RGB LED pins\n    Usage: setrgbpins <red> <green> <blue>\n           (use same value for all pins for single-pin LED strips)\n\n");
        printf("setneopixelbrightness\n    Set maximum neopixel brightness (percent)\n    Usage: setneopixelbrightness <0-100>\n\n");
        printf("getneopixelbrightness\n    Show current neopixel max brightness (percent)\n    Usage: getneopixelbrightness\n\n");
        TERMINAL_VIEW_ADD_TEXT("rgbmode, setrgbpins, setneopixelbrightness, getneopixelbrightness\n");
        return;
    }

    if (strcmp(category, "misc") == 0) {
        glog("\nMiscellaneous Commands:\n\n");
        printf("help\n");
        printf("    Description: Display this help message.\n");
        printf("    Usage: help [category]\n\n");
        printf("chipinfo\n");
        printf("    Description: Display chip information including model, revision, and features\n");
        printf("    Usage: chipinfo\n");
        printf("    Shows:\n");
        printf("        - Chip model and revision\n");
        printf("        - CPU cores and features\n");
        printf("        - Flash size and memory info\n");
        printf("        - ESP-IDF version\n\n");
        printf("timezone\n");
        printf("    Description: Set the display timezone for the clock view.\n");
        printf("    Usage: timezone <TZ_STRING>\n\n");
        printf("webauth\n");
        printf("    Description: Enable/disable web authentication.\n");
        printf("    Usage: webauth <enable|disable>\n\n");
        printf("pineap\n");
        printf("    Description: Start/Stop detecting WiFi Pineapples.\n");
        printf("    Usage: pineap [-s]\n");
        printf("    Arguments:\n");
        printf("        -s  : Stop PineAP detection\n\n");
        printf("Port Scanner\n");
        printf("    Description: Scan ports on local subnet or specific IP\n");
        printf("    Usage: scanports local\n");
        printf("           scanports <IP> [all | start-end]\n");
        printf("    Arguments:\n");
        printf("        all  : Scan all ports (1-65535)\n");
        printf("        start-end : Custom port range (e.g. 80-443)\n");
        printf("        (no range) : Scan common ports (default)\n\n");
        printf("scanarp\n");
        printf("    Description: Perform ARP scan on local network to discover active hosts\n");
        printf("    Usage: scanarp\n\n");
        printf("settings\n");
        printf("    Description: Manage NVS stored settings via command line\n");
        printf("    Usage: settings <command> [arguments]\n");
        printf("    Commands:\n");
        printf("        list                    - List all available settings\n");
        printf("        get <setting>           - Get current value of a setting\n");
        printf("        set <setting> <value>   - Set a setting to a value\n");
        printf("        reset [setting]         - Reset setting(s) to defaults\n");
        printf("        help                    - Show settings help\n");
        printf("    Examples:\n");
        printf("        settings list\n");
        printf("        settings get ap_ssid\n");
        printf("        settings set rgb_mode 1\n");
        printf("        settings reset\n\n");
        TERMINAL_VIEW_ADD_TEXT("help, chipinfo, timezone, webauth, pineap, scanports, scanarp, settings\n");
        return;
    }
    if (strcmp(category, "gps") == 0) {
        glog("\nGPS Commands:\n\n");
        printf("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo\n\n");
        printf("startwd\n    Start GPS wardriving.\n    Usage: startwd [seconds]\n\n");
        TERMINAL_VIEW_ADD_TEXT("gpsinfo, startwd\n");
        return;
    }
    if (strcmp(category, "portal") == 0) {
        glog("\nEvil Portal Commands:\n\n");
        printf("startportal\n");
        printf("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
        printf("                 /mnt/ prefix is added automatically to file paths if missing.\n");
        printf("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
        printf("           PSK is optional for an open network.\n");
        printf("    Use 'default' as the file path for the default Evil Portal.\n");
        printf("\n");
        printf("evilportal\n");
        printf("    Description: Configure Evil Portal HTML content via UART buffer.\n");
        printf("    Usage: evilportal -c sethtmlstr\n");
        printf("    Steps:\n");
        printf("      1. Run: evilportal -c sethtmlstr\n");
        printf("      2. Send [HTML/BEGIN] marker over UART\n");
        printf("      3. Send HTML content over UART\n");
        printf("      4. Send [HTML/CLOSE] marker over UART\n");
        printf("      5. Run startportal (will use buffered HTML)\n");
        printf("\n");
        printf("stopportal\n");
        printf("    Description: Stop Evil Portal\n");
        printf("    Usage: stopportal\n\n");
        printf("listportals\n    List available Evil Portal files.\n    Usage: listportals\n\n");
        TERMINAL_VIEW_ADD_TEXT("startportal, stopportal, listportals\n");
        return;
    }

    if (strcmp(category, "printer") == 0) {
        glog("\nPrinter Commands:\n\n");
        printf("powerprinter\n");
        printf("    Description: Print Custom Text to a Printer on your LAN (Requires You to Run Connect First)\n");
        printf("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
        printf("    aligment options: CM = Center Middle, TL = Top Left, TR = Top Right, BR = Bottom Right, BL = Bottom Left\n\n");
        TERMINAL_VIEW_ADD_TEXT("powerprinter\n");
        TERMINAL_VIEW_ADD_TEXT("    Print custom text to a network printer.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n\n");
        return;
    }

    if (strcmp(category, "cast") == 0) {
        glog("\nYouTube Cast Commands:\n\n");
        printf("dialconnect\n");
        printf("    Description: Cast a Random Youtube Video on all Smart TV's on your LAN (Requires You to Run Connect First)\n");
        printf("    Usage: dialconnect\n\n");
        TERMINAL_VIEW_ADD_TEXT("dialconnect\n");
        TERMINAL_VIEW_ADD_TEXT("    Cast a random YouTube video to all smart TVs on your LAN.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: dialconnect\n\n");
        return;
    }

    if (strcmp(category, "capture") == 0) {
        glog("\nCapture Commands:\n\n");
        printf("capture\n");
        printf("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
        printf("    Usage: capture [OPTION]\n");
        printf("    Arguments:\n");
        printf("        -probe   : Start Capturing Probe Packets\n");
        printf("        -beacon  : Start Capturing Beacon Packets\n");
        printf("        -deauth   : Start Capturing Deauth Packets\n");
        printf("        -raw   :   Start Capturing Raw Packets\n");
        printf("        -wps   :   Start Capturing WPS Packets and there Auth Type\n");
        printf("        -pwn   :   Start Capturing Pwnagotchi Packets\n");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        printf("        -802154:   Start Capturing IEEE 802.15.4 Packets [C5/C6]\n");
        #endif
        printf("        -stop   : Stops the active capture\n\n");
        TERMINAL_VIEW_ADD_TEXT("capture\n");
        TERMINAL_VIEW_ADD_TEXT("    Start a WiFi packet capture.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: capture [OPTION]\n");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        TERMINAL_VIEW_ADD_TEXT("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -802154, -stop\n\n");
        #else
        TERMINAL_VIEW_ADD_TEXT("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -stop\n\n");
        #endif
        return;
    }

    if (strcmp(category, "beacon") == 0) {
        glog("\nBeacon Spam Commands:\n\n");
        printf("beaconadd\n    Add an SSID to the beacon spam list.\n    Usage: beaconadd <SSID>\n\n");
        printf("beaconremove\n    Remove an SSID from the beacon spam list.\n    Usage: beaconremove <SSID>\n\n");
        printf("beaconclear\n    Clear the beacon spam list.\n    Usage: beaconclear\n\n");
        printf("beaconshow\n    Show the current beacon spam list.\n    Usage: beaconshow\n\n");
        printf("beaconspamlist\n    Start beacon spamming using the beacon spam list.\n    Usage: beaconspamlist\n\n");
        TERMINAL_VIEW_ADD_TEXT("beaconadd, beaconremove, beaconclear, beaconshow, beaconspamlist\n");
        return;
    }

    if (strcmp(category, "attack") == 0) {
        glog("\nAttack Commands:\n\n");
        printf("dhcpstarve\n");
        printf("    Description: DHCP starvation flood attack\n");
        printf("    Usage: dhcpstarve start [threads]\n");
        printf("           dhcpstarve stop\n");
        printf("           dhcpstarve display\n\n");
        printf("saeflood\n");
        printf("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
        printf("    Usage: saeflood <password> (requires selected WPA3 AP)\n\n");
        printf("stopsaeflood\n    Stop SAE flood attack.\n    Usage: stopsaeflood\n\n");
        printf("saefloodhelp\n    Show detailed SAE flood attack help.\n    Usage: saefloodhelp\n\n");
        TERMINAL_VIEW_ADD_TEXT("dhcpstarve, saeflood, stopsaeflood, saefloodhelp\n");
        return;
    }
    
    glog("\nGhost ESP Command Categories:\n\n");

    printf("  help wifi      - Wi-Fi commands\n");
    printf("  help ble       - Bluetooth/BLE commands\n");
    printf("  help comm      - ESP32 communication commands\n");
    printf("  help sd        - SD card commands\n");
    printf("  help led       - LED/RGB commands\n");
    printf("  help gps       - GPS commands\n");
    printf("  help misc      - Miscellaneous commands\n");
    printf("  help portal    - Evil Portal commands\n");
    printf("  help printer   - Printer commands\n");
    printf("  help cast      - YouTube cast commands\n");
    printf("  help capture   - Wi-Fi packet capture commands\n");
    printf("  help beacon    - Beacon spam commands\n");
    printf("  help attack    - Attack/flood commands\n");
    printf("  help all      - All commands\n\n");

    TERMINAL_VIEW_ADD_TEXT(
        "  help wifi      - Wi-Fi commands\n"
        "  help ble       - Bluetooth/BLE commands\n"
        "  help comm      - ESP32 communication commands\n"
        "  help sd        - SD card commands\n"
        "  help led       - LED/RGB commands\n"
        "  help gps       - GPS commands\n"
        "  help misc      - Miscellaneous commands\n");
    TERMINAL_VIEW_ADD_TEXT("  help portal    - Evil Portal commands\n"
                      "  help printer   - Printer commands\n"
                      "  help cast      - YouTube cast commands\n"
                      "  help capture   - Wi-Fi packet capture commands\n"
                      "  help beacon    - Beacon spam commands\n"
                      "  help attack    - Attack/flood commands\n"
                      "  help all      - All commands\n\n");

    printf("Type 'help <category>' for details on that category.\n\n");
    TERMINAL_VIEW_ADD_TEXT("Type 'help <category>' for details on that category.\n\n");
}

void handle_capture(int argc, char **argv) {
    if (argc < 2) {
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("Usage: capture [-probe|-beacon|-deauth|-raw|-ble|-zigbee]\n");
        #else
        glog("Usage: capture [-probe|-beacon|-deauth|-raw|-ble]\n");
        #endif
        status_display_show_status("Capture Usage");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(argv[1], "-ble") == 0) {
        glog("Starting BLE packet capture...\n");
        ble_start_capture();
        status_display_show_status("Capture BLE");
    }
#endif
}

void handle_gps_info(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        if (gps_info_task_handle != NULL) {
            vTaskDelete(gps_info_task_handle);
            gps_info_task_handle = NULL;
            gps_manager_deinit(&g_gpsManager);
            printf("GPS info display stopped.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display stopped.\n");
            status_display_show_status("GPS Info Off");
        }
    } else {
        if (gps_info_task_handle == NULL) {
            gps_manager_init(&g_gpsManager);

            // Wait a brief moment for GPS initialization
            vTaskDelay(pdMS_TO_TICKS(100));

            // Start the info display task
            xTaskCreate(gps_info_display_task, "gps_info", 4096, NULL, 1, &gps_info_task_handle);
            printf("GPS info started.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info started.\n");
            status_display_show_status("GPS Info On");
        }
    }
}


#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_wardriving(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        ble_stop();
        gps_manager_deinit(&g_gpsManager);
        if (buffer_offset > 0) { // Only flush if there's data in buffer
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
        status_display_show_status("BLE Drive Off");
    } else {
        if (!g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            status_display_show_status("CSV Open Fail");
            return;
        }

        ble_register_handler(ble_wardriving_callback);
        ble_start_scanning();
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
        status_display_show_status("BLE Drive On");
    }
}
#endif

void handle_pineap_detection(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        status_display_show_status("PineAP Stop");
        return;
    }
    // Open PCAP file for logging detections
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        glog("Warning: Failed to open PCAP file for logging\n");
        status_display_show_status("PCAP Warn");
    }

    // Start PineAP detection with channel hopping
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);

    glog("Monitoring for Pineapples\n");
    status_display_show_status("PineAP Watch");
}


void handle_apcred(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: apcred <ssid> <password>\n");
        glog("       apcred -r (reset to defaults)\n");
        status_display_show_status("APCred Usage");
        return;
    }
                
    // Check for reset flag
    if (argc == 2 && strcmp(argv[1], "-r") == 0) {
        // Set empty strings to trigger default values
        settings_set_ap_ssid(&G_Settings, "");
        settings_set_ap_password(&G_Settings, "");
        settings_save(&G_Settings);
        ap_manager_stop_services();
        esp_err_t err = ap_manager_start_services();
        if (err != ESP_OK) {
            printf("Error resetting AP: %s\n", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("Error resetting AP:\n%s\n", esp_err_to_name(err));
            status_display_show_status("AP Reset Fail");
            return;
        }

        printf("AP credentials reset to defaults (SSID: GhostNet, Password: GhostNet)\n");
        TERMINAL_VIEW_ADD_TEXT("AP reset to defaults:\nSSID: GhostNet\nPSK: GhostNet\n");
        status_display_show_status("AP Reset");
        return;
    }

    if (argc != 3) {
        glog("Error: Incorrect number of arguments.\n");
        status_display_show_status("APCred Args");
        return;
    }

    const char *new_ssid = argv[1];
    const char *new_password = argv[2];

    if (strlen(new_password) < 8) {
        glog("Error: Password must be at least 8 characters\n");
        status_display_show_status("Password Weak");
        return;
    }

    // immediate AP reconfiguration
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(new_ssid),
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    strcpy((char *)ap_config.ap.ssid, new_ssid);
    strcpy((char *)ap_config.ap.password, new_password);
    
    // Force the new config immediately
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    settings_set_ap_ssid(&G_Settings, new_ssid);
    settings_set_ap_password(&G_Settings, new_password);
    settings_save(&G_Settings);

    const char *saved_ssid = settings_get_ap_ssid(&G_Settings);
    const char *saved_password = settings_get_ap_password(&G_Settings);
    if (strcmp(saved_ssid, new_ssid) != 0 || strcmp(saved_password, new_password) != 0) {
        glog("Error: Failed to save AP credentials\n");
        status_display_show_status("Save Failed");
        return;
    }

    ap_manager_stop_services();
    esp_err_t err = ap_manager_start_services();
    if (err != ESP_OK) {
        glog("Error restarting AP: %s\n", esp_err_to_name(err));
        status_display_show_status("AP Restart NG");
        return;
    }

    glog("AP credentials updated - SSID: %s, Password: %s\n", saved_ssid, saved_password);
    status_display_show_status("AP Updated");
}

void handle_rgb_mode(int argc, char **argv) {
    static bool last_effect_is_rainbow = false;
    if (argc < 2) {
        glog("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        status_display_show_status("RGB Usage");
        return;
    }

    // Cancel any currently running LED effect task safely.
    if (rgb_effect_task_handle != NULL) {
        if (last_effect_is_rainbow) {
            rgb_manager_signal_rainbow_exit();
            vTaskDelay(pdMS_TO_TICKS(50));
            rgb_effect_task_handle = NULL;
        } else {
            vTaskDelete(rgb_effect_task_handle);
            rgb_effect_task_handle = NULL;
        }
    }

    // Check for built-in modes first.
    if (strcasecmp(argv[1], "rainbow") == 0) {
        if (!(rgb_manager.is_separate_pins || rgb_manager.strip)) {
            glog("RGB not initialized\n");
            status_display_show_status("RGB Not Ready");
            return;
        }
        xTaskCreate(rainbow_task, "rainbow_effect", 2048, &rgb_manager, 5, &rgb_effect_task_handle);
        last_effect_is_rainbow = true;
        glog("Rainbow mode activated\n");
        status_display_show_status("RGB Rainbow");
    } else if (strcasecmp(argv[1], "police") == 0) {
        if (!(rgb_manager.is_separate_pins || rgb_manager.strip)) {
            glog("RGB not initialized\n");
            status_display_show_status("RGB Not Ready");
            return;
        }
        xTaskCreate(police_task, "police_effect", 2048, &rgb_manager, 5, &rgb_effect_task_handle);
        last_effect_is_rainbow = false;
        glog("Police mode activated\n");
        status_display_show_status("RGB Police");
    } else if (strcasecmp(argv[1], "strobe") == 0) {
        glog("SEIZURE WARNING\nPLEASE EXIT NOW IF\nYOU ARE SENSITIVE\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!(rgb_manager.is_separate_pins || rgb_manager.strip)) {
            glog("RGB not initialized\n");
            status_display_show_status("RGB Not Ready");
            return;
        }
        xTaskCreate(strobe_task, "strobe_effect", 2048, &rgb_manager, 5, &rgb_effect_task_handle);
        last_effect_is_rainbow = false;
        glog("Strobe mode activated\n");
        status_display_show_status("RGB Strobe");
    } else if (strcasecmp(argv[1], "off") == 0) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        if (!rgb_manager.is_separate_pins && rgb_manager.strip) {
            led_strip_clear(rgb_manager.strip);
            led_strip_refresh(rgb_manager.strip);
        }
        glog("RGB disabled\n");
        status_display_show_status("RGB Off");
        if (rgb_effect_task_handle != NULL) {
            vTaskDelete(rgb_effect_task_handle);
            rgb_effect_task_handle = NULL;
        }
    } else {
        // Otherwise, treat the argument as a color name.
        typedef struct {
            const char *name;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } color_t;
        static const color_t supported_colors[] = {
            { "red",    255, 0,   0 },
            { "green",  0,   255, 0 },
            { "blue",   0,   0,   255 },
            { "yellow", 255, 255, 0 },
            { "purple", 128, 0,   128 },
            { "cyan",   0,   255, 255 },
            { "orange", 255, 165, 0 },
            { "white",  255, 255, 255 },
            { "pink",   255, 192, 203 }
        };
        const int num_colors = sizeof(supported_colors) / sizeof(supported_colors[0]);
        int found = 0;
        uint8_t r, g, b;
        for (int i = 0; i < num_colors; i++) {
            // Use case-insensitive compare.
            if (strcasecmp(argv[1], supported_colors[i].name) == 0) {
                r = supported_colors[i].r;
                g = supported_colors[i].g;
                b = supported_colors[i].b;
                found = 1;
                break;
            }
        }
        if (!found) {
            glog("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            status_display_show_status("Color Invalid");
            return;
        }
        // Set each LED to the selected static color.
        for (int i = 0; i < rgb_manager.num_leds; i++) {
            rgb_manager_set_color(&rgb_manager, i, r, g, b, false);
        }
        led_strip_refresh(rgb_manager.strip);
        glog("Static color mode activated: %s\n", argv[1]);
        status_display_show_status("RGB Static");
    }
}

void handle_setrgb(int argc, char **argv) {
    if (argc != 4) {
        glog("Usage: setrgbpins <red> <green> <blue>\n");
        glog("           (use same value for all pins for single-pin LED strips)\n\n");
        status_display_show_status("SetRGB Usage");
        return;
    }
    gpio_num_t red_pin = (gpio_num_t)atoi(argv[1]);
    gpio_num_t green_pin = (gpio_num_t)atoi(argv[2]);
    gpio_num_t blue_pin = (gpio_num_t)atoi(argv[3]);

    esp_err_t ret;
    if (red_pin == green_pin && green_pin == blue_pin) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, red_pin, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                                GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, red_pin);
            settings_set_rgb_separate_pins(&G_Settings, -1, -1, -1);
            settings_save(&G_Settings);
            glog("Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            status_display_show_status("RGB Single");
        }
    } else {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               red_pin, green_pin, blue_pin);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, -1);
            settings_set_rgb_separate_pins(&G_Settings, red_pin, green_pin, blue_pin);
            settings_save(&G_Settings);
            glog("RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            status_display_show_status("RGB Pins Set");
        }
    }
}

void handle_sd_config(int argc, char **argv) {
  sd_card_print_config();
  status_display_show_status("SD Config");
}

void handle_sd_pins_mmc(int argc, char **argv) {
  if (argc != 7) {
    glog("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    glog("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
    glog("Example: sd_pins_mmc 19 18 20 21 22 23\n");
    status_display_show_status("SD MMC Usage");
    return;
  }
  
  int clk = atoi(argv[1]);
  int cmd = atoi(argv[2]);
  int d0 = atoi(argv[3]);
  int d1 = atoi(argv[4]);
  int d2 = atoi(argv[5]);
  int d3 = atoi(argv[6]);
  
  if (clk < 0 || cmd < 0 || d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 ||
      clk > 40 || cmd > 40 || d0 > 40 || d1 > 40 || d2 > 40 || d3 > 40) {
    glog("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    status_display_show_status("Pins Invalid");
    return;
  }
  
  sd_card_set_mmc_pins(clk, cmd, d0, d1, d2, d3);
  status_display_show_status("SD MMC Set");
}

void handle_sd_pins_spi(int argc, char **argv) {
  if (argc != 5) {
    glog("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    glog("Sets pins for SPI mode (only effective if compiled for SPI).\n");
    glog("Example: sd_pins_spi 5 18 19 23\n");
    status_display_show_status("SD SPI Usage");
    return;
  }
  
  int cs = atoi(argv[1]);
  int clk = atoi(argv[2]);
  int miso = atoi(argv[3]);
  int mosi = atoi(argv[4]);
  
  if (cs < 0 || clk < 0 || miso < 0 || mosi < 0 ||
      cs > 40 || clk > 40 || miso > 40 || mosi > 40) {
    glog("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    status_display_show_status("Pins Invalid");
    return;
  }
  
  sd_card_set_spi_pins(cs, clk, miso, mosi);
  status_display_show_status("SD SPI Set");
}

void handle_sd_save_config(int argc, char **argv) {
  sd_card_save_config();
  status_display_show_status("SD Saved");
}

void handle_congestion_cmd(int argc, char **argv) {
    wifi_manager_start_scan();
    status_display_show_status("Congest Scan");

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_manager_get_scan_results_data(&ap_count, &ap_records);

    if (ap_count == 0 || ap_records == NULL) {
        glog("No APs found during scan.\n");
        status_display_show_status("No AP Found");
        return;
    }

    int unique_count = 0;
    int *channels = malloc(ap_count * sizeof(int));
    int *counts = malloc(ap_count * sizeof(int));
    int max_count = 0;
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_records[i].primary;
        if (ch <= 0) continue;
        int idx = -1;
        for (int j = 0; j < unique_count; j++) {
            if (channels[j] == ch) { idx = j; break; }
        }
        if (idx >= 0) {
            counts[idx]++;
        } else {
            channels[unique_count] = ch;
            counts[unique_count] = 1;
            idx = unique_count++;
        }
        if (counts[idx] > max_count) {
            max_count = counts[idx];
        }
    }
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = i + 1; j < unique_count; j++) {
            if (channels[i] > channels[j]) {
                int tmp_ch = channels[i]; channels[i] = channels[j]; channels[j] = tmp_ch;
                int tmp_cnt = counts[i]; counts[i] = counts[j]; counts[j] = tmp_cnt;
            }
        }
    }

    glog("\nChannel Congestion:\n\n");
    const char* header = "+----+-------+------------+\n";
    const char* separator = "+----+-------+------------+\n";
    const char* row_format = "| %2d | %5d | %s |\n";
    const char* footer = "+----+-------+------------+\n";

    glog("%s", header);
    glog("| CH | Count | Bar        |\n");
    glog("%s", separator);

    const int max_bar_length = 10;
    char display_bar[max_bar_length * 4]; // Generous buffer: 3 bytes/block + 1 space/pad + null

    for (int i = 0; i < unique_count; i++) {
        int ch = channels[i];
        int cnt = counts[i];
        int bar_length = 0;
        if (max_count > 0) {
            bar_length = (int)(((float)cnt / max_count) * max_bar_length);
            if (bar_length == 0 && cnt > 0) bar_length = 1;
        }
        char *ptr = display_bar;
        for (int j = 0; j < bar_length; ++j) {
            *ptr++ = '#';
        }
        int spaces_needed = max_bar_length - bar_length;
        for (int j = 0; j < spaces_needed; ++j) {
            *ptr++ = ' ';
        }
        *ptr = '\0';
        glog(row_format, ch, cnt, display_bar);
    }
    free(channels);
    free(counts);
    glog("%s", footer);
}

// Forward declaration for the new print function
void wifi_manager_scanall_chart();

void handle_scanall(int argc, char **argv) {
    int total_seconds = 10; // Default total duration: 10 seconds
    if (argc > 1) {
        char *endptr;
        long sec = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && sec > 0) {
            total_seconds = (int)sec;
        } else {
            glog("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
            status_display_show_status("ScanAll Usage");
        }
    }

    int ap_scan_seconds = total_seconds / 2;
    int sta_scan_seconds = total_seconds - ap_scan_seconds; // Use remaining time

    glog("Starting combined scan (%d sec AP, %d sec STA)...\n", ap_scan_seconds, sta_scan_seconds);
    status_display_show_status("ScanAll Start");

    // 1. Perform AP Scan
    glog("--- Starting AP Scan (%d seconds) ---\n", ap_scan_seconds);
    wifi_manager_start_scan_with_time(ap_scan_seconds);
    // Results are now in scanned_aps and ap_count

    // 2. Perform Station Scan
    glog("--- Starting Station Scan (%d seconds) ---\n", sta_scan_seconds);
    station_count = 0; // Reset station list before new scan
    wifi_manager_start_station_scan(); // Starts monitor mode + channel hopping
    glog("Station scan running for %d seconds...\n", sta_scan_seconds);
    vTaskDelay(pdMS_TO_TICKS(sta_scan_seconds * 1000));
    wifi_manager_stop_monitor_mode(); // Stops monitor mode + channel hopping
    // Results are now in station_ap_list and station_count

    glog("--- Scan Complete ---\n");

    // 3. Print Combined Results
    wifi_manager_scanall_chart();

    // Ensure AP mode is restored if it was stopped
    ap_manager_start_services(); // Restore AP for WebUI
    status_display_show_status("ScanAll Done");
}

// Helper function to simplify calling list airtags
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv) {
    ble_list_airtags();
    status_display_show_status("List AirTags");
}
#endif

// Select AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_select_airtag(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectairtag <number>\n");
        status_display_show_status("AirTag Usage");
        return;
    }

    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_airtag(num);
        status_display_show_status("AirTag Select");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("AirTag Invalid");
    }
}
#endif

// Spoof AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_spoof_airtag(int argc, char **argv) {
    ble_start_spoofing_selected_airtag();
    status_display_show_status("AirTag Spoof");
}
#endif

// Stop Spoof handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_stop_spoof(int argc, char **argv) {
    ble_stop_spoofing();
    status_display_show_status("Spoof Stop");
}
#endif

// Handlers for Flipper commands
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_flippers_cmd(int argc, char **argv) {
    ble_list_flippers();
    status_display_show_status("List Flipper");
}

void handle_select_flipper_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectflipper <index>\n");
        status_display_show_status("Flipper Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_flipper(num);
        status_display_show_status("Flipper Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("Flipper Bad");
    }
}
#endif

// New beacon list command handlers
void handle_beaconadd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: beaconadd <SSID>\n");
        status_display_show_status("BeaconAdd Use");
        return;
    }
    wifi_manager_add_beacon_ssid(argv[1]);
    status_display_show_status("Beacon Added");
}

void handle_beaconremove(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: beaconremove <SSID>\n");
        status_display_show_status("BeaconRm Use");
        return;
    }
    wifi_manager_remove_beacon_ssid(argv[1]);
    status_display_show_status("Beacon Removed");
}

void handle_beaconclear(int argc, char **argv) {
    wifi_manager_clear_beacon_list();
    status_display_show_status("Beacon Clear");
}

void handle_beaconshow(int argc, char **argv) {
    wifi_manager_show_beacon_list();
    status_display_show_status("Beacon Show");
}

void handle_beaconspamlist(int argc, char **argv) {
    wifi_manager_start_beacon_list();
    status_display_show_status("Beacon List On");
}

void handle_dhcpstarve_cmd(int argc, char **argv) {
    if (argc < 2) {
        wifi_manager_dhcpstarve_help();
        status_display_show_status("DHCP Usage");
    } else if (strcmp(argv[1], "start") == 0) {
        int thr = (argc >= 3) ? atoi(argv[2]) : 1;
        wifi_manager_start_dhcpstarve(thr);
        status_display_show_status("DHCP Start");
    } else if (strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_dhcpstarve();
        status_display_show_status("DHCP Stop");
    } else if (strcmp(argv[1], "display") == 0) {
        wifi_manager_dhcpstarve_display();
        status_display_show_status("DHCP Stats");
    } else {
        wifi_manager_dhcpstarve_help();
        status_display_show_status("DHCP Usage");
    }
}
#if CONFIG_IDF_TARGET_ESP32C5
void handle_setcountry(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: setcountry <CC>\n");
        status_display_show_status("Country Usage");
        return;
    }
    esp_err_t err = esp_wifi_set_country_code(argv[1], true);
    if (err == ESP_OK) {
        glog("country set to %s\n", argv[1]);
        status_display_show_status("Country Set");
    } else {
        glog("failed to set country: %s\n", esp_err_to_name(err));
        status_display_show_status("Country Fail");
    }
}
#endif

void handle_listen_probes_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        g_listen_probes_save_to_sd = false;
        glog("Probe request listening stopped.\n");
        status_display_show_status("Probes Stop");
        return;
    }

    uint8_t channel = 0;
    bool channel_hopping = true;

    if (argc > 1) {
        char *endptr;
        long ch = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && ch >= 1 && ch <= MAX_WIFI_CHANNEL) {
            channel = (uint8_t)ch;
            channel_hopping = false;
            glog("Starting to listen for probe requests on channel %d...\n", channel);
            char status_msg[18];
            snprintf(status_msg, sizeof(status_msg), "Probes Ch %02d", channel);
            status_display_show_status(status_msg);
        } else {
            glog("Invalid channel: %s. Valid range: 1-%d\n", argv[1], MAX_WIFI_CHANNEL);
            status_display_show_status("Channel Bad");
            return;
        }
    } else {
        glog("Starting to listen for probe requests (channel hopping)...\n");
        status_display_show_status("Probes Hop");
    }

    bool sd_available = sd_card_exists("/mnt/ghostesp/pcaps");
    g_listen_probes_save_to_sd = sd_available;
    if (sd_available) {
        int err = pcap_file_open("probelisten", PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            glog("Warning: PCAP file open failed; probes will not be saved to SD card.\n");
            g_listen_probes_save_to_sd = false;
            status_display_show_status("PCAP Warn");
        }
    } else {
        glog("SD card not available; probe PCAP disabled.\n");
        status_display_show_status("SD Missing");
    }

    if (channel_hopping) {
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    } else {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    }
}

void handle_web_auth_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: webauth <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        settings_set_web_auth_enabled(&G_Settings, true);
        settings_save(&G_Settings);
        glog("Web authentication enabled.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        settings_set_web_auth_enabled(&G_Settings, false);
        settings_save(&G_Settings);
        glog("Web authentication disabled.\n");
    } else {
        glog("Usage: webauth <on|off>\n");
    }
}


void handle_listportals(int argc, char **argv);
void handle_evilportal(int argc, char **argv);
void handle_wifi_disconnect(int argc, char **argv);
void handle_set_rgb_mode_cmd(int argc, char **argv);
void handle_karma_cmd(int argc, char **argv);
void handle_set_neopixel_brightness_cmd(int argc, char **argv);
void handle_get_neopixel_brightness_cmd(int argc, char **argv);


void handle_comm_discovery(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    
    if (state == COMM_STATE_SCANNING) {
        glog("Already in discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Scanning");
        return;
    }
    
    if (esp_comm_manager_start_discovery()) {
        glog("Started discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Discover");
    } else {
        glog("Failed to start discovery. Check if already connected.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_connect(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: commconnect <peer_name>\n");
        glog("Example: commconnect ESP_A1B2C3\n");
        status_display_show_status("CommConn Use");
        return;
    }
    
    if (esp_comm_manager_connect_to_peer(argv[1])) {
        glog("Attempting to connect to peer: %s\n", argv[1]);
        status_display_show_status("Comm Connect");
    } else {
        glog("Failed to connect. Make sure you're in discovery mode first.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_send(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: commsend <command> [data]\n");
        glog("Example: commsend hello world\n");
        glog("Example: commsend scanap\n");
        status_display_show_status("CommSend Use");
        return;
    }
    
    if (!esp_comm_manager_is_connected()) {
        glog("Not connected to any peer. Use 'commdiscovery' and 'commconnect' first.\n");
        status_display_show_status("Comm NotConn");
        return;
    }
    
    char data_buffer[256] = {0};
    if (argc > 2) {
        int offset = 0;
        for (int i = 2; i < argc; i++) {
            int remaining = sizeof(data_buffer) - offset;
            int written = snprintf(data_buffer + offset, remaining, "%s ", argv[i]);
            if (written >= remaining) {
                glog("W: Command data truncated.\n");
                break;
            }
            offset += written;
        }
        if (offset > 0) {
            data_buffer[offset - 1] = '\0'; // Remove trailing space
        }
    }

    const char* command = argv[1];
    const char* data = (argc > 2) ? data_buffer : NULL;

    if (esp_comm_manager_send_command(command, data)) {
        if (data && data[0] != '\0') {
            glog("Command sent: %s %s\n", command, data);
        } else {
            glog("Command sent: %s\n", command);
        }
        status_display_show_status("Comm Sent");
    } else {
        glog("Failed to send command.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_status(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    const char* state_str;
    
    switch(state) {
        case COMM_STATE_IDLE: state_str = "IDLE"; break;
        case COMM_STATE_SCANNING: state_str = "SCANNING"; break;
        case COMM_STATE_HANDSHAKE: state_str = "HANDSHAKE"; break;
        case COMM_STATE_CONNECTED: state_str = "CONNECTED"; break;
        case COMM_STATE_ERROR: state_str = "ERROR"; break;
        default: state_str = "UNKNOWN"; break;
    }
    
    glog("Communication Status: %s\n", state_str);
    if (esp_comm_manager_is_connected()) {
        glog("Connected to peer. Ready to send commands.\n");
        status_display_show_status("Comm Connected");
    } else {
        glog("Not connected. Use 'commdiscovery' to find peers.\n");
        status_display_show_status("Comm Idle");
    }
}

void handle_comm_disconnect(int argc, char **argv) {
    esp_comm_manager_disconnect();
    glog("Disconnected from peer.\n");
    status_display_show_status("Comm Closed");
}

void handle_comm_setpins(int argc, char **argv) {
    if (argc != 3) {
        glog("Usage: commsetpins <tx_pin> <rx_pin>\n");
        glog("Example: commsetpins 4 5\n");
        status_display_show_status("Pins Usage");
        return;
    }
    
    int tx_pin = atoi(argv[1]);
    int rx_pin = atoi(argv[2]);
    
    if (tx_pin < 0 || tx_pin > 48 || rx_pin < 0 || rx_pin > 48) {
        glog("Invalid pin numbers. Must be between 0-48.\n");
        status_display_show_status("Pins Invalid");
        return;
    }
    
    if (esp_comm_manager_set_pins((gpio_num_t)tx_pin, (gpio_num_t)rx_pin)) {
        settings_set_esp_comm_pins(&G_Settings, tx_pin, rx_pin);
        settings_save(&G_Settings);
        
        glog("Communication pins changed to TX:%d RX:%d and saved to NVS\n", tx_pin, rx_pin);
        status_display_show_status("Pins Updated");
    } else {
        glog("Failed to change pins. Make sure not connected or scanning.\n");
        status_display_show_status("Pins Failed");
    }
}

static void comm_command_callback(const char* command, const char* data, void* user_data) {
    static char full_command[128];
    
    // Minimal processing in command executor task - just queue the command
    if (data && strlen(data) > 0) {
        snprintf(full_command, sizeof(full_command), "peer:%s %s", command, data);
    } else {
        snprintf(full_command, sizeof(full_command), "peer:%s", command);
    }
    
    simulateCommand(full_command);
}
void handle_ap_enable_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: apenable <on|off>\n");
        glog("Example: apenable on\n");
        glog("         apenable off\n");
        status_display_show_status("APEnable Use");
        return;
    }
    
    bool enable = false;
    if (strcmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        glog("Invalid argument. Use 'on' or 'off'\n");
        status_display_show_status("APEnable Bad");
        return;
    }
    
    settings_set_ap_enabled(&G_Settings, enable);
    settings_save(&G_Settings);
    
    glog("Access Point %s. Restart required to take effect.\n", enable ? "enabled" : "disabled");
    status_display_show_status(enable ? "AP Enabled" : "AP Disabled");
}

void handle_chip_info_cmd(int argc, char **argv) {
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    
    esp_chip_info(&chip_info);
    
    const char *model_name = "Unknown";
    switch(chip_info.model) {
        case CHIP_ESP32:
            model_name = "ESP32";
            break;
        case CHIP_ESP32S2:
            model_name = "ESP32-S2";
            break;
        case CHIP_ESP32S3:
            model_name = "ESP32-S3";
            break;
        case CHIP_ESP32C3:
            model_name = "ESP32-C3";
            break;
        case CHIP_ESP32C2:
            model_name = "ESP32-C2";
            break;
        case CHIP_ESP32C6:
            model_name = "ESP32-C6";
            break;
        case CHIP_ESP32H2:
            model_name = "ESP32-H2";
            break;
        case CHIP_ESP32P4:
            model_name = "ESP32-P4";
            break;
        case CHIP_ESP32C5:
            model_name = "ESP32-C5";
            break;
        case CHIP_ESP32C61:
            model_name = "ESP32-C61";
            break;
        default:
            model_name = "Unknown";
            break;
    }
    
    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    
    glog("Chip Information:\n");
    glog("  Model: %s\n", model_name);
    glog("  Revision: v%d.%d\n", major_rev, minor_rev);
    glog("  CPU Cores: %d\n", chip_info.cores);

    char features_str[256] = "";
    bool first = true;
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) {
        strcat(features_str, "WiFi");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_BT) {
        if (!first) strcat(features_str, "/");
        strcat(features_str, "BT");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_BLE) {
        if (!first) strcat(features_str, "/");
        strcat(features_str, "BLE");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_IEEE802154) {
        if (!first) strcat(features_str, "/");
        strcat(features_str, "802.15.4");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH) {
        if (!first) strcat(features_str, "/");
        strcat(features_str, "Embedded Flash");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_EMB_PSRAM) {
        if (!first) strcat(features_str, "/");
        strcat(features_str, "Embedded PSRAM");
        first = false;
    }
    if (first) {
        strcat(features_str, "None");
    }
    glog("  Features: %s\n", features_str);

    glog("  Free Heap: %lu bytes\n", esp_get_free_heap_size());
    glog("  Min Free Heap: %lu bytes\n", esp_get_minimum_free_heap_size());
    glog("  IDF Version: %s\n", esp_get_idf_version());
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    glog("  Build Config: %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);
#endif
    
    glog("  Model: %s\n  Revision: v%d.%d\n  CPU Cores: %d\n  Free Heap: %lu bytes\n",
          model_name, major_rev, minor_rev, chip_info.cores, esp_get_free_heap_size());
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    glog("  Build Config: %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);
#endif
    status_display_show_status("Chip Info");
}

// Settings command handler
void handle_settings_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Settings Management Commands:\n");
        glog("  settings list                    - List all available settings\n");
        glog("  settings get <setting>           - Get current value of a setting\n");
        glog("  settings set <setting> <value>   - Set a setting to a value\n");
        glog("  settings reset [setting]         - Reset setting(s) to defaults\n");
        glog("  settings help                    - Show this help\n");
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        glog("Settings Management Commands:\n");
        glog("  settings list                    - List all available settings\n");
        glog("  settings get <setting>           - Get current value of a setting\n");
        glog("  settings set <setting> <value>   - Set a setting to a value\n");
        glog("  settings reset [setting]         - Reset setting(s) to defaults\n");
        glog("  settings help                    - Show this help\n");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        glog("Available Settings:\n");
        glog("  RGB Settings:\n");
        glog("    rgb_mode          - RGB mode (0=Normal, 1=Rainbow, 2=Stealth)\n");
        glog("    rgb_speed         - RGB animation speed (0-255)\n");
        glog("    rgb_data_pin      - RGB data pin (-1 if not used)\n");
        glog("    rgb_red_pin       - RGB red pin (-1 if not used)\n");
        glog("    rgb_green_pin     - RGB green pin (-1 if not used)\n");
        glog("    rgb_blue_pin      - RGB blue pin (-1 if not used)\n");
        glog("    neopixel_bright   - Neopixel max brightness (0-100)\n");
        glog("  WiFi Settings:\n");
        glog("    ap_ssid           - Access Point SSID\n");
        glog("    ap_password       - Access Point password\n");
        glog("    ap_enabled        - Enable AP on boot (true/false)\n");
        glog("    sta_ssid          - Station mode SSID\n");
        glog("    sta_password      - Station mode password\n");
        glog("  Evil Portal Settings:\n");
        glog("    portal_url        - Portal URL or file path\n");
        glog("    portal_ssid       - Portal SSID\n");
        glog("    portal_password   - Portal password\n");
        glog("    portal_ap_ssid    - Portal AP SSID\n");
        glog("    portal_domain     - Portal domain\n");
        glog("    portal_offline    - Portal offline mode (true/false)\n");
        glog("  Printer Settings:\n");
        glog("    printer_ip        - Printer IP address\n");
        glog("    printer_text      - Last printed text\n");
        glog("    printer_font_size - Printer font size\n");
        glog("    printer_alignment - Printer alignment (0-4)\n");
        glog("  Display Settings:\n");
        glog("    display_timeout   - Display timeout in ms\n");
        glog("    max_bright        - Max screen brightness (0-100)\n");
        glog("    invert_colors     - Invert screen colors (true/false)\n");
        glog("    terminal_color    - Terminal text color (hex)\n");
        glog("    menu_theme        - Menu theme (0=Default)\n");
        glog("  System Settings:\n");
        glog("    channel_delay     - Channel delay in ms\n");
        glog("    broadcast_speed   - Broadcast speed\n");
        glog("    gps_rx_pin        - GPS RX pin\n");
        glog("    power_save        - Power save mode (true/false)\n");
        glog("    zebra_menus       - Zebra menus (true/false)\n");
        glog("    nav_buttons       - Navigation buttons (true/false)\n");
        glog("    menu_layout       - Menu layout (0=Carousel, 1=Grid, 2=List)\n");
        glog("    infrared_easy     - Infrared easy mode (true/false)\n");
        glog("    web_auth          - Web authentication (true/false)\n");
        glog("    rts_enabled       - RTS enabled (true/false)\n");
        glog("    third_ctrl        - Third control enabled (true/false)\n");
        glog("  Custom Settings:\n");
        glog("    flappy_name       - Flappy Ghost name\n");
        glog("    timezone          - Selected timezone\n");
        glog("    accent_color      - Accent color (hex)\n");
        return;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) { glog("Usage: settings get <setting>\n"); return; }
        const char *setting = argv[2];
        const SettingDescriptor *d = find_setting_desc(setting);
        if (!d) {
            glog("Unknown setting: %s\n", setting);
            glog("Use 'settings list' to see available settings\n");
            return;
        }
        print_setting_value(d, &G_Settings);
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) { glog("Usage: settings set <setting> <value>\n"); return; }
        const char *setting = argv[2];
        const char *value = argv[3];
        const SettingDescriptor *d = find_setting_desc(setting);
        if (!d) {
            glog("Unknown setting: %s\n", setting);
            glog("Use 'settings list' to see available settings\n");
            return;
        }
        FSettings *settings = &G_Settings;
        if (!set_setting_value(d, settings, value)) {
            if (d->type == ST_BOOL) {
                glog("Invalid %s. Use true or false\n", d->name);
            } else if (d->type == ST_U8 || d->type == ST_U16 || d->type == ST_ENUM8) {
                if (d->max_i > d->min_i) glog("Invalid %s. Use %d-%d\n", d->name, d->min_i, d->max_i);
                else glog("Invalid %s value\n", d->name);
            } else if (d->type == ST_COLOR_HEX) {
                glog("Invalid %s. Use hex like 00FF00\n", d->name);
            } else {
                glog("Invalid %s value\n", d->name);
            }
            return;
        }
        settings_save(settings);
        log_set_confirmation(d, settings);
        return;
    }

    if (strcmp(argv[1], "reset") == 0) {
        if (argc == 2) {
            settings_set_defaults(&G_Settings);
            settings_save(&G_Settings);
            glog("Reset all settings to defaults\n");
        } else if (argc == 3) {
            const char *setting = argv[2];
            const SettingDescriptor *d = find_setting_desc(setting);
            if (!d) {
                glog("Unknown setting: %s\n", setting);
                glog("Use 'settings list' to see available settings\n");
                return;
            }
            FSettings defaults; settings_set_defaults(&defaults);
            reset_setting_value(d, &G_Settings, &defaults);
            settings_save(&G_Settings);
            glog("Reset %s to default\n", d->name);
        } else {
            glog("Usage: settings reset [setting]\n");
        }
        return;
    }

    glog("Unknown settings command: %s\n", argv[1]);
    glog("Use 'settings help' for available commands\n");
}

void register_commands() {
    command_init();
    register_command("help", handle_help);
    register_command("mem", handle_mem_cmd);
    register_command("scanap", cmd_wifi_scan_start);
    register_command("scansta", handle_sta_scan);
    register_command("scanlocal", handle_ip_lookup);
    register_command("stopscan", cmd_wifi_scan_stop);
    register_command("attack", handle_attack_cmd);
    register_command("list", handle_list);
    register_command("beaconspam", handle_beaconspam);
    register_command("beaconadd", handle_beaconadd);
    register_command("beaconremove", handle_beaconremove);
    register_command("beaconclear", handle_beaconclear);
    register_command("beaconshow", handle_beaconshow);
    register_command("beaconspamlist", handle_beaconspamlist);
    register_command("stopspam", handle_stop_spam);
    register_command("stopdeauth", handle_stop_deauth);
    register_command("select", handle_select_cmd);
    register_command("capture", handle_capture_scan);
    register_command("startportal", handle_start_portal);
    register_command("disconnect", handle_wifi_disconnect);
    register_command("stopportal", stop_portal);
    register_command("connect", handle_wifi_connection);
    register_command("dialconnect", handle_dial_command);
    register_command("powerprinter", handle_printer_command);
    register_command("tplinktest", handle_tp_link_test);
    register_command("stop", handle_stop_flipper);
    register_command("reboot", handle_reboot);
    register_command("startwd", handle_startwd);
    register_command("gpsinfo", handle_gps_info);
    register_command("scanports", handle_scan_ports);
    register_command("scanarp", handle_scan_arp);
    register_command("scanssh", handle_scan_ssh);
    register_command("congestion", handle_congestion_cmd);
    register_command("listenprobes", handle_listen_probes_cmd);
    register_command("settings", handle_settings_cmd);
    register_command("listportals", handle_listportals);
    register_command("evilportal", handle_evilportal);
    register_command("commdiscovery", handle_comm_discovery);
    register_command("commconnect", handle_comm_connect);
    register_command("commsend", handle_comm_send);
    register_command("commstatus", handle_comm_status);
    register_command("commdisconnect", handle_comm_disconnect);
    register_command("commsetpins", handle_comm_setpins);

#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blescan", handle_ble_scan_cmd);
    register_command("blewardriving", handle_ble_wardriving);
    register_command("listairtags", handle_list_airtags_cmd);
    register_command("selectairtag", handle_select_airtag);
    register_command("spoofairtag", handle_spoof_airtag);
    register_command("stopspoof", handle_stop_spoof);
#endif
#ifdef DEBUG
    register_command("crash", handle_crash);
#endif
    register_command("pineap", handle_pineap_detection);
    register_command("apcred", handle_apcred);
    register_command("apenable", handle_ap_enable_cmd);
    register_command("chipinfo", handle_chip_info_cmd);
    register_command("rgbmode", handle_rgb_mode);
    register_command("setrgbpins", handle_setrgb);
    register_command("sd_config", handle_sd_config);
    register_command("sd_pins_mmc", handle_sd_pins_mmc);
    register_command("sd_pins_spi", handle_sd_pins_spi);
    register_command("sd_save_config", handle_sd_save_config);
    register_command("scanall", handle_scanall);
    register_command("timezone", handle_timezone_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("listflippers", handle_list_flippers_cmd);
    register_command("selectflipper", handle_select_flipper_cmd);
#endif
    register_command("dhcpstarve", handle_dhcpstarve_cmd);
    register_command("saeflood", handle_sae_flood_cmd);
    register_command("stopsaeflood", handle_stop_sae_flood_cmd);
    register_command("saefloodhelp", handle_sae_flood_help_cmd);
#if CONFIG_IDF_TARGET_ESP32C5
    register_command("setcountry", handle_setcountry);
#endif
    register_command("webauth", handle_web_auth_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blespam", handle_ble_spam_cmd);
#endif
    register_command("setrgbmode", handle_set_rgb_mode_cmd);
    register_command("karma", handle_karma_cmd);
    register_command("setneopixelbrightness", handle_set_neopixel_brightness_cmd);
    register_command("getneopixelbrightness", handle_get_neopixel_brightness_cmd);
    
    esp_comm_manager_set_command_callback(comm_command_callback, NULL);
    
    glog("Registered Commands\n");
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_spam_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-apple") == 0) {
            glog("starting apple ble spam...\n");
            ble_start_ble_spam(BLE_SPAM_APPLE);
            return;
        }
        if (strcmp(argv[1], "-ms") == 0 || strcmp(argv[1], "-microsoft") == 0) {
            glog("starting microsoft ble spam...\n");
            ble_start_ble_spam(BLE_SPAM_MICROSOFT);
            return;
        }
        if (strcmp(argv[1], "-samsung") == 0) {
            glog("starting samsung ble spam...\n");
            ble_start_ble_spam(BLE_SPAM_SAMSUNG);
            return;
        }
        if (strcmp(argv[1], "-google") == 0) {
            glog("starting google ble spam...\n");
            ble_start_ble_spam(BLE_SPAM_GOOGLE);
            return;
        }
        if (strcmp(argv[1], "-random") == 0) {
            glog("starting random ble spam...\n");
            ble_start_ble_spam(BLE_SPAM_RANDOM);
            return;
        }
        if (strcmp(argv[1], "-s") == 0) {
            glog("stopping ble spam...\n");
            ble_stop_ble_spam();
            return;
        }
    }
    glog("usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
}
#endif

void handle_listportals(int argc, char **argv) {
    char portal_names[MAX_PORTALS][MAX_PORTAL_NAME];
    int count = get_evil_portal_list(portal_names);

    if (count <= 0) {
        glog("No portals found.\n");
        return;
    }

    glog("Available Evil Portals:\n");
    for (int i = 0; i < count; ++i) {
        glog("  %.508s\n", portal_names[i]);
    }
}

void handle_evilportal(int argc, char **argv) {
    if (argc < 3) {
        glog("Usage: %s -c <command>\n", argv[0]);
        glog("Commands:\n");
        glog("  sethtmlstr - Set HTML content from buffer (use with UART markers)\n");
        glog("  clear - Clear HTML buffer and disable buffer mode\n");
        return;
    }

    if (strcmp(argv[1], "-c") != 0) {
        glog("Error: Expected -c flag\n");
        return;
    }

    if (strcmp(argv[2], "sethtmlstr") == 0) {
        wifi_manager_set_html_from_uart();
        glog("HTML buffer mode enabled for evil portal\n");
    } else if (strcmp(argv[2], "clear") == 0) {
        wifi_manager_clear_html_buffer();
        glog("HTML buffer cleared - will use default portal on next startportal\n");
    } else {
        glog("Error: Unknown command '%s'\n", argv[2]);
    }
}

void handle_set_rgb_mode_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: setrgbmode <normal|rainbow|stealth>\n");
        return;
    }
    RGBMode mode;
    if (strcasecmp(argv[1], "normal") == 0) {
        mode = RGB_MODE_NORMAL;
    } else if (strcasecmp(argv[1], "rainbow") == 0) {
        mode = RGB_MODE_RAINBOW;
    } else if (strcasecmp(argv[1], "stealth") == 0) {
        mode = RGB_MODE_STEALTH;
    } else {
        glog("Invalid mode '%s'. Supported modes: normal, rainbow, stealth\n", argv[1]);
        return;
    }
    settings_set_rgb_mode(&G_Settings, mode);
    settings_save(&G_Settings);
    glog("RGB mode set to %s\n", argv[1]);
}

void handle_karma_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: karma <start|stop> [ssid1 ssid2 ...]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: karma <start|stop> [ssid1 ssid2 ...]\n");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        if (argc > 2) {
            // User specified SSIDs
            const char *ssid_list[32];
            int ssid_count = 0;
            for (int i = 2; i < argc && ssid_count < 32; ++i) {
                if (strlen(argv[i]) > 0 && strlen(argv[i]) < 33) {
                    ssid_list[ssid_count++] = argv[i];
                }
            }
            if (ssid_count > 0) {
                wifi_manager_set_karma_ssid_list(ssid_list, ssid_count);
                printf("Karma SSID list set (%d):\n", ssid_count);
                for (int i = 0; i < ssid_count; ++i) {
                    printf("  %s\n", ssid_list[i]);
                    TERMINAL_VIEW_ADD_TEXT("  %s\n", ssid_list[i]);
                }
            }
        }
        wifi_manager_start_karma();
    } else if (strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_karma();
    } else {
        printf("Usage: karma <start|stop> [ssid1 ssid2 ...]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: karma <start|stop> [ssid1 ssid2 ...]\n");
    }
}

void handle_set_neopixel_brightness_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: setneopixelbrightness <0-100>\n");
        glog("Example: setneopixelbrightness 50\n");
        return;
    }
    
    int brightness = atoi(argv[1]);
    if (brightness < 0 || brightness > 100) {
        glog("Invalid brightness value '%s'. Must be between 0-100\n", argv[1]);
        return;
    }
    
    settings_set_neopixel_max_brightness(&G_Settings, (uint8_t)brightness);
    settings_save(&G_Settings);
    glog("Neopixel max brightness set to %d%%\n", brightness);
}

void handle_get_neopixel_brightness_cmd(int argc, char **argv) {
    uint8_t brightness = settings_get_neopixel_max_brightness(&G_Settings);
    glog("Current neopixel max brightness: %d%%\n", brightness);
}