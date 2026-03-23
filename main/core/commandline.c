 // command.c

#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/serial_manager.h"
#include "core/utils.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "managers/ap_manager.h"
#include "sdkconfig.h"
#include "vendor/drivers/pcf8563.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#include "attacks/ble/ble_spam.h"
#include "scans/ble/flipper_scan.h"
#endif
#include "managers/dial_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/port_scan.h"
#include "scans/wifi/ssh_scan.h"
#include "managers/sd_card_manager.h"
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_visualizer.h"
#endif
#include "vendor/pcap.h"
#include "vendor/printer.h"
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
#include "managers/zigbee_manager.h"
#endif
#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#include "managers/badusb_builtin_script.h"
#endif
#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_comm_handler.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "managers/ethernet/eth_utils.h"
#include "managers/ethernet/eth_http.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

// Forward declaration - esp_netif_get_netif_impl is not in public API but exists internally
void* esp_netif_get_netif_impl(esp_netif_t *esp_netif);
#endif
#include <esp_timer.h>
#include <managers/gps_manager.h>
#include <managers/views/terminal_screen.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <vendor/dial_client.h>
#include "esp_wifi.h"
#include "managers/default_portal.h"
#include "core/glog.h"
#include <time.h>
#include <dirent.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "core/ghostesp_version.h"
#include "managers/chameleon_manager.h"
#include <stddef.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_memory_utils.h"
#include <dirent.h>
#include "managers/infrared_manager.h"
#include "core/universal_ir.h"
#include "core/screen_mirror.h"
#include "managers/display_manager.h"
#include "freertos/queue.h"
#include "managers/usb_keyboard_manager.h"
#include "mbedtls/base64.h"
#include "esp_partition.h"
#include "esp_core_dump.h"
#include "managers/aerial_detector_manager.h"
#include "managers/wigle_manager.h"
#include "managers/config_manager.h"
#include "managers/nrf24_remote_manager.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/app_gallery_screen.h"

#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
#include "managers/views/nrf24_analyzer_view.h"
#endif

#include "attacks/wifi/dhcp_starvation.h"
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

// Static storage for GPS info task stack and TCB to enable proper cleanup
static StackType_t* gps_task_stack = NULL;
static StaticTask_t* gps_task_tcb = NULL;

// Forward declarations for command handlers
void cmd_wifi_scan_stop(int argc, char **argv);
void handle_listportals(int argc, char **argv);
void handle_evilportal(int argc, char **argv);
void handle_wifi_disconnect(int argc, char **argv);
void handle_wifi_status(int argc, char **argv);
void handle_set_rgb_mode_cmd(int argc, char **argv);
void handle_karma_cmd(int argc, char **argv);
void handle_set_neopixel_brightness_cmd(int argc, char **argv);
void handle_get_neopixel_brightness_cmd(int argc, char **argv);
void handle_webuiap_cmd(int argc, char **argv);
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv);
void handle_select_airtag(int argc, char **argv);
void handle_spoof_airtag(int argc, char **argv);
void handle_stop_spoof(int argc, char **argv);
void handle_ble_spam_cmd(int argc, char **argv);
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
void handle_status_idle_cmd(int argc, char **argv);
#endif
void handle_settime_cmd(int argc, char **argv);
void handle_time_cmd(int argc, char **argv);
void handle_aerial_scan_cmd(int argc, char **argv);
void handle_aerial_list_cmd(int argc, char **argv);
void handle_aerial_track_cmd(int argc, char **argv);
void handle_aerial_stop_cmd(int argc, char **argv);
void handle_aerial_spoof_cmd(int argc, char **argv);
void handle_aerial_spoof_stop_cmd(int argc, char **argv);
void handle_wigle_cmd(int argc, char **argv);
void handle_loadconfig_cmd(int argc, char **argv);
void handle_nrf24_cmd(int argc, char **argv);

#define MAX_PORTAL_PATH_LEN 128 // reasonable i guess?

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

typedef struct {
    int last_percent;
    int last_total;
} chameleon_cli_progress_state_t;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

static void chameleon_cli_progress_cb(int current, int total, void *user) {
    chameleon_cli_progress_state_t *state = (chameleon_cli_progress_state_t *)user;
    if (!state || total <= 0) return;
    if (current < 0) current = 0;
    if (current > total) current = total;
    if (total != state->last_total) state->last_percent = -1;
    int percent = (int)((current * 100) / total);
    if (percent != state->last_percent) {
        glog("Classic dictionary progress: %d%% (%d/%d)\n", percent, current, total);
        state->last_percent = percent;
        state->last_total = total;
    }
}

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
        glog("Failed to register command '%s': out of memory\n", name);
        return;
    }
    new_command->name = strdup(name);
    if (new_command->name == NULL) {
        glog("Failed to register command '%s': out of memory\n", name);
        free(new_command);
        return;
    }
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
    glog("Unsupported command: %s\n", cmd);
}

void cmd_wifi_scan_start(int argc, char **argv) {
    esp_err_t timed_scan_err = ESP_OK;
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
        char *endptr = NULL;
        long seconds_long = strtol(argv[1], &endptr, 10);
        if (endptr == argv[1] || *endptr != '\0') {
            glog("Invalid scan duration '%s'. Use an integer number of seconds.\n", argv[1]);
            return;
        }
        if (seconds_long < 1 || seconds_long > 120) {
            glog("Scan duration out of range (%ld). Valid range is 1-120 seconds.\n", seconds_long);
            return;
        }
        timed_scan_err = wifi_manager_start_scan_with_time((int)seconds_long);
        if (timed_scan_err != ESP_OK) {
            glog("WiFi timed scan failed: %s\n", esp_err_to_name(timed_scan_err));
            status_display_show_status("Scan Failed");
            return;
        }
    } else {
        wifi_manager_start_scan();
    }
    wifi_manager_print_scan_results_with_oui();
    status_display_show_status("Scan Complete");
}

void cmd_wifi_scan_stop(int argc, char **argv) {
    // Properly stop any ongoing WiFi scan
    wifi_manager_stop_scan();

    // Stop monitor mode
    wifi_manager_stop_monitor_mode();

    // Close pcap file
    pcap_file_close();

    // Reset WiFi to a good state
    esp_err_t stop_err = esp_wifi_stop();
    esp_err_t start_err = esp_wifi_start();

    if (stop_err != ESP_OK || start_err != ESP_OK) {
        glog("WiFi scan stop completed with recovery errors (stop=%s, start=%s).\n",
             esp_err_to_name(stop_err), esp_err_to_name(start_err));
        status_display_show_status("Scan Stop Warn");
        return;
    }

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
    {"auto_save_scans", ST_BOOL, OFF(auto_save_scans), "System", 0, 0, 0},

    {"flappy_name", ST_STRING, OFF(flappy_ghost_name), "Custom", 65, 0, 0},
    {"timezone", ST_STRING, OFF(selected_timezone), "Custom", 25, 0, 0},
    {"accent_color", ST_STRING, OFF(selected_hex_accent_color), "Custom", 25, 0, 0},
    {"io_btn_p10_cmd", ST_STRING, OFF(io_btn_p10_cmd), "IO Button", 129, 0, 0},
    {"io_btn_p11_cmd", ST_STRING, OFF(io_btn_p11_cmd), "IO Button", 129, 0, 0},
    {"io_btn_p12_cmd", ST_STRING, OFF(io_btn_p12_cmd), "IO Button", 129, 0, 0},
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
        } else if (strcmp(argv[1], "-c") == 0) {
            glog("Channel Switch attack starting...\n");
            wifi_manager_start_channel_switch_attack();
            status_display_show_status("CSA Attack Start");
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
    glog("Usage: attack -d (deauth) | attack -c (channel switch) | attack -e (EAPOL logoff) | attack -s <password> (SAE flood)\n");
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
    wifi_manager_stop_channel_switch_attack();
    glog("All WiFi attacks stopped...\n");
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
                glog("Error: '%s' is not a valid number.\n", input);
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

            if (token != NULL) {
                glog("Error: too many indices (max 32).\n");
                return;
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
            glog("Error: '%s' is not a valid number.\n", argv[2]);
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

static bool g_dial_cast_all = false;

void discover_task(void *pvParameter) {
    DIALClient client;
    DIALManager manager;

    if (dial_client_init(&client) == ESP_OK) {
        dial_manager_init(&manager, &client);
        explore_network(&manager, g_dial_cast_all);
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

static TaskHandle_t g_ir_universal_send_task = NULL;
static volatile bool g_ir_universal_send_cancel = false;

static TaskHandle_t g_ir_rx_learn_task = NULL;

void wifi_manager_cancel_connect(void);
void wifi_manager_stop_visualizer(void);

#ifdef CONFIG_WITH_ETHERNET
static volatile bool g_eth_scan_cancel = false;
#endif

void handle_stop_flipper(int argc, char **argv) {
#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
    rgb_manager_set_mic_stream_suspended(true);
#endif

#ifdef CONFIG_HAS_MIC
    bool restart_mic_visualizer = mic_visualizer_is_running();
    mic_visualizer_stop();
#endif

    if (g_ir_universal_send_task != NULL) {
        g_ir_universal_send_cancel = true;
    }

    stop_wardriving();
    if (!esp_comm_manager_is_remote_command()) {
        if (esp_comm_manager_is_connected()) {
            bool peer_stop_ok = esp_comm_manager_send_command("startwd", "-s --helper");
            glog(peer_stop_ok
                     ? "Wardrive helper stop sent to peer.\n"
                     : "Wardrive helper stop could not be sent to peer.\n");
        }
        wardriving_set_peer_assist(false);
    }
    wifi_manager_stop_deauth();
    wifi_manager_stop_channel_switch_attack();
    wifi_manager_cancel_connect();
#ifndef CONFIG_IDF_TARGET_ESP32S2
    ble_spam_stop();
    ble_stop_gatt_scan();
    ble_stop();
#endif
    if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
        csv_flush_buffer_to_file();
    }
    csv_file_close();                  // Close any open CSV files
    gps_manager_deinit(&g_gpsManager); // Clean up GPS if active

    // stop aerial operations
    if (aerial_detector_is_scanning()) {
        aerial_detector_stop_scan();
    }
    if (aerial_detector_is_emulating()) {
        aerial_detector_stop_emulation();
    }
    aerial_detector_untrack_device();

    // also stop any in-progress IR RX (ir rx / ir learn)
    infrared_manager_rx_cancel();

    // stop IR dazzler if running
    infrared_manager_dazzler_stop();

    // also stop the gps info display task if it is running
    if (gps_info_task_handle != NULL) {
        vTaskDelete(gps_info_task_handle);
        gps_info_task_handle = NULL;

        // Free the manually allocated stack and TCB
        if (gps_task_stack) {
            heap_caps_free(gps_task_stack);
            gps_task_stack = NULL;
        }
        if (gps_task_tcb) {
            heap_caps_free(gps_task_tcb);
            gps_task_tcb = NULL;
        }
    }

    wifi_manager_stop_monitor_mode();  // Stop any active monitoring
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_deauth();
    dhcp_starvation_stop();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
    wifi_manager_stop_karma();            // stop karma attack (sets flag; task self-exits)
    wifi_manager_stop_evil_portal();  // stop evil portal and flush credentials
    wifi_manager_stop_tracking();  // stop ap/sta rssi tracking
    wifi_manager_stop_beacon();  // stop beacon spam
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    // ensure zigbee capture is stopped when using generic stop
    zigbee_manager_stop_capture();
#endif
#ifdef CONFIG_WITH_ETHERNET
    g_eth_scan_cancel = true;
    if (eth_arp_poison_is_running()) eth_arp_poison_stop();
#endif
    // ensure pcap is properly flushed and closed
    pcap_file_close();
    glog("All activities stopped.\n");
    status_display_show_status("All Stopped");

    // kill any feature tasks we spawned that may still be around
    if (VisualizerHandle != NULL) {
        wifi_manager_stop_visualizer();
    }
    settings_restart_rgb_effect();

#ifdef CONFIG_HAS_MIC
    if (restart_mic_visualizer) {
        mic_visualizer_start();
    }
#endif

#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
    rgb_manager_set_mic_stream_suspended(false);
#endif
}

void handle_dial_command(int argc, char **argv) {
    g_dial_cast_all = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0 || strcmp(argv[i], "-a") == 0) {
            g_dial_cast_all = true;
        } else {
            dial_manager_set_device_name(argv[i]);
        }
    }
    BaseType_t rc = xTaskCreate(&discover_task, "discover_task", DISCOVER_TASK_STACK, NULL, 5, NULL);
    if (rc != pdPASS) {
        glog("Failed to start DIAL discovery task (err=%ld).\n", (long)rc);
    }
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

#ifdef CONFIG_HAS_RTC_CLOCK
// Time synchronization callback for SNTP
static void sntp_time_sync_callback(struct timeval *tv) {
    if (tv && tv->tv_sec > 1600000000) { // Valid time (after Sept 2020)
        struct tm timeinfo;
        struct tm utc_timeinfo;
        
        // Get local time for display
        localtime_r(&tv->tv_sec, &timeinfo);
        
        // Save UTC time to RTC (not local time)
        gmtime_r(&tv->tv_sec, &utc_timeinfo);
        RTC_Date rtc_time;
        rtc_time.year = utc_timeinfo.tm_year + 1900;
        rtc_time.month = utc_timeinfo.tm_mon + 1;
        rtc_time.day = utc_timeinfo.tm_mday;
        rtc_time.hour = utc_timeinfo.tm_hour;
        rtc_time.minute = utc_timeinfo.tm_min;
        rtc_time.second = utc_timeinfo.tm_sec;
        
        if (rtc_set_datetime(&rtc_time) == ESP_OK) {
            ESP_LOGI("SNTP", "Time synchronized from NTP and UTC saved to RTC: %04d-%02d-%02d %02d:%02d:%02d", 
                     rtc_time.year, rtc_time.month, rtc_time.day, 
                     rtc_time.hour, rtc_time.minute, rtc_time.second);
        }
    }
}
#endif

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

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");

#ifdef CONFIG_HAS_RTC_CLOCK
        esp_sntp_set_time_sync_notification_cb(sntp_time_sync_callback);
#endif

        esp_sntp_init();
    }
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
        wifi_manager_stop_visualizer();
    }
}

void handle_wifi_status(int argc, char **argv)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    
    glog("=== WIFI STATUS ===\n");
    
    bool is_connected = is_wifi_sta_connected();
    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    bool has_saved = (saved_ssid != NULL && strlen(saved_ssid) > 0);
    
    glog("connected=%s\n", is_connected ? "true" : "false");
    glog("has_saved_network=%s\n", has_saved ? "true" : "false");
    
    if (is_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            glog("connected_ssid=%s\n", ap_info.ssid);
            glog("connected_rssi=%d\n", ap_info.rssi);
            glog("connected_bssid=" MACSTR "\n", MAC2STR(ap_info.bssid));
            glog("connected_channel=%d\n", ap_info.primary);
        }
    } else {
        glog("connected_ssid=\n");
    }
    
    if (has_saved) {
        glog("saved_ssid=%s\n", saved_ssid);
    } else {
        glog("saved_ssid=\n");
    }
    
    glog("=== END STATUS ===\n");
}

#ifndef CONFIG_IDF_TARGET_ESP32S2

void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        glog("Starting Find the Flippers.\n");
        flipper_scan_start();
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

    if (argc > 1 && strcmp(argv[1], "-g") == 0) {
        glog("Starting GATT Device Scan.\n");
        ble_start_gatt_scan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping BLE Scan.\n");
        ble_stop();
        ble_stop_gatt_scan();
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
    TERMINAL_VIEW_ADD_TEXT("%s", log_buf);
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

#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_anim_to_name(IdleAnimation anim) {
    switch (anim) {
        case IDLE_ANIM_GAME_OF_LIFE: return "life";
        case IDLE_ANIM_GHOST: return "ghost";
        case IDLE_ANIM_STARFIELD: return "starfield";
        case IDLE_ANIM_HUD: return "hud";
        case IDLE_ANIM_MATRIX: return "matrix";
        case IDLE_ANIM_FLYING_GHOSTS: return "ghosts";
        case IDLE_ANIM_SPIRAL: return "spiral";
        case IDLE_ANIM_FALLING_LEAVES: return "leaves";
        case IDLE_ANIM_BOUNCING_TEXT: return "bouncing";
        default: return "unknown";
    }
}

static bool parse_idle_anim_arg(const char *arg, IdleAnimation *out) {
    if (!arg || !out) return false;
    if (strcmp(arg, "0") == 0 || strcmp(arg, "life") == 0 || strcmp(arg, "gameoflife") == 0) {
        *out = IDLE_ANIM_GAME_OF_LIFE;
        return true;
    }
    if (strcmp(arg, "1") == 0 || strcmp(arg, "ghost") == 0) {
        *out = IDLE_ANIM_GHOST;
        return true;
    }
    if (strcmp(arg, "2") == 0 || strcmp(arg, "starfield") == 0 || strcmp(arg, "startfield") == 0) {
        *out = IDLE_ANIM_STARFIELD;
        return true;
    }
    if (strcmp(arg, "3") == 0 || strcmp(arg, "hud") == 0 || strcmp(arg, "stats") == 0) {
        *out = IDLE_ANIM_HUD;
        return true;
    }
    if (strcmp(arg, "4") == 0 || strcmp(arg, "matrix") == 0 || strcmp(arg, "rain") == 0 || strcmp(arg, "code") == 0) {
        *out = IDLE_ANIM_MATRIX;
        return true;
    }
    if (strcmp(arg, "5") == 0 || strcmp(arg, "ghosts") == 0 || strcmp(arg, "flyingghosts") == 0 || strcmp(arg, "flying_ghosts") == 0 || strcmp(arg, "ghoster") == 0 || strcmp(arg, "flyingghoster") == 0 || strcmp(arg, "flying_ghoster") == 0 || strcmp(arg, "toaster") == 0 || strcmp(arg, "flyingtoaster") == 0 || strcmp(arg, "flying_toaster") == 0) {
        *out = IDLE_ANIM_FLYING_GHOSTS;
        return true;
    }
    if (strcmp(arg, "6") == 0 || strcmp(arg, "spiral") == 0 || strcmp(arg, "hypnotic") == 0 || strcmp(arg, "hypnoticspiral") == 0 || strcmp(arg, "hypnotic_spiral") == 0) {
        *out = IDLE_ANIM_SPIRAL;
        return true;
    }
    if (strcmp(arg, "7") == 0 || strcmp(arg, "leaves") == 0 || strcmp(arg, "fallingleaves") == 0 || strcmp(arg, "falling_leaves") == 0) {
        *out = IDLE_ANIM_FALLING_LEAVES;
        return true;
    }
    if (strcmp(arg, "8") == 0 || strcmp(arg, "bouncing") == 0 || strcmp(arg, "bouncingtext") == 0 || strcmp(arg, "bouncing_text") == 0 || strcmp(arg, "dvd") == 0 || strcmp(arg, "dvdplayer") == 0) {
        *out = IDLE_ANIM_BOUNCING_TEXT;
        return true;
    }
    return false;
}

void handle_status_idle_cmd(int argc, char **argv) {
    if (!status_display_is_ready()) {
        glog("Status display not ready; check wiring and CONFIG_WITH_STATUS_DISPLAY.\n");
        return;
    }

    if (argc < 2) {
        IdleAnimation current = settings_get_status_idle_animation(&G_Settings);
        uint32_t timeout = settings_get_status_idle_timeout_ms(&G_Settings);
        const char *name = idle_anim_to_name(current);
        const char *timeout_desc = (timeout == 0 || timeout == UINT32_MAX) ? "never" : "delayed";
        glog("Current idle animation: %s (%d)\n", name, (int)current);
        glog("Idle timeout: %lu ms (%s)\n", (unsigned long)timeout, timeout_desc);
        status_display_show_status("Idle Anim Info");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        glog("Available idle animations:\n");
        glog("  0 - life      (Game of Life)\n");
        glog("  1 - ghost      (Ghost sprite)\n");
        glog("  2 - starfield  (Starfield effect)\n");
        glog("  3 - hud        (System HUD)\n");
        glog("  4 - matrix     (Matrix code rain)\n");
        glog("  5 - ghosts     (Flying Ghosts)\n");
        glog("  6 - spiral    (Hypnotic Spiral)\n");
        glog("  7 - leaves    (Falling Leaves)\n");
        glog("  8 - bouncing  (Bouncing Text)\n");
        status_display_show_status("Idle Anim List");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            glog("Usage: statusidle set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>\n");
            return;
        }
        IdleAnimation anim;
        if (!parse_idle_anim_arg(argv[2], &anim)) {
            glog("Unknown idle animation: %s\n", argv[2]);
            glog("Use 'statusidle list' to see options.\n");
            return;
        }
        settings_set_status_idle_animation(&G_Settings, anim);
        settings_save(&G_Settings);
        glog("Idle animation set to %s (%d)\n", idle_anim_to_name(anim), (int)anim);
        status_display_show_status("Idle Anim Set");
        return;
    }

    glog("Usage: statusidle [list|set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>]\n");
}

#endif

void handle_capture_scan(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
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
    
    // Parse channel parameter if present
    uint8_t fixed_channel = 0;
    bool use_fixed_channel = false;
    
    if (argc >= 4 && strcmp(argv[2], "-c") == 0) {
        fixed_channel = atoi(argv[3]);
        use_fixed_channel = true;
        
        if (fixed_channel < 1 || fixed_channel > MAX_WIFI_CHANNEL) {
            glog("Error: Invalid channel %d. Must be between 1 and %d\n", fixed_channel, MAX_WIFI_CHANNEL);
            status_display_show_status("Invalid Channel");
            return;
        }
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

    if (strcmp(capturetype, "-wireshark") == 0) {
        status_display_show_status("Wireshark WiFi");
        int err = pcap_wireshark_start(PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            status_display_show_status("Wireshark Err");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
        
        if (use_fixed_channel) {
            err = wifi_manager_set_wireshark_fixed_channel(fixed_channel);
            if (err != ESP_OK) {
                glog("Error: Failed to set fixed channel %d\n", fixed_channel);
                status_display_show_status("Channel Err");
                return;
            }
            glog("Wireshark capture locked to channel %d\n", fixed_channel);
        } else {
            wifi_manager_start_wireshark_channel_hop();
        }
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-wiresharkble") == 0) {
        status_display_show_status("Wireshark BLE");
        int err = pcap_wireshark_start(PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            status_display_show_status("Wireshark Err");
            return;
        }
        ble_start_capture_wireshark();
    }
#endif

    if (strcmp(capturetype, "-stop") == 0) {
        glog("Stopping packet capture...\n");
        wifi_manager_stop_wireshark_channel_hop();
        wifi_manager_stop_monitor_mode();
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        zigbee_manager_stop_capture();
#endif
        pcap_file_close();
        pcap_wireshark_stop();
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

#ifdef CONFIG_WITH_ETHERNET

void handle_eth_up_cmd(int argc, char **argv) {
    glog("Bringing up Ethernet Manager...\n");
    esp_err_t ret = ethernet_manager_init();
    if (ret == ESP_OK) {
        glog("Ethernet Manager initialized successfully\n");
        
        // Wait a moment for link to establish and DHCP to complete
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Check connection status
        if (ethernet_manager_is_connected()) {
            glog("Ethernet link is UP\n");
            
            // Get and display IP info
            esp_netif_ip_info_t ip_info;
            if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
                char ip_str[16], netmask_str[16], gw_str[16];
                ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
                ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
                ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
                
                // Check if IP is actually assigned (not 0.0.0.0)
                if (ip_info.ip.addr == 0) {
                    glog("IP Address: Not assigned yet (waiting for DHCP...)\n");
                    glog("Netmask: Not assigned\n");
                    glog("Gateway: Not assigned\n");
                    glog("Note: DHCP may take a few more seconds. Check again shortly.\n");
                } else {
                    glog("IP Address: %s\n", ip_str);
                    glog("Netmask: %s\n", netmask_str);
                    glog("Gateway: %s\n", gw_str);
                    
                    // Get and display DNS server information and DHCP server
                    esp_netif_t *netif = ethernet_manager_get_netif();
                    if (netif != NULL) {
                        esp_netif_dns_info_t dns_main, dns_backup, dns_fallback;
                        char dns_str[16];
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
                            if (dns_main.ip.type == ESP_IPADDR_TYPE_V4 && dns_main.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_main.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Main: %s\n", dns_str);
                            }
                        }
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
                            if (dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_backup.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Backup: %s\n", dns_str);
                            }
                        }
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback) == ESP_OK) {
                            if (dns_fallback.ip.type == ESP_IPADDR_TYPE_V4 && dns_fallback.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_fallback.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Fallback: %s\n", dns_str);
                            }
                        }
                        
                        // Get DHCP server IP address
                        ip4_addr_t dhcp_server_ip;
                        if (ethernet_manager_get_dhcp_server_ip(&dhcp_server_ip) == ESP_OK) {
                            ip4addr_ntoa_r(&dhcp_server_ip, dns_str, sizeof(dns_str));
                            glog("DHCP Server: %s\n", dns_str);
                        }
                    }
                }
            } else {
                glog("Failed to get IP information\n");
            }
        } else {
            glog("Ethernet link is DOWN - waiting for cable connection...\n");
            glog("Please connect an Ethernet cable to the W5500 module\n");
        }
    } else {
        glog("Ethernet Manager initialization failed: %s\n", esp_err_to_name(ret));
    }
}

void handle_eth_down_cmd(int argc, char **argv) {
    glog("Bringing down Ethernet Manager...\n");
    esp_err_t ret = ethernet_manager_deinit();
    if (ret == ESP_OK) {
        glog("Ethernet Manager deinitialized successfully\n");
    } else {
        glog("Ethernet Manager deinitialization failed: %s\n", esp_err_to_name(ret));
    }
}

// Helper function to ensure Ethernet interface is initialized
// Returns true if interface is ready, false otherwise
static bool ensure_eth_interface_up(void) {
    return eth_ensure_interface_up();
}

void handle_eth_fingerprint_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected\n");
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        glog("No IP address assigned\n");
        return;
    }
    eth_fingerprint_run_scan();
}

void handle_eth_info_cmd(int argc, char **argv) {
    glog("Ethernet Information\n");
    glog("===================\n");
    
    // Check connection status
    if (!ethernet_manager_is_connected()) {
        glog("Status: DOWN\n");
        glog("Ethernet link is not established\n");
        return;
    }
    
    glog("Status: UP\n");

    ethernet_link_info_t link_info;
    if (ethernet_manager_get_link_info(&link_info) == ESP_OK && link_info.link_up) {
        glog("Link: %dMbps %s\n", link_info.speed_mbps, link_info.full_duplex ? "Full Duplex" : "Half Duplex");
    }

    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif != NULL) {
        uint8_t mac[6];
        if (esp_netif_get_mac(eth_netif, mac) == ESP_OK) {
            glog("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    
    // Get and display IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        char ip_str[16], netmask_str[16], gw_str[16];
        ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
        ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
        ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
        
        // Check if IP is actually assigned (not 0.0.0.0)
        if (ip_info.ip.addr == 0) {
            glog("IP Address: Not assigned yet (waiting for DHCP...)\n");
            glog("Netmask: Not assigned\n");
            glog("Gateway: Not assigned\n");
        } else {
            glog("IP Address: %s\n", ip_str);
            glog("Netmask: %s\n", netmask_str);
            glog("Gateway: %s\n", gw_str);
            
            // Get and display DNS server information and DHCP server
            if (eth_netif != NULL) {
                esp_netif_dns_info_t dns_main, dns_backup, dns_fallback;
                char dns_str[16];
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
                    if (dns_main.ip.type == ESP_IPADDR_TYPE_V4 && dns_main.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_main.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Main: %s\n", dns_str);
                    } else {
                        glog("DNS Main: Not assigned\n");
                    }
                }
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
                    if (dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_backup.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Backup: %s\n", dns_str);
                    } else {
                        glog("DNS Backup: Not assigned\n");
                    }
                }
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback) == ESP_OK) {
                    if (dns_fallback.ip.type == ESP_IPADDR_TYPE_V4 && dns_fallback.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_fallback.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Fallback: %s\n", dns_str);
                    }
                }
                
                // Get DHCP server IP address
                ip4_addr_t dhcp_server_ip;
                if (ethernet_manager_get_dhcp_server_ip(&dhcp_server_ip) == ESP_OK) {
                    ip4addr_ntoa_r(&dhcp_server_ip, dns_str, sizeof(dns_str));
                    glog("DHCP Server: %s\n", dns_str);
                } else {
                    glog("DHCP Server: Not available\n");
                }
            }
        }
    } else {
        glog("Failed to get IP information\n");
    }
    
    glog("===================\n");
}

void handle_eth_arp_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    // Check if Ethernet is connected
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    // Get Ethernet IP info to determine subnet
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK) {
        glog("Failed to get Ethernet IP information\n");
        return;
    }

    if (ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    // Get Ethernet netif
    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    // Get underlying LWIP netif
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
    if (lwip_netif == NULL) {
        glog("Failed to get LWIP netif\n");
        return;
    }

    // Calculate subnet prefix (e.g., "192.168.1.")
    char subnet_prefix[16];
    uint32_t ip = ip_info.ip.addr;
    uint32_t netmask = ip_info.netmask.addr;
    uint32_t network = ip & netmask;
    
    snprintf(subnet_prefix, sizeof(subnet_prefix), "%d.%d.%d.",
             (int)((network >> 0) & 0xFF),
             (int)((network >> 8) & 0xFF),
             (int)((network >> 16) & 0xFF));

    g_eth_scan_cancel = false;
    glog("Starting ARP scan on Ethernet network %s0/24\n", subnet_prefix);
    glog("Scanning network using ARP requests...\n");

    const int START_HOST = 1;
    const int END_HOST = 254;
    const int batch_size = 10;
    int num_found = 0;

    // Scan the subnet in batches
    for (int batch_start = START_HOST; batch_start <= END_HOST && !g_eth_scan_cancel; batch_start += batch_size) {
        int batch_end = (batch_start + batch_size - 1 > END_HOST) ? END_HOST : batch_start + batch_size - 1;
        
        // Send batch of ARP requests
        for (int host = batch_start; host <= batch_end && !g_eth_scan_cancel; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", subnet_prefix, host);
            
            // Parse IP address
            ip4_addr_t target_addr;
            if (ip4addr_aton(current_ip, &target_addr)) {
                // Send ARP request using lwIP
                etharp_request(lwip_netif, &target_addr);
            }
            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between requests
        }
        if (g_eth_scan_cancel) break;
        
        // Wait for responses to arrive
        for (int i = 0; i < 5 && !g_eth_scan_cancel; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        // Check ARP table for this batch
        for (int host = batch_start; host <= batch_end && !g_eth_scan_cancel; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", subnet_prefix, host);
            
            // Parse IP address
            ip4_addr_t target_addr;
            if (!ip4addr_aton(current_ip, &target_addr)) {
                continue;
            }
            
            // Search ARP table
            struct eth_addr *eth_ret = NULL;
            const ip4_addr_t *ip_ret = NULL;
            s8_t arp_idx = etharp_find_addr(lwip_netif, &target_addr, &eth_ret, &ip_ret);
            
            if (arp_idx >= 0 && eth_ret) {
                num_found++;
                glog("  %-15s %02x:%02x:%02x:%02x:%02x:%02x\n",
                     current_ip,
                     eth_ret->addr[0], eth_ret->addr[1], eth_ret->addr[2],
                     eth_ret->addr[3], eth_ret->addr[4], eth_ret->addr[5]);
            }
        }
        
        // Progress update
        if (batch_end % 50 == 0 || batch_end == END_HOST) {
            glog("Progress: Scanned %d/%d hosts, found %d active hosts\n", 
                 batch_end - START_HOST + 1, END_HOST - START_HOST + 1, num_found);
        }
    }

    glog("\n=== ARP Scan Summary ===\n");
    glog("Network: %s0/24\n", subnet_prefix);
    glog("Hosts scanned: %d (1-254)\n", END_HOST - START_HOST + 1);
    glog("Active hosts found: %d\n", num_found);
    if (num_found > 0) {
        glog("Success rate: %.1f%%\n", (float)num_found / (END_HOST - START_HOST + 1) * 100.0f);
    }
    glog("=======================\n");
}

void handle_eth_ports_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    // Check if Ethernet is connected
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    // Get Ethernet IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK) {
        glog("Failed to get Ethernet IP information\n");
        return;
    }

    if (ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    char target_ip[16];
    uint16_t start_port = 1;
    uint16_t end_port = 1024;
    bool scan_all = false;

    // Parse arguments
    if (argc < 2) {
        // Default: scan local network gateway
        ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        if (ip_info.gw.addr == 0) {
            glog("No gateway configured. Usage: ethports <IP> [all | start-end]\n");
            return;
        }
        // Default to common ports when no arguments
        start_port = 1;
        end_port = 1024;
    } else if (strcmp(argv[1], "local") == 0) {
        // Scan local network (gateway)
        ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        if (ip_info.gw.addr == 0) {
            glog("No gateway configured.\n");
            return;
        }
        // Check if "all" is specified after "local"
        if (argc >= 3 && strcmp(argv[2], "all") == 0) {
            scan_all = true;
            start_port = 1;
            end_port = 65535;
        }
    } else {
        strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
        target_ip[sizeof(target_ip) - 1] = '\0';

        if (argc >= 3) {
            if (strcmp(argv[2], "all") == 0) {
                scan_all = true;
                start_port = 1;
                end_port = 65535;
            } else {
                // Parse range like "80-443"
                if (sscanf(argv[2], "%hu-%hu", &start_port, &end_port) != 2) {
                    glog("Invalid port range. Use format: start-end (e.g., 80-443)\n");
                    return;
                }
                if (start_port > end_port || start_port == 0 || end_port > 65535) {
                    glog("Invalid port range. Ports must be 1-65535 and start <= end.\n");
                    return;
                }
            }
        }
    }

    g_eth_scan_cancel = false;
    glog("Scanning TCP ports on %s\n", target_ip);
    if (scan_all) {
        glog("Scanning all ports (1-65535)...\n");
    } else {
        glog("Scanning ports %d-%d...\n", start_port, end_port);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1) {
        glog("Invalid IP address: %s\n", target_ip);
        return;
    }

    // Common ports to scan if no range specified
    static const uint16_t COMMON_PORTS[] = {
        21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080
    };
    const size_t NUM_COMMON_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);

    int ports_scanned = 0;
    int open_ports = 0;
    uint32_t total_ports = scan_all ? (end_port - start_port + 1) : 
                          (argc >= 3 ? (end_port - start_port + 1) : NUM_COMMON_PORTS);

    if (scan_all || (argc >= 3 && !scan_all)) {
        // Scan port range
        // Use uint32_t for port to handle full range including 65535
        for (uint32_t port = start_port; port <= end_port && port <= 65535 && !g_eth_scan_cancel; port++) {
            ports_scanned++;
            if (ports_scanned % 100 == 0) {
                glog("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned, total_ports,
                     (float)ports_scanned / total_ports * 100);
            }

            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock < 0) {
                continue;
            }

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            server_addr.sin_port = htons(port);
            int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

            bool connected = false;
            if (scan_result == 0) {
                connected = true;
            } else if (scan_result < 0 && errno == EINPROGRESS) {
                uint32_t elapsed = 0;
                const uint32_t interval = 50;
                while (elapsed < 500 && !g_eth_scan_cancel) {
                    struct timeval tv = { .tv_sec = 0, .tv_usec = interval * 1000 };
                    fd_set fdset;
                    FD_ZERO(&fdset);
                    FD_SET(sock, &fdset);
                    if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                            connected = true;
                        }
                        break;
                    }
                    elapsed += interval;
                }
            }
            if (connected) {
                open_ports++;
                glog("  %s:%lu - OPEN\n", target_ip, (unsigned long)port);
            }
            close(sock);
            if (g_eth_scan_cancel) break;
        }
    } else {
        // Scan common ports
        for (size_t i = 0; i < NUM_COMMON_PORTS && !g_eth_scan_cancel; i++) {
            uint16_t port = COMMON_PORTS[i];
            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock < 0) {
                continue;
            }

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            server_addr.sin_port = htons(port);
            int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

            bool connected = false;
            if (scan_result == 0) {
                connected = true;
            } else if (scan_result < 0 && errno == EINPROGRESS) {
                uint32_t elapsed = 0;
                const uint32_t interval = 50;
                while (elapsed < 500 && !g_eth_scan_cancel) {
                    struct timeval tv = { .tv_sec = 0, .tv_usec = interval * 1000 };
                    fd_set fdset;
                    FD_ZERO(&fdset);
                    FD_SET(sock, &fdset);

                    if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                            connected = true;
                        }
                        break;
                    }
                    elapsed += interval;
                }
            }
            if (connected) {
                open_ports++;
                glog("  %s:%d - OPEN\n", target_ip, port);
            }
            close(sock);
            if (g_eth_scan_cancel) break;
        }
    }

    glog("\n=== Port Scan Summary ===\n");
    glog("Target: %s\n", target_ip);
    if (scan_all) {
        glog("Port range: 1-65535 (all ports)\n");
    } else if (argc >= 3) {
        glog("Port range: %d-%d\n", start_port, end_port);
    } else {
        glog("Ports scanned: %zu common ports\n", NUM_COMMON_PORTS);
    }
    glog("Total ports scanned: %lu\n", (unsigned long)total_ports);
    glog("Open ports found: %d\n", open_ports);
    if (total_ports > 0) {
        glog("Open port rate: %.1f%%\n", (float)open_ports / total_ports * 100.0f);
    }
    glog("========================\n");
}

void handle_eth_ping_cmd(int argc, char **argv) {
    if (!ensure_eth_interface_up()) {
        return;
    }

    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    const uint32_t ip_host = ntohl(ip_info.ip.addr);
    const uint32_t netmask_host = ntohl(ip_info.netmask.addr);
    const uint32_t network_host = ip_host & netmask_host;
    const uint32_t base_host = network_host & 0xFFFFFF00;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        glog("Failed to create raw socket: %d\n", errno);
        return;
    }

    struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    typedef struct {
        uint8_t type;
        uint8_t code;
        uint16_t checksum;
        uint16_t id;
        uint16_t seq;
    } icmp_hdr_t;

    g_eth_scan_cancel = false;
    glog("Starting ICMP ping scan on local /24...\n");

    int alive = 0;
    for (int host = 1; host <= 254 && !g_eth_scan_cancel; host++) {
        const uint32_t target_host = base_host | (uint32_t)host;
        if (target_host == ip_host) continue;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(target_host);

        icmp_hdr_t icmp = {0};
        icmp.type = 8;
        icmp.code = 0;
        icmp.id = 0xBEEF;
        icmp.seq = htons((uint16_t)host);

        uint16_t *buf = (uint16_t *)&icmp;
        uint32_t sum = 0;
        for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
            sum += buf[i];
        }
        sum = (sum >> 16) + (sum & 0xFFFF);
        sum += (sum >> 16);
        icmp.checksum = ~sum;

        sendto(sock, &icmp, sizeof(icmp), 0, (struct sockaddr *)&addr, sizeof(addr));

        uint8_t recv_buf[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&from, &fromlen);
        if (r > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr) {
            char ip_str[16];
            ip4_addr_t ip4;
            ip4.addr = from.sin_addr.s_addr;
            ip4addr_ntoa_r(&ip4, ip_str, sizeof(ip_str));
            glog("  %s - ALIVE\n", ip_str);
            alive++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("Ping scan complete. Alive hosts: %d\n", alive);
    close(sock);
}

// ethdns - DNS lookup/resolution
void handle_eth_dns_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethdns <hostname> [reverse]\n");
        glog("  hostname: Domain name to resolve\n");
        glog("  reverse:  IP address for reverse DNS lookup\n");
        glog("Example: ethdns google.com\n");
        glog("Example: ethdns reverse 8.8.8.8\n");
        return;
    }

    if (strcmp(argv[1], "reverse") == 0) {
        // Reverse DNS lookup
        if (argc < 3) {
            glog("Usage: ethdns reverse <ip_address>\n");
            return;
        }

        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        if (inet_pton(AF_INET, argv[2], &sa.sin_addr) != 1) {
            glog("Invalid IP address: %s\n", argv[2]);
            return;
        }

        // Note: Reverse DNS may not be fully supported in lwIP
        // This is a simplified implementation
        glog("Reverse DNS lookup for %s:\n", argv[2]);
        glog("  Note: Reverse DNS (PTR records) may not be fully supported.\n");
        glog("  Use forward DNS lookup (ethdns <hostname>) instead.\n");
    } else {
        // Forward DNS lookup
        const char *hostname = argv[1];
        glog("Resolving %s...\n", hostname);

        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *result = NULL;
        int ret = getaddrinfo(hostname, NULL, &hints, &result);
        if (ret != 0) {
            const char *err_msg = "Unknown error";
            switch (ret) {
                case EAI_NONAME: err_msg = "Name does not resolve"; break;
                case EAI_FAIL: err_msg = "Non-recoverable failure"; break;
                case EAI_MEMORY: err_msg = "Memory allocation failure"; break;
                default: err_msg = "DNS lookup failed"; break;
            }
            glog("DNS lookup failed: %s\n", err_msg);
            return;
        }

        glog("DNS resolution for %s:\n", hostname);
        int count = 0;
        for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                glog("  IP: %s\n", ip_str);
                count++;
            }
        }

        if (count == 0) {
            glog("  No IPv4 addresses found\n");
        }

        freeaddrinfo(result);
    }
}

// ethtrace - Traceroute
void handle_eth_trace_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethtrace <hostname_or_ip> [max_hops]\n");
        glog("Example: ethtrace 8.8.8.8\n");
        glog("Example: ethtrace google.com 30\n");
        return;
    }

    const char *target = argv[1];
    int max_hops = (argc >= 3) ? atoi(argv[2]) : 30;
    if (max_hops < 1 || max_hops > 64) {
        glog("Max hops must be between 1 and 64\n");
        return;
    }

    // Resolve hostname if needed
    struct sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, target, &target_addr.sin_addr) != 1) {
        // Try DNS lookup
        struct hostent *he = gethostbyname(target);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            glog("Failed to resolve %s\n", target);
            return;
        }
        memcpy(&target_addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
        char resolved_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &target_addr.sin_addr, resolved_ip, sizeof(resolved_ip));
        glog("Tracing route to %s (%s) [max %d hops]:\n", target, resolved_ip, max_hops);
    } else {
        glog("Tracing route to %s [max %d hops]:\n", target, max_hops);
    }

    // Traceroute using ICMP with increasing TTL
    for (int ttl = 1; ttl <= max_hops; ttl++) {
        int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            glog("Failed to create raw socket: %d\n", errno);
            return;
        }

        // Set TTL
        if (setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            close(sock);
            continue;
        }

        // Set timeout
        struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        typedef struct {
            uint8_t type;
            uint8_t code;
            uint16_t checksum;
            uint16_t id;
            uint16_t seq;
        } icmp_hdr_t;

        icmp_hdr_t icmp = {0};
        icmp.type = 8; // Echo request
        icmp.code = 0;
        icmp.id = 0xAFAF;
        icmp.seq = htons(ttl);

        // Calculate checksum
        uint16_t *buf = (uint16_t *)&icmp;
        uint32_t sum = 0;
        for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
            sum += buf[i];
        }
        sum = (sum >> 16) + (sum & 0xFFFF);
        sum += (sum >> 16);
        icmp.checksum = ~sum;

        struct timeval start, end;
        gettimeofday(&start, NULL);

        sendto(sock, &icmp, sizeof(icmp), 0, (struct sockaddr *)&target_addr, sizeof(target_addr));

        // Wait for response
        char recv_buf[1024];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t recv_len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, 
                                     (struct sockaddr *)&from_addr, &from_len);

        gettimeofday(&end, NULL);
        long elapsed = ((end.tv_sec - start.tv_sec) * 1000) + 
                       ((end.tv_usec - start.tv_usec) / 1000);

        close(sock);

        if (recv_len > 0) {
            char from_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
            glog("  %2d  %s  %ldms\n", ttl, from_ip, elapsed);

            // Check if we reached the target
            if (from_addr.sin_addr.s_addr == target_addr.sin_addr.s_addr) {
                glog("Trace complete.\n");
                break;
            }
        } else {
            glog("  %2d  *  (timeout)\n", ttl);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void handle_eth_stats_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(netif);
    if (lwip_netif == NULL) {
        glog("Failed to get LWIP netif\n");
        return;
    }

    glog("=== Ethernet Statistics ===\n");

    // Link status
    glog("Link Status: %s\n", ethernet_manager_is_connected() ? "UP" : "DOWN");

    // IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        char ip_str[16], netmask_str[16], gw_str[16];
        ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
        ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
        ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
        glog("IP Address: %s\n", ip_str);
        glog("Netmask: %s\n", netmask_str);
        glog("Gateway: %s\n", gw_str);
    }

    // MAC address
    uint8_t mac[6];
    if (esp_netif_get_mac(netif, mac) == ESP_OK) {
        glog("MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // LWIP statistics (using global stats)
    glog("\n--- Packet Statistics ---\n");
#if LWIP_STATS
    extern struct stats_ lwip_stats;
    glog("RX Packets: %lu\n", (unsigned long)STATS_GET(link.recv));
    glog("TX Packets: %lu\n", (unsigned long)STATS_GET(link.xmit));
    glog("RX Errors: %lu\n", (unsigned long)STATS_GET(link.err));
    glog("RX Drops: %lu\n", (unsigned long)STATS_GET(link.drop));
    glog("TX Errors: %lu\n", (unsigned long)STATS_GET(link.chkerr));
    glog("TX Drops: %lu\n", (unsigned long)STATS_GET(link.memerr));
#else
    glog("Statistics not available (LWIP_STATS disabled)\n");
#endif

    // ARP statistics
    glog("\n--- ARP Statistics ---\n");
#if LWIP_STATS && ETHARP_STATS
    glog("ARP Requests: %lu\n", (unsigned long)STATS_GET(etharp.xmit));
    glog("ARP Replies: %lu\n", (unsigned long)STATS_GET(etharp.recv));
#endif

    glog("===================\n");
}

// ethconfig - Static IP configuration
void handle_eth_config_cmd(int argc, char **argv) {
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Ethernet interface is not initialized. Please run 'ethup' first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethconfig <command>\n");
        glog("Commands:\n");
        glog("  dhcp          - Use DHCP (automatic IP)\n");
        glog("  static <ip> <netmask> <gateway> - Set static IP\n");
        glog("  show          - Show current configuration\n");
        return;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        esp_err_t ret = esp_netif_dhcpc_start(netif);
        if (ret == ESP_OK) {
            glog("DHCP client started. Waiting for IP assignment...\n");
        } else {
            glog("Failed to start DHCP client: %s\n", esp_err_to_name(ret));
        }
    } else if (strcmp(argv[1], "static") == 0) {
        if (argc < 5) {
            glog("Usage: ethconfig static <ip> <netmask> <gateway>\n");
            glog("Example: ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1\n");
            return;
        }

        esp_netif_ip_info_t ip_info;
        if (inet_aton(argv[2], &ip_info.ip) == 0 ||
            inet_aton(argv[3], &ip_info.netmask) == 0 ||
            inet_aton(argv[4], &ip_info.gw) == 0) {
            glog("Invalid IP address format\n");
            return;
        }

        esp_err_t ret = esp_netif_dhcpc_stop(netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            glog("Failed to stop DHCP: %s\n", esp_err_to_name(ret));
            return;
        }

        ret = esp_netif_set_ip_info(netif, &ip_info);
        if (ret == ESP_OK) {
            glog("Static IP configured:\n");
            char ip_str[16], netmask_str[16], gw_str[16];
            ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
            ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
            ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
            glog("  IP: %s\n", ip_str);
            glog("  Netmask: %s\n", netmask_str);
            glog("  Gateway: %s\n", gw_str);
        } else {
            glog("Failed to set static IP: %s\n", esp_err_to_name(ret));
            esp_err_t restart_ret = esp_netif_dhcpc_start(netif);
            if (restart_ret == ESP_OK) {
                glog("DHCP client restarted to restore connectivity.\n");
            } else {
                glog("Failed to restart DHCP client: %s\n", esp_err_to_name(restart_ret));
            }
        }
    } else if (strcmp(argv[1], "show") == 0) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16], netmask_str[16], gw_str[16];
            ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
            ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
            ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
            glog("Current IP Configuration:\n");
            glog("  IP: %s\n", ip_str);
            glog("  Netmask: %s\n", netmask_str);
            glog("  Gateway: %s\n", gw_str);
        }
    } else {
        glog("Unsupported command: %s\n", argv[1]);
    }
}

// ethmac - MAC address management
void handle_eth_mac_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    if (argc < 2) {
        // Show current MAC
        uint8_t mac[6];
        if (esp_netif_get_mac(netif, mac) == ESP_OK) {
            glog("Current MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            glog("Failed to get MAC address\n");
        }
        glog("\nUsage: ethmac set <xx:xx:xx:xx:xx:xx>\n");
        glog("Note: MAC address changes require reinitialization\n");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            glog("Usage: ethmac set <xx:xx:xx:xx:xx:xx>\n");
            glog("Example: ethmac set 02:00:00:00:00:01\n");
            return;
        }

        uint8_t new_mac[6];
        if (sscanf(argv[2], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                   &new_mac[0], &new_mac[1], &new_mac[2],
                   &new_mac[3], &new_mac[4], &new_mac[5]) != 6) {
            glog("Invalid MAC address format. Use xx:xx:xx:xx:xx:xx\n");
            return;
        }

        esp_err_t ret = esp_netif_set_mac(netif, new_mac);
        if (ret == ESP_OK) {
            glog("MAC address set to: %02x:%02x:%02x:%02x:%02x:%02x\n",
                 new_mac[0], new_mac[1], new_mac[2], new_mac[3], new_mac[4], new_mac[5]);
            glog("Note: You may need to restart Ethernet for changes to take effect.\n");
        } else {
            glog("Failed to set MAC address: %s\n", esp_err_to_name(ret));
        }
    } else {
        glog("Unsupported command: %s\n", argv[1]);
    }
}

// ethserv - Service discovery and banner grabbing
void handle_eth_serv_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    char target_ip[16];
    if (argc < 2) {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        } else {
            glog("Usage: ethserv <ip_address>\n");
            return;
        }
    } else {
        strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
        target_ip[sizeof(target_ip) - 1] = '\0';
    }

    glog("Service discovery for %s\n", target_ip);
    glog("==========================================\n\n");

    // Common services to check
    struct {
        uint16_t port;
        const char *name;
        const char *probe;
    } services[] = {
        {21, "FTP", "USER anonymous\r\n"},
        {22, "SSH", ""},
        {23, "Telnet", ""},
        {25, "SMTP", "EHLO test\r\n"},
        {80, "HTTP", "GET / HTTP/1.0\r\n\r\n"},
        {110, "POP3", "USER test\r\n"},
        {143, "IMAP", "a001 LOGIN test test\r\n"},
        {443, "HTTPS", ""},
        {445, "SMB", ""},
        {3306, "MySQL", ""},
        {3389, "RDP", ""},
        {5432, "PostgreSQL", ""},
        {8080, "HTTP-Proxy", "GET / HTTP/1.0\r\n\r\n"},
    };

    const size_t num_services = sizeof(services) / sizeof(services[0]);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1) {
        glog("Invalid IP address: %s\n", target_ip);
        return;
    }

    int found_count = 0;
    for (size_t i = 0; i < num_services; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(services[i].port);
        int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    glog("[OPEN] %s:%d (%s)\n", target_ip, services[i].port, services[i].name);
                    found_count++;

                    // Try banner grabbing for some services
                    if (strlen(services[i].probe) > 0) {
                        send(sock, services[i].probe, strlen(services[i].probe), 0);
                        vTaskDelay(pdMS_TO_TICKS(100));

                        char banner[256];
                        ssize_t recv_len = recv(sock, banner, sizeof(banner) - 1, MSG_DONTWAIT);
                        if (recv_len > 0) {
                            banner[recv_len] = '\0';
                            // Clean up banner (remove newlines, limit length)
                            for (int j = 0; j < recv_len && j < 200; j++) {
                                if (banner[j] == '\n' || banner[j] == '\r') {
                                    banner[j] = ' ';
                                }
                            }
                            banner[200] = '\0';
                            glog("      Banner: %.200s\n", banner);
                        }
                    }
                }
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("\nFound %d open service(s)\n", found_count);
}

// ethntp - Query NTP server and set system time
void handle_eth_ntp_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        status_display_show_status("Eth Not Connected");
        return;
    }

    const char *ntp_server = "pool.ntp.org";
    if (argc >= 2) {
        ntp_server = argv[1];
    }

    glog("Querying NTP server: %s\n", ntp_server);
    glog("Please wait...\n");

    // Initialize SNTP with the specified server
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        glog("Failed to initialize SNTP: %s\n", esp_err_to_name(ret));
        status_display_show_status("NTP Init Fail");
        return;
    }

    // Wait for time synchronization (with timeout)
    const int timeout_ms = 10000; // 10 seconds
    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    
    if (ret != ESP_OK) {
        glog("Failed to synchronize time within %d seconds\n", timeout_ms / 1000);
        glog("Error: %s\n", esp_err_to_name(ret));
        esp_netif_sntp_deinit();
        status_display_show_status("NTP Sync Fail");
        return;
    }

    // Get the synchronized time
    time_t now;
    struct tm timeinfo;
    struct tm utc_timeinfo;
    char strftime_buf[64];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    glog("Time synchronized successfully!\n");
    glog("Current system time: %s\n", strftime_buf);
    
#ifdef CONFIG_HAS_RTC_CLOCK
    // Save UTC time to RTC (not local time)
    gmtime_r(&now, &utc_timeinfo);
    RTC_Date rtc_time;
    rtc_time.year = utc_timeinfo.tm_year + 1900;
    rtc_time.month = utc_timeinfo.tm_mon + 1;
    rtc_time.day = utc_timeinfo.tm_mday;
    rtc_time.hour = utc_timeinfo.tm_hour;
    rtc_time.minute = utc_timeinfo.tm_min;
    rtc_time.second = utc_timeinfo.tm_sec;
    
    if (rtc_set_datetime(&rtc_time) == ESP_OK) {
        glog("UTC time saved to RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
             rtc_time.year, rtc_time.month, rtc_time.day, 
             rtc_time.hour, rtc_time.minute, rtc_time.second);
    } else {
        glog("Failed to save time to RTC\n");
    }
#endif
    
    // Also show UTC time
    struct tm timeinfo_utc;
    gmtime_r(&now, &timeinfo_utc);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo_utc);
    glog("UTC time: %s\n", strftime_buf);
    
    // Clean up SNTP
    esp_netif_sntp_deinit();
    
    // If dual comm is connected, sync the peer's time using our newly synced time
    if (esp_comm_manager_is_connected()) {
        glog("Dual comm connected. Setting peer time from local clock...\n");
        // Get current time as timestamp
        struct timeval tv;
        gettimeofday(&tv, NULL);
        
        // Send timestamp to peer as a string
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%ld", (long)tv.tv_sec);
        
        if (esp_comm_manager_send_command("settime", time_str)) {
            glog("Time sync command sent to peer (timestamp: %s)\n", time_str);
        } else {
            glog("Failed to send time sync command to peer\n");
        }
    }
    
    status_display_show_status("NTP Sync OK");
}

// ethhttp - Send HTTP GET request and display response
void handle_eth_http_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        status_display_show_status("Eth Not Connected");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethhttp <url> [lines|all]\n");
        glog("Example: ethhttp http://example.com\n");
        glog("         ethhttp http://example.com 50  (show first 50 lines)\n");
        glog("         ethhttp http://example.com all  (show full response)\n");
        status_display_show_status("HTTP Usage");
        return;
    }

    // Parse optional line limit - default is 25 lines
    int max_lines = 25;  // Default to 25 lines
    if (argc >= 3) {
        if (strcasecmp(argv[2], "all") == 0) {
            max_lines = 0;  // 0 means show all
        } else {
            max_lines = atoi(argv[2]);
            if (max_lines <= 0) {
                glog("Invalid line count. Use a positive number or 'all' for full response.\n");
                status_display_show_status("HTTP Usage");
                return;
            }
        }
    }

    const char *url = argv[1];
    glog("Sending HTTP GET request to: %s\n", url);

    if (strncmp(url, "https://", 8) != 0) {
        char http_url[256];
        const char *http_url_ptr = url;
        if (strncmp(url, "http://", 7) != 0) {
            int n = snprintf(http_url, sizeof(http_url), "http://%s", url);
            if (n <= 0 || n >= (int)sizeof(http_url)) {
                glog("URL too long\n");
                status_display_show_status("HTTP Usage");
                return;
            }
            http_url_ptr = http_url;
        }

        char *resp = NULL;
#if defined(CONFIG_SPIRAM)
        resp = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!resp) resp = (char *)malloc(4096);
        if (!resp) {
            glog("Failed to allocate response buffer\n");
            status_display_show_status("HTTP Fail");
            return;
        }

        int got = eth_http_get_simple(http_url_ptr, resp, 4096, 2000);
        if (got < 0) {
            glog("HTTP request failed\n");
            status_display_show_status("HTTP Fail");
            if (esp_ptr_external_ram(resp)) {
                heap_caps_free(resp);
            } else {
                free(resp);
            }
            return;
        }

        int lines = 0;
        const char *p = resp;
        while (*p) {
            if (max_lines > 0 && lines >= max_lines) break;
            const char *nl = strchr(p, '\n');
            if (!nl) {
                glog("%s\n", p);
                break;
            }
            size_t len = (size_t)(nl - p + 1);
            char line[160];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            glog("%s", line);
            lines++;
            p = nl + 1;
        }

        if (got >= 4096 - 1) {
            glog("(response truncated)\n");
        }

        if (esp_ptr_external_ram(resp)) {
            heap_caps_free(resp);
        } else {
            free(resp);
        }
        status_display_show_status("HTTP Done");
        return;
    }

    // Parse URL - use smaller buffers to reduce stack usage
    char hostname[128] = {0};
    char path[256] = "/";
    uint16_t port = 80;
    bool is_https = false;

    // Check for http:// or https://
    const char *url_start = url;
    if (strncmp(url, "http://", 7) == 0) {
        url_start = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        url_start = url + 8;
        port = 443;
        is_https = true;
    } else {
        // No protocol specified, assume http://
        url_start = url;
    }

    // Find hostname and path
    const char *path_start = strchr(url_start, '/');
    if (path_start != NULL) {
        size_t hostname_len = path_start - url_start;
        if (hostname_len >= sizeof(hostname)) {
            hostname_len = sizeof(hostname) - 1;
        }
        strncpy(hostname, url_start, hostname_len);
        hostname[hostname_len] = '\0';
        strncpy(path, path_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strncpy(hostname, url_start, sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    // Check for port in hostname (e.g., example.com:8080)
    char *port_str = strchr(hostname, ':');
    if (port_str != NULL) {
        *port_str = '\0';
        port = (uint16_t)atoi(port_str + 1);
        if (port == 0) {
            port = is_https ? 443 : 80;
        }
    }

    glog("Hostname: %s\n", hostname);
    glog("Path: %s\n", path);
    glog("Port: %d\n", port);

    // Resolve hostname to IP address
    struct hostent *host_entry = gethostbyname(hostname);
    if (host_entry == NULL) {
        glog("Failed to resolve hostname: %s\n", hostname);
        status_display_show_status("DNS Resolve Fail");
        return;
    }

    struct in_addr **addr_list = (struct in_addr **)host_entry->h_addr_list;
    if (addr_list[0] == NULL) {
        glog("No IP address found for hostname: %s\n", hostname);
        status_display_show_status("DNS No IP");
        return;
    }

    char ip_str[16];
    // Convert struct in_addr to ip4_addr_t for LWIP compatibility
    ip4_addr_t ip4_addr;
    ip4_addr.addr = addr_list[0]->s_addr;
    ip4addr_ntoa_r(&ip4_addr, ip_str, sizeof(ip_str));
    glog("Resolved to IP: %s\n", ip_str);

    // Prepare HTTP request
    char request[512];
    int request_len = snprintf(request, sizeof(request),
                                "GET %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: GhostESP/1.0\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                path, hostname);

    if (request_len >= (int)sizeof(request)) {
        glog("Request too long\n");
        status_display_show_status("Request Too Long");
        return;
    }

    // Variables for response handling
    char buffer[128];
    ssize_t total_received = 0;
    // No size limit - we process in small chunks to avoid stack issues

    if (is_https) {
        // HTTPS using mbedTLS
        // Use static contexts to reduce stack usage (mbedTLS contexts are very large ~5KB+)
        // Since this is a synchronous command handler, static is safe
        static mbedtls_entropy_context entropy;
        static mbedtls_ctr_drbg_context ctr_drbg;
        static mbedtls_net_context server_fd;
        static mbedtls_ssl_context ssl;
        static mbedtls_ssl_config conf;
        static bool tls_initialized = false;
        const char *pers = "ghost_esp_https";

        // Initialize contexts (only once, reuse for subsequent calls)
        if (!tls_initialized) {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&ctr_drbg);
            mbedtls_net_init(&server_fd);
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&conf);
            if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                      (const unsigned char *)pers, strlen(pers)) != 0) {
                glog("mbedTLS RNG seed failed\n");
                mbedtls_ssl_config_free(&conf);
                mbedtls_ssl_free(&ssl);
                mbedtls_net_free(&server_fd);
                mbedtls_ctr_drbg_free(&ctr_drbg);
                mbedtls_entropy_free(&entropy);
                status_display_show_status("TLS Init Fail");
                return;
            }
            tls_initialized = true;
        } else {
            // Clean up previous connection state and reinit
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_init(&ssl);
            mbedtls_net_init(&server_fd);
            mbedtls_ssl_config_init(&conf);
        }

        // Configure SSL
        int ret = mbedtls_ssl_config_defaults(&conf,
                                              MBEDTLS_SSL_IS_CLIENT,
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            glog("mbedTLS config defaults failed: -0x%04x\n", -ret);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Config Fail");
            return;
        }

        // Set authmode to optional (skip certificate verification for simplicity)
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        mbedtls_ssl_conf_read_timeout(&conf, 10000); // 10 second timeout

        // Setup SSL
        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret != 0) {
            glog("mbedTLS SSL setup failed: -0x%04x\n", -ret);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Setup Fail");
            return;
        }

        // Set hostname for SNI
        ret = mbedtls_ssl_set_hostname(&ssl, hostname);
        if (ret != 0) {
            glog("mbedTLS set hostname failed: -0x%04x\n", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Hostname Fail");
            return;
        }

        // Connect TCP
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", port);
        glog("Connecting to %s:%s...\n", ip_str, port_str);
        
        ret = mbedtls_net_connect(&server_fd, ip_str, port_str, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            glog("mbedTLS connect failed: -0x%04x\n", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Connect Fail");
            return;
        }

        // Set BIO callbacks
        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        // Perform TLS handshake
        glog("Performing TLS handshake...\n");
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                // Use smaller error buffer to reduce stack usage
                char error_buf[128];
                mbedtls_strerror(ret, error_buf, sizeof(error_buf));
                glog("TLS handshake failed: -0x%04x - %s\n", -ret, error_buf);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                mbedtls_ctr_drbg_free(&ctr_drbg);
                mbedtls_entropy_free(&entropy);
                mbedtls_net_free(&server_fd);
                tls_initialized = false;
                status_display_show_status("TLS Handshake Fail");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        glog("TLS handshake successful\n");
        glog("Connected successfully (HTTPS)\n");
        glog("==========================================\n");

        // Send HTTP request over TLS
        size_t written = 0;
        while (written < (size_t)request_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request + written,
                                    request_len - written);
            if (ret < 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    glog("TLS write failed: -0x%04x\n", -ret);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            written += ret;
        }

        if (written != (size_t)request_len) {
            glog("Warning: Only sent %zu of %d bytes\n", written, request_len);
        } else {
            glog("Request sent (%d bytes)\n", request_len);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
        glog("==========================================\n");
        glog("Response:\n");
        glog("==========================================\n");

        // Receive response over TLS - process in small chunks with optional line limiting
        int line_count = 0;
        while (1) {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)buffer, sizeof(buffer) - 1);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                glog("\n[Connection closed by server]\n");
                break;
            }
            if (ret <= 0) {
                if (ret != 0) {
                    glog("\n[TLS read error: -0x%04x]\n", -ret);
                }
                break;
            }

            // Process chunk with line counting and truncation
            buffer[ret] = '\0';
            int print_len = ret;
            
            if (max_lines > 0) {
                // Count lines and find truncation point
                for (int i = 0; i < ret; i++) {
                    if (line_count >= max_lines) {
                        print_len = i;
                        break;
                    }
                    if (buffer[i] == '\n') {
                        line_count++;
                        if (line_count >= max_lines) {
                            print_len = i + 1;  // Include the newline
                            break;
                        }
                    }
                }
            } else {
                // Count lines for statistics (no truncation)
                for (int i = 0; i < ret; i++) {
                    if (buffer[i] == '\n') {
                        line_count++;
                    }
                }
            }
            
            // Print the chunk (or truncated portion)
            if (print_len > 0) {
                glog("%.*s", print_len, buffer);
            }
            
            if (max_lines > 0 && line_count >= max_lines) {
                glog("\n[Response truncated at %d lines]\n", max_lines);
                goto https_done;
            }
            
            total_received += ret;
        }
https_done:

        // Cleanup TLS connection (keep static contexts for reuse)
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_net_free(&server_fd);

    } else {
        // HTTP using regular sockets
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            glog("Failed to create socket: %d\n", errno);
            status_display_show_status("Socket Create Fail");
            return;
        }

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct timeval recv_timeout = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
        
        struct timeval send_timeout = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            glog("Failed to set socket non-blocking: %d\n", errno);
            close(sock);
            status_display_show_status("Socket Config Fail");
            return;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        memcpy(&server_addr.sin_addr, addr_list[0], sizeof(struct in_addr));

        int connect_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (connect_result == 0) {
            fcntl(sock, F_SETFL, flags);
        } else if (errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) <= 0) {
                glog("Connection timeout\n");
                close(sock);
                status_display_show_status("Connect Timeout");
                return;
            }

            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                glog("Connection failed: %d\n", error);
                close(sock);
                status_display_show_status("Connect Fail");
                return;
            }
            fcntl(sock, F_SETFL, flags);
        } else {
            glog("Failed to connect: %d\n", errno);
            close(sock);
            status_display_show_status("Connect Fail");
            return;
        }

        glog("Connected successfully\n");
        glog("==========================================\n");

        ssize_t sent = send(sock, request, request_len, 0);
        if (sent < 0) {
            glog("Failed to send request: %d\n", errno);
            close(sock);
            status_display_show_status("Send Fail");
            return;
        }

        if (sent != request_len) {
            glog("Warning: Only sent %d of %d bytes\n", (int)sent, request_len);
        }

        glog("Request sent (%d bytes)\n", (int)sent);
        vTaskDelay(pdMS_TO_TICKS(50));
        glog("==========================================\n");
        glog("Response:\n");
        glog("==========================================\n");

        recv_timeout.tv_sec = 10;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // Receive response - process in small chunks with optional line limiting
        int line_count = 0;
        while (1) {
            ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received <= 0) {
                if (received == 0) {
                    glog("\n[Connection closed by server]\n");
                } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    glog("\n[Receive timeout]\n");
                } else {
                    glog("\n[Receive error: %d]\n", errno);
                }
                break;
            }

            // Process chunk with line counting and truncation
            buffer[received] = '\0';
            int print_len = received;
            
            if (max_lines > 0) {
                // Count lines and find truncation point
                for (int i = 0; i < received; i++) {
                    if (line_count >= max_lines) {
                        print_len = i;
                        break;
                    }
                    if (buffer[i] == '\n') {
                        line_count++;
                        if (line_count >= max_lines) {
                            print_len = i + 1;  // Include the newline
                            break;
                        }
                    }
                }
            } else {
                // Count lines for statistics (no truncation)
                for (int i = 0; i < received; i++) {
                    if (buffer[i] == '\n') {
                        line_count++;
                    }
                }
            }
            
            // Print the chunk (or truncated portion)
            if (print_len > 0) {
                glog("%.*s", print_len, buffer);
            }
            
            if (max_lines > 0 && line_count >= max_lines) {
                glog("\n[Response truncated at %d lines]\n", max_lines);
                goto http_done;
            }
            
            total_received += received;
        }
http_done:

        // Cleanup socket connection
        if (sock >= 0) {
            shutdown(sock, SHUT_RDWR);
            close(sock);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("\n==========================================\n");
    glog("Total received: %zu bytes\n", total_received);
    
    status_display_show_status(is_https ? "HTTPS OK" : "HTTP OK");
}

void handle_eth_poison_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: ethpoison <start|stop|list|cookies|creds|status>\n");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        eth_arp_poison_start();
    } else if (strcmp(argv[1], "stop") == 0) {
        eth_arp_poison_stop();
    } else if (strcmp(argv[1], "list") == 0) {
        eth_arp_poison_print_domains();
    } else if (strcmp(argv[1], "cookies") == 0) {
        eth_arp_poison_print_cookies();
    } else if (strcmp(argv[1], "creds") == 0) {
        eth_arp_poison_print_creds();
    } else if (strcmp(argv[1], "status") == 0) {
        eth_arp_poison_print_status();
    } else {
        glog("Unknown subcommand: %s\n", argv[1]);
        glog("Usage: ethpoison <start|stop|list|cookies|creds|status>\n");
    }
}
#endif

// settime - Set system time from a Unix timestamp
void handle_settime_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: settime <unix_timestamp>\n");
        glog("Example: settime 1704067200\n");
        status_display_show_status("SetTime Use");
        return;
    }
    
    long timestamp = strtol(argv[1], NULL, 10);
    if (timestamp <= 0) {
        glog("Invalid timestamp: %s\n", argv[1]);
        status_display_show_status("SetTime Invalid");
        return;
    }
    
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    
    if (settimeofday(&tv, NULL) != 0) {
        glog("Failed to set system time: %s\n", strerror(errno));
        status_display_show_status("SetTime Fail");
        return;
    }
    
    // Display the set time
    time_t now = (time_t)timestamp;
    struct tm timeinfo;
    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    glog("System time set successfully!\n");
    glog("Time: %s\n", strftime_buf);
    
#ifdef CONFIG_HAS_RTC_CLOCK
    // Save UTC time to RTC (not local time)
    struct tm utc_timeinfo;
    gmtime_r(&timestamp, &utc_timeinfo);
    RTC_Date rtc_time;
    rtc_time.year = utc_timeinfo.tm_year + 1900;
    rtc_time.month = utc_timeinfo.tm_mon + 1;
    rtc_time.day = utc_timeinfo.tm_mday;
    rtc_time.hour = utc_timeinfo.tm_hour;
    rtc_time.minute = utc_timeinfo.tm_min;
    rtc_time.second = utc_timeinfo.tm_sec;
    
    if (rtc_set_datetime(&rtc_time) == ESP_OK) {
        glog("UTC time saved to RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
             rtc_time.year, rtc_time.month, rtc_time.day, 
             rtc_time.hour, rtc_time.minute, rtc_time.second);
    } else {
        glog("Failed to save time to RTC\n");
    }
#endif
    
    status_display_show_status("Time Set OK");
}

// time - Display current system time
void handle_time_cmd(int argc, char **argv) {
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];
    
    time(&now);
    
    // Display local time
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    glog("Local time: %s\n", strftime_buf);
    
    // Display UTC time
    struct tm timeinfo_utc;
    gmtime_r(&now, &timeinfo_utc);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo_utc);
    glog("UTC time: %s\n", strftime_buf);
    
    // Display Unix timestamp
    glog("Unix timestamp: %ld\n", (long)now);
}

void handle_startwd(int argc, char **argv) {
    bool stop_flag = false;
    bool helper_mode = false;
    char helper_channels_csv[192] = {0};
    bool helper_channels_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
        } else if (strcmp(argv[i], "--helper") == 0) {
            helper_mode = true;
        } else if (strcmp(argv[i], "--channels") == 0) {
            if (i + 1 >= argc) {
                glog("startwd: --channels requires a value\n");
                return;
            }
            strncpy(helper_channels_csv, argv[++i], sizeof(helper_channels_csv) - 1);
            helper_channels_set = true;
        } else if (strncmp(argv[i], "--channels=", 11) == 0) {
            strncpy(helper_channels_csv, argv[i] + 11, sizeof(helper_channels_csv) - 1);
            helper_channels_set = true;
        }
    }

    if (stop_flag) {
        if (!helper_mode && !esp_comm_manager_is_remote_command()) {
            if (esp_comm_manager_is_connected()) {
                bool peer_stop_ok = esp_comm_manager_send_command("startwd", "-s --helper");
                glog(peer_stop_ok
                         ? "Wardrive helper stop sent to peer.\n"
                         : "Wardrive helper stop could not be sent to peer.\n");
            }
            wardriving_set_peer_assist(false);
        }

        stop_wardriving();
        wifi_manager_stop_monitor_mode();
        if (!helper_mode) {
            if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
                csv_flush_buffer_to_file();
            }
            csv_file_close();
            gps_manager_deinit(&g_gpsManager);
            glog("Wardriving stopped.\n");
            status_display_show_status("Wardrive Stop");
        } else {
            (void)wardriving_set_helper_channels_from_csv(NULL);
            glog("Wardriving helper stopped.\n");
            status_display_show_status("WD Helper Stop");
        }
    } else {
        if (helper_mode) {
            if (helper_channels_set) {
                if (!wardriving_set_helper_channels_from_csv(helper_channels_csv)) {
                    glog("Wardriving helper: invalid channel list, using default helper channels.\n");
                }
            } else {
                (void)wardriving_set_helper_channels_from_csv(NULL);
            }

            if (!g_gpsManager.isinitilized) {
                gps_manager_init(&g_gpsManager);
            }

            wifi_manager_start_monitor_mode(wardriving_scan_callback);
            start_wardriving_helper();
            glog("Wardriving helper started.\n");
            status_display_show_status("WD Helper Start");
            return;
        }

        bool prefer_peer_only = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        prefer_peer_only = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) &&
                           !esp_comm_manager_is_remote_command() &&
                           esp_comm_manager_is_connected();
#endif

        if (prefer_peer_only) {
            gps_manager_set_peer_gps_preferred(true);
            gps_manager_clear_peer_fix();
            if (g_gpsManager.isinitilized) {
                gps_manager_deinit(&g_gpsManager);
            }
            glog("Wardriving: peer GPS preferred, local soft GPS disabled.\n");
        } else {
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_init(&g_gpsManager);
        }
        esp_err_t err = csv_file_open("wardriving");
        if (err != ESP_OK) {
            glog("Failed to open CSV for wardriving\n");
            status_display_show_status("CSV Open Fail");
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        start_wardriving();

        bool peer_helper_ok = false;
        if (!esp_comm_manager_is_remote_command()) {
            if (esp_comm_manager_is_connected()) {
                char helper_args[256] = "--helper";
                char helper_plan_csv[192] = {0};
                if (wardriving_get_helper_channel_plan_csv(helper_plan_csv, sizeof(helper_plan_csv))) {
                    snprintf(helper_args, sizeof(helper_args), "--helper --channels %s", helper_plan_csv);
                }
                peer_helper_ok = esp_comm_manager_send_command("startwd", helper_args);
                glog(peer_helper_ok
                         ? "Wardrive helper started on peer (split-channel).\n"
                         : "Wardrive helper not started on peer; continuing local only.\n");
            } else {
                glog("Wardrive helper unavailable: no GhostLink peer connected.\n");
            }
        }
        wardriving_set_peer_assist(peer_helper_ok);

        if (prefer_peer_only && !peer_helper_ok) {
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_clear_peer_fix();
            if (!g_gpsManager.isinitilized) {
                gps_manager_init(&g_gpsManager);
            }
            glog("Wardriving: peer helper unavailable, falling back to local GPS.\n");
        }

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
        port_scan_subnet_async();  // Use async version to keep CLI responsive
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
    
    glog("Starting SSH scan on %s...\n", target_ip);
    
    ssh_scan_host(target_ip);
    
    status_display_show_status("SSH Scan Done");
}

void handle_crash(int argc, char **argv) {
    glog("Triggering crash for coredump test...\n");
    (void)argc;
    (void)argv;
    /* Intentional null pointer write to trigger panic; coredump will be saved to flash. */
    int *ptr = NULL;
    *ptr = 42;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/* Read coredump partition and print summary or stream base64 for host decode. */
static void handle_coredump_cmd(int argc, char **argv) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
    if (part == NULL) {
        glog("No coredump partition found. Check partition table.\n");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "erase") == 0) {
        /* Use partition erase on all targets (esp_core_dump_image_erase can fail on plain ESP32). */
        size_t esz = part->erase_size;
        size_t to_erase = (part->size / esz) * esz;
        if (to_erase == 0) {
            to_erase = esz;
        }
        esp_err_t err = esp_partition_erase_range(part, 0, to_erase);
        if (err == ESP_OK) {
            glog("Coredump partition erased.\n");
        } else {
            glog("Failed to erase coredump: %s\n", esp_err_to_name(err));
        }
        return;
    }

    const int do_dump = (argc > 1 && strcmp(argv[1], "dump") == 0);

    if (do_dump) {
        /* Stream partition as base64 so user can save and run: idf.py coredump-info -c <file> */
        glog("=== COREDUMP BASE64 START ===\n");
        glog("Save the lines below to a file (e.g. coredump.b64), then run:\n");
        glog("  idf.py coredump-info -c coredump.b64\n");
        glog("(Omit the start/end marker lines from the file.)\n");
        uint8_t buf[768]; /* multiple of 3 for base64 */
        char b64[1032];
        size_t offset = 0;
        while (offset < part->size) {
            size_t chunk = (part->size - offset) > sizeof(buf) ? sizeof(buf) : (part->size - offset);
            if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
                glog("\nRead error at offset %u\n", (unsigned)offset);
                break;
            }
            size_t written = 0;
            int ret = mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &written, buf, chunk);
            if (ret != 0) {
                glog("\nBase64 encode error\n");
                break;
            }
            b64[written] = '\0';
            glog("%s", b64);
            offset += chunk;
        }
        glog("\n=== COREDUMP BASE64 END ===\n");
        return;
    }

    /* Summary: partition info and whether it contains valid coredump data.
     * ESP-IDF may write a small header (e.g. checksum) before the ELF, so scan
     * the first 128 bytes for ELF magic (0x7f 'E' 'L' 'F') instead of only offset 0. */
    uint8_t head[128];
    size_t head_len = part->size < sizeof(head) ? (size_t)part->size : sizeof(head);
    if (esp_partition_read(part, 0, head, head_len) != ESP_OK) {
        glog("Failed to read coredump partition.\n");
        return;
    }
    int elf_offset = -1;
    for (size_t i = 0; i + 4 <= head_len; i++) {
        if (head[i] == 0x7f && head[i + 1] == 'E' && head[i + 2] == 'L' && head[i + 3] == 'F') {
            elf_offset = (int)i;
            break;
        }
    }
    const int is_elf = (elf_offset >= 0);

    glog("Coredump partition: %s, size %u bytes\n", part->label, (unsigned)part->size);

    /* Use ESP-IDF API to parse and print panic reason from coredump in flash */
    {
        char panic_reason[256];
        esp_err_t err = esp_core_dump_get_panic_reason(panic_reason, sizeof(panic_reason));
        if (err == ESP_OK && panic_reason[0] != '\0') {
            glog("Panic reason: %s\n", panic_reason);
        } else if (err == ESP_ERR_NOT_FOUND) {
            /* Do not call esp_core_dump_get_summary() here: it uses too much stack and can
             * cause Stack protection fault in SerialTask (same task as CLI). */
            glog("Panic reason: (not available on device; run idf.py coredump-info on host)\n");
        } else {
            glog("Panic reason: (error %s)\n", esp_err_to_name(err));
        }
    }

    if (is_elf) {
        glog("Coredump data: present (ELF format");
        if (elf_offset > 0) {
            glog(", ELF at offset %d", elf_offset);
        }
        glog(").\n");
        glog("For full backtrace run on host: idf.py coredump-info\n");
    } else {
        /* Check for empty (all 0xff) or binary format */
        int empty = 1;
        for (size_t i = 0; i < head_len && empty; i++) {
            if (head[i] != 0xff) {
                empty = 0;
            }
        }
        if (empty != 0) {
            glog("Coredump data: partition empty (no crash recorded yet).\n");
        } else {
            glog("Coredump data: present (binary format).\n");
            glog("For full backtrace run on host: idf.py coredump-info\n");
        }
    }
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */


// Help command
void handle_help(int argc, char **argv) {
    const char *category = (argc > 1) ? argv[1] : "unknown"; // Default to "unknown" if no category is provided to fall through ifs

    // List of all categories to print in order
    const char *all_categories[] = {
        "wifi", "ble", "chameleon", "comm", "sd", "led", "gps", "misc", "portal", "printer", "cast", "capture", "beacon", "attack"
#ifdef CONFIG_HAS_INFRARED
        , "ir"
#endif
#ifdef CONFIG_WITH_ETHERNET
        , "ethernet"
#endif
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
        glog("scanap\n");
        glog("    Description: Start a Wi-Fi access point (AP) scan.\n");
        glog("    Usage: scanap [seconds]\n\n");
        glog("scansta\n");
        glog("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
        glog("    Usage: scansta\n\n");
        glog("stopscan\n");
        glog("    Description: Stop any ongoing Wi-Fi scan.\n");
        glog("    Usage: stopscan\n\n");
        glog("attack\n");
        glog("    Description: Launch an attack (e.g., deauthentication attack).\n");
        glog("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
        glog("    Usage: attack -d (deauth) | attack -c (channel switch) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
        glog("    Arguments:\n");
        glog("        -d  : Start deauth attack (supports multiple APs)\n");
        glog("        -c  : Start channel switch attack (supports multiple APs)\n");
        glog("        -e  : Start EAPOL logoff attack\n");
        glog("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n\n");
        glog("list\n");
        glog("    Description: List Wi-Fi scan results or connected stations.\n");
        glog("    Usage: list -a | list -s | list -airtags\n");
        glog("    Arguments:\n");
        glog("        -a  : Show access points from Wi-Fi scan\n");
        glog("        -s  : List connected stations\n");
        glog("        -airtags: List discovered AirTags\n\n");
        glog("beaconspam\n");
        glog("    Description: Start beacon spam with different modes.\n");
        glog("    Usage: beaconspam [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -r   : Start random beacon spam\n");
        glog("        -rr  : Start Rickroll beacon spam\n");
        glog("        -l   : Start AP List beacon spam\n");
        glog("        [SSID]: Use specified SSID for beacon spam\n\n");
        glog("stopspam\n");
        glog("    Description: Stop ongoing beacon spam.\n");
        glog("    Usage: stopspam\n\n");
        glog("stopdeauth\n");
        glog("    Description: Stop ongoing deauthentication attack.\n");
        glog("    Usage: stopdeauth\n\n");
        glog("select\n");
        glog("    Description: Select access point(s), station, or AirTag by index from the scan results.\n");
        glog("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
        glog("    Arguments:\n");
        glog("        -a      : AP selection index (supports multiple: 1,3,5)\n");
        glog("        -s      : Station selection index\n");
        glog("        -airtag : AirTag selection index\n");
        glog("    Examples:\n");
        glog("        select -a 4      : Select single AP at index 4\n");
        glog("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
        glog("scanall\n");
        glog("    Description: Perform combined AP and Station scan, display results.\n");
        glog("    Usage: scanall [seconds]\n\n");
        glog("sweep\n");
        glog("    Description: Full environment sweep - scans WiFi APs, stations, BLE devices\n");
        glog("                 and saves comprehensive report to SD card.\n");
        glog("    Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
        glog("    Arguments:\n");
        glog("        -w  : WiFi scan duration per phase in seconds (default: 5)\n");
        glog("        -b  : BLE scan duration per phase in seconds (default: 5)\n");
        glog("    Output: /mnt/ghostesp/sweeps/sweep_N.csv\n\n");
        glog("congestion\n");
        glog("    Description: Display Wi-Fi channel congestion chart.\n");
        glog("    Usage: congestion\n\n");
        glog("connect\n");
        glog("    Description: Connects to Specific WiFi Network and saves credentials.\n");
        glog("    Usage: connect <SSID> [Password]\n\n");
        glog("apcred\n");
        glog("    Description: Change or reset the GhostNet AP credentials\n");
        glog("    Usage: apcred <ssid> <password>\n");
        glog("           apcred -r (reset to defaults)\n");
        glog("    Arguments:\n");
        glog("        <ssid>     : New SSID for the AP\n");
        glog("        <password> : New password (min 8 characters)\n");
        glog("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
        glog("apenable\n");
        glog("    Description: Enable or disable the Access Point across reboots\n");
        glog("    Usage: apenable <on|off>\n");
        glog("    Arguments:\n");
        glog("        on  : Enable the Access Point (requires restart)\n");
        glog("        off : Disable the Access Point (requires restart)\n\n");
        glog("listenprobes\n");
        glog("    Description: Listen for and log probe requests.\n");
        glog("    Usage: listenprobes [channel] [stop]\n");
        glog("    Arguments:\n");
        glog("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
        glog("        stop      : Stop probe request listening\n\n");
        glog("karma\n");
        glog("    Description: Start or stop the Karma attack (responds to probe requests with specified or all SSIDs).\n");
        glog("    Usage: karma start [ssid1 ssid2 ...]\n");
        glog("           karma stop\n");
        glog("    Arguments:\n");
        glog("        start : Begin Karma attack. Optionally specify SSIDs to respond with (default: all known SSIDs).\n");
        glog("        stop  : Stop Karma attack.\n");
        glog("    Examples:\n");
        glog("        karma start\n");
        glog("        karma start FreeWiFi Starbucks\n");
        glog("        karma stop\n\n");
        glog("trackap\n");
        glog("    Description: track selected ap signal strength (rssi)\n");
        glog("    Usage: trackap\n");
        glog("    Note: select an ap first with 'select -a <index>'\n\n");
        glog("tracksta\n");
        glog("    Description: track selected station signal strength (rssi)\n");
        glog("    Usage: tracksta\n");
        glog("    Note: select a station first with 'select -s <index>'\n\n");
#if CONFIG_IDF_TARGET_ESP32C5
        glog("setcountry\n");
        glog("    Description: Set the Wi-Fi country code.\n");
        glog("    Usage: setcountry <CC>\n");
        glog("    Arguments:\n");
        glog("        <CC> : Country code (\"01\" world-safe) or two-letter ISO (e.g., US)\n");
        glog("    Supported: 01, AT, AU, BE, BG, BR, CA, CH, CN, CY, CZ, DE, DK, EE, ES, FI, FR, GB, GR, HK, HR, HU,\n");
        glog("               IE, IN, IS, IT, JP, KR, LI, LT, LU, LV, MT, MX, NL, NO, NZ, PL, PT, RO, SE, SI, SK, TW, US\n\n");
#endif
        return;
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(category, "ble") == 0) {
        glog("\nBLE Commands:\n\n");
        glog("blescan\n");
        glog("    Description: Handle BLE scanning with various modes.\n");
        glog("    Usage: blescan [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -f   : Start 'Find the Flippers' mode\n");
        glog("        -ds  : Start BLE spam detector\n");
        glog("        -a   : Start AirTag scanner\n");
        glog("        -r   : Scan for raw BLE packets\n");
        glog("        -s   : Stop BLE scanning\n\n");
        glog("blespam\n");
        glog("    Description: Start BLE advertisement spam attacks.\n");
        glog("    Usage: blespam [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -apple     : Apple device spam (AirPods, Apple TV, etc.)\n");
        glog("        -ms        : Microsoft Swift Pair spam\n");
        glog("        -samsung   : Samsung Galaxy Watch spam\n");
        glog("        -google    : Google Fast Pair spam\n");
        glog("        -random    : Random spam (cycles through all types)\n");
        glog("        -s         : Stop BLE spam\n\n");
        glog("blewardriving\n");
        glog("    Description: Start/Stop BLE wardriving with GPS logging\n");
        glog("    Usage: blewardriving [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop BLE wardriving\n\n");
        glog("list -airtags\n");
        glog("    Description: List discovered AirTags\n");
        glog("    Usage: list -airtags\n\n");
        glog("select -airtag <index>\n\n");
        return;
    }

    if (strcmp(category, "chameleon") == 0) {
        glog("\nChameleon Ultra Commands:\n\n");
        glog("chameleon connect [timeout] [pin]\n");
        glog("    Description: Connect to a Chameleon Ultra device via BLE\n");
        glog("    Usage: chameleon connect [timeout_seconds] [pin]\n");
        glog("    Arguments:\n");
        glog("        timeout_seconds : Connection timeout (default: 10)\n");
        glog("        pin            : PIN for authentication (4-6 digits, optional)\n\n");
        glog("chameleon disconnect\n");
        glog("    Description: Disconnect from the Chameleon Ultra device\n");
        glog("    Usage: chameleon disconnect\n\n");
        glog("chameleon status\n");
        glog("    Description: Check connection status with Chameleon Ultra\n");
        glog("    Usage: chameleon status\n\n");
        glog("chameleon scanhf\n");
        glog("    Description: Scan for High Frequency (HF) RFID tags\n");
        glog("    Usage: chameleon scanhf\n\n");
        glog("chameleon scanlf\n");
        glog("    Description: Scan for Low Frequency (LF) RFID tags\n");
        glog("    Usage: chameleon scanlf\n\n");
        glog("chameleon battery\n");
        glog("    Description: Get battery information from Chameleon Ultra\n");
        glog("    Usage: chameleon battery\n\n");
        glog("chameleon reader\n");
        glog("    Description: Set Chameleon Ultra to reader mode\n");
        glog("    Usage: chameleon reader\n\n");
        glog("chameleon emulator\n");
        glog("    Description: Set Chameleon Ultra to emulator mode\n");
        glog("    Usage: chameleon emulator\n\n");
        return;
    }
#endif

    if (strcmp(category, "comm") == 0) {
        glog("\nCommunication Commands:\n\n");
        glog("commdiscovery\n    Check discovery status.\n    Usage: commdiscovery\n\n");
        glog("commconnect\n    Connect to a discovered peer ESP32.\n    Usage: commconnect <peer_name>\n    Example: commconnect ESP_A1B2C3\n\n");
        glog("commsend\n    Send a command to connected peer ESP32.\n    Usage: commsend <command> [data]\n    Example: commsend scanap\n    Example: commsend hello world\n\n");
        glog("commstatus\n    Show communication status.\n    Usage: commstatus\n\n");
        glog("commdisconnect\n    Disconnect from current peer.\n    Usage: commdisconnect\n\n");
        glog("commsetpins\n    Change communication GPIO pins at runtime.\n    Usage: commsetpins <tx_pin> <rx_pin>\n    Example: commsetpins 4 5\n\n");
        return;
    }

    if (strcmp(category, "sd") == 0) {
        glog("\nSD Card Commands:\n\n");
        glog("-- File Operations (machine-parsable) --\n");
        glog("sd status\n    Show SD mount status, type, capacity, usage.\n    Usage: sd status\n\n");
        glog("sd list\n    List files/dirs with indices.\n    Usage: sd list [path]\n\n");
        glog("sd info\n    Show file/dir details.\n    Usage: sd info <index|path>\n\n");
        glog("sd size\n    Get file size.\n    Usage: sd size <index|path>\n\n");
        glog("sd read\n    Read file (chunked downloads).\n    Usage: sd read <index|path> [offset] [length]\n\n");
        glog("sd write\n    Create/overwrite file with base64 data.\n    Usage: sd write <path> <base64>\n\n");
        glog("sd append\n    Append base64 data to file.\n    Usage: sd append <path> <base64>\n\n");
        glog("sd mkdir\n    Create directory.\n    Usage: sd mkdir <path>\n\n");
        glog("sd rm\n    Delete file or empty directory.\n    Usage: sd rm <index|path>\n\n");
        glog("sd tree\n    Recursive listing.\n    Usage: sd tree [path] [depth]\n\n");
        glog("-- Pin Configuration --\n");
        glog("sd_config\n    Show current SD GPIO pin configuration.\n    Usage: sd_config\n\n");
        glog("sd_pins_mmc\n    Set GPIO pins for SDMMC mode.\n    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");
        glog("sd_pins_spi\n    Set GPIO pins for SPI mode.\n    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");
        glog("sd_save_config\n    Save pin config to NVS.\n    Usage: sd_save_config\n\n");
        return;
    }

    if (strcmp(category, "led") == 0) {
        glog("\nLED & RGB Commands:\n\n");
        glog("rgbmode\n    Control LED effects (rainbow, police, strobe, knight, off)\n    Usage: rgbmode <rainbow|police|strobe|knight|off|color>\n\n");
        glog("setrgbpins\n    Change RGB LED pins\n    Usage: setrgbpins <red> <green> <blue>\n           (use same value for all pins for single-pin LED strips)\n\n");
        glog("setrgbcount\n    Configure how many RGB LEDs are attached\n    Usage: setrgbcount <1-512>\n\n");
        glog("setneopixelbrightness\n    Set maximum neopixel brightness (percent)\n    Usage: setneopixelbrightness <0-100>\n\n");
        glog("getneopixelbrightness\n    Show current neopixel max brightness (percent)\n    Usage: getneopixelbrightness\n\n");
        return;
    }

    if (strcmp(category, "misc") == 0) {
        glog("\nMiscellaneous Commands:\n\n");
        glog("help\n");
        glog("    Description: Display this help message.\n");
        glog("    Usage: help [category]\n\n");
        glog("chipinfo\n");
        glog("    Description: Display chip information including model, revision, and features\n");
        glog("    Usage: chipinfo\n");
        glog("    Shows:\n");
        glog("        - Chip model and revision\n");
        glog("        - CPU cores and features\n");
        glog("        - Flash size and memory info\n");
        glog("        - ESP-IDF version\n\n");
        glog("crash\n");
        glog("    Description: Intentionally trigger a crash (for coredump testing).\n");
        glog("    Usage: crash\n");
        glog("    The device will panic and save a coredump to flash; use idf.py coredump-info to inspect.\n\n");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        glog("coredump [dump|erase]\n");
        glog("    Description: Read or clear coredump in flash.\n");
        glog("    Usage: coredump        - Print summary (partition size, whether coredump present).\n");
        glog("           coredump dump   - Stream coredump as base64; save to file and run idf.py coredump-info -c <file> on host.\n");
        glog("           coredump erase  - Erase coredump partition (clears saved crash).\n");
        glog("    With device connected, 'idf.py coredump-info' on host shows full panic reason and backtrace.\n\n");
#endif
        glog("timezone\n");
        glog("    Description: Set the display timezone for the clock view.\n");
        glog("    Usage: timezone <TZ_STRING>\n\n");
        glog("webauth\n");
        glog("    Description: Enable/disable web authentication.\n");
        glog("    Usage: webauth <enable|disable>\n\n");
        glog("pineap\n");
        glog("    Description: Start/Stop detecting WiFi Pineapples.\n");
        glog("    Usage: pineap [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop PineAP detection\n\n");
        glog("Port Scanner\n");
        glog("    Description: Scan ports on local subnet or specific IP\n");
        glog("    Usage: scanports local\n");
        glog("           scanports <IP> [all | start-end]\n");
        glog("    Arguments:\n");
        glog("        all  : Scan all ports (1-65535)\n");
        glog("        start-end : Custom port range (e.g. 80-443)\n");
        glog("        (no range) : Scan common ports (default)\n\n");
        glog("scanarp\n");
        glog("    Description: Perform ARP scan on local network to discover active hosts\n");
        glog("    Usage: scanarp\n\n");
        glog("settings\n");
        glog("    Description: Manage NVS stored settings via command line\n");
        glog("    Usage: settings <command> [arguments]\n");
        glog("    Commands:\n");
        glog("        list                    - List all available settings\n");
        glog("        get <setting>           - Get current value of a setting\n");
        glog("        set <setting> <value>   - Set a setting to a value\n");
        glog("        reset [setting]         - Reset setting(s) to defaults\n");
        glog("        help                    - Show settings help\n");
        glog("    Examples:\n");
        glog("        settings list\n");
        glog("        settings get ap_ssid\n");
        glog("        settings set rgb_mode 1\n");
        glog("        settings reset\n\n");
        glog("    Description: View or change the status display idle animation (status OLED only).\n");
        glog("    Usage: statusidle [list|set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>]\n\n");
        return;
    }
    if (strcmp(category, "gps") == 0) {
        glog("\nGPS Commands:\n\n");
        glog("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo [-s]\n\n");
        glog("gpspin\n    Set GPS RX pin for external GPS module.\n    Usage: gpspin <pin>\n\n");
        glog("startwd\n    Start GPS wardriving.\n    Usage: startwd [-s] [--helper] [--channels <csv>]\n\n");
        return;
    }
    if (strcmp(category, "wigle") == 0) {
        glog("\nWiGLE Commands:\n\n");
        glog("wigle API <encoded|name:token>\n    Set WiGLE API credentials (encoded token or legacy format).\n\n");
        glog("wigle auto on/off\n    Enable/disable auto-upload.\n\n");
        glog("wigle donate on/off\n    Enable/disable WiGLE donate flag.\n\n");
        glog("wigle show\n    Show WiGLE settings.\n\n");
        glog("wigle list\n    Show uploaded CSV memory.\n\n");
        glog("wigle files [page]\n    List CSVs in /mnt/ghostesp/gps/ for manual upload.\n\n");
        glog("wigle upload <filename>\n    Upload a specific CSV file.\n\n");
        glog("wigle upload all\n    Upload all pending queue files.\n\n");
        glog("wigle stats\n    Show account stats for current API key.\n\n");
        return;
    }
    if (strcmp(category, "portal") == 0) {
        glog("\nEvil Portal Commands:\n\n");
        glog("startportal\n");
        glog("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
        glog("                 /mnt/ prefix is added automatically to file paths if missing.\n");
        glog("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
        glog("           PSK is optional for an open network.\n");
        glog("    Use 'default' as the file path for the default Evil Portal.\n");
        glog("\n");
        glog("evilportal\n");
        glog("    Description: Configure Evil Portal HTML content via UART buffer.\n");
        glog("    Usage: evilportal -c sethtmlstr\n");
        glog("    Steps:\n");
        glog("      1. Run: evilportal -c sethtmlstr\n");
        glog("      2. Send [HTML/BEGIN] marker over UART\n");
        glog("      3. Send HTML content over UART\n");
        glog("      4. Send [HTML/CLOSE] marker over UART\n");
        glog("      5. Run startportal (will use buffered HTML)\n");
        glog("\n");
        glog("stopportal\n");
        glog("    Description: Stop Evil Portal\n");
        glog("    Usage: stopportal\n\n");
        glog("listportals\n    List available Evil Portal files.\n    Usage: listportals\n\n");
        return;
    }

    if (strcmp(category, "printer") == 0) {
        glog("\nPrinter Commands:\n\n");
        glog("powerprinter\n");
        glog("    Description: Print Custom Text to a Printer on your LAN (Requires You to Run Connect First)\n");
        glog("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
        glog("    aligment options: CM = Center Middle, TL = Top Left, TR = Top Right, BR = Bottom Right, BL = Bottom Left\n\n");
        glog("powerprinter\n");
        glog("    Print custom text to a network printer.\n");
        glog("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n\n");
        return;
    }

    if (strcmp(category, "cast") == 0) {
        glog("\nYouTube Cast Commands:\n\n");
        glog("dialconnect\n");
        glog("    Description: Cast a Random Youtube Video on all Smart TV's on your LAN (Requires You to Run Connect First)\n");
        glog("    Usage: dialconnect\n\n");
        glog("dialconnect\n");
        glog("    Cast a random YouTube video to all smart TVs on your LAN.\n");
        glog("    Usage: dialconnect\n\n");
        return;
    }

    if (strcmp(category, "capture") == 0) {
        glog("\nCapture Commands:\n\n");
        glog("capture\n");
        glog("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
        glog("    Usage: capture [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -probe     : Start Capturing Probe Packets\n");
        glog("        -beacon    : Start Capturing Beacon Packets\n");
        glog("        -deauth    : Start Capturing Deauth Packets\n");
        glog("        -raw       : Start Capturing Raw Packets\n");
        glog("        -wps       : Start Capturing WPS Packets and there Auth Type\n");
        glog("        -pwn       : Start Capturing Pwnagotchi Packets\n");
        glog("        -wireshark : Stream raw PCAP to USB/UART for Wireshark\n");
        glog("                    Usage: capture -wireshark [-c <channel>]\n");
        glog("                    -c <channel>: Lock to specific channel (1-%d)\n", MAX_WIFI_CHANNEL);
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("        -802154    : Start Capturing IEEE 802.15.4 Packets [C5/C6]\n");
        #endif
        glog("        -stop      : Stops the active capture\n\n");
        glog("capture\n");
        glog("    Start a WiFi packet capture.\n");
        glog("    Usage: capture [OPTION]\n");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -802154, -stop\n\n");
        #else
        glog("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -stop\n\n");
        #endif
        return;
    }

    if (strcmp(category, "beacon") == 0) {
        glog("\nBeacon Spam Commands:\n\n");
        glog("beaconadd\n    Add an SSID to the beacon spam list.\n    Usage: beaconadd <SSID>\n\n");
        glog("beaconremove\n    Remove an SSID from the beacon spam list.\n    Usage: beaconremove <SSID>\n\n");
        glog("beaconclear\n    Clear the beacon spam list.\n    Usage: beaconclear\n\n");
        glog("beaconshow\n    Show the current beacon spam list.\n    Usage: beaconshow\n\n");
        glog("beaconspamlist\n    Start beacon spamming using the beacon spam list.\n    Usage: beaconspamlist\n\n");
        return;
    }

    if (strcmp(category, "attack") == 0) {
        glog("\nAttack Commands:\n\n");
        glog("dhcpstarve\n");
        glog("    Description: DHCP starvation flood attack\n");
        glog("    Usage: dhcpstarve start [threads]\n");
        glog("           dhcpstarve stop\n");
        glog("           dhcpstarve display\n\n");
        glog("saeflood\n");
        glog("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
        glog("    Usage: saeflood <password> (requires selected WPA3 AP)\n\n");
        glog("stopsaeflood\n    Stop SAE flood attack.\n    Usage: stopsaeflood\n\n");
        glog("saefloodhelp\n    Show detailed SAE flood attack help.\n    Usage: saefloodhelp\n\n");
        return;
    }
    
#ifdef CONFIG_HAS_INFRARED
    if (strcmp(category, "ir") == 0) {
        glog("\nInfrared Commands:\n\n");
        glog("ir send\n");
        glog("    Description: Send an IR signal from a file.\n");
        glog("    Usage: ir send <path> [index]\n\n");

        glog("ir learn\n");
        glog("    Description: Learn an IR signal and save to file.\n");
        glog("    Usage: ir learn <path>\n\n");
        glog("ir list\n");
        glog("    Description: List IR files in default directory.\n");
        glog("    Usage: ir list [path]\n\n");
        glog("ir rx\n");
        glog("    Description: Receive and display IR signals (Matrix mode).\n");
        glog("    Usage: ir rx [timeout]\n\n");
        glog("ir show\n");
        glog("    Description: Show content of an IR file.\n");
        glog("    Usage: ir show <path>\n\n");
        glog("ir universals\n");
        glog("    Description: Manage universal IR signals (files and built-ins).\n");
        glog("    Usage: ir universals list [-all]\n");
        glog("           ir universals send <index>\n");
        glog("           ir universals sendall <file|TURNHISTVOFF> [delay_ms]\n");
        glog("           ir universals show <file|TURNHISTVOFF>\n\n");
        glog("ir dazzler\n");
        glog("    Description: IR dazzler mode - emit continuous IR to interfere with cameras.\n");
        glog("    Usage: ir dazzler [stop]\n\n");
        return;
    }
#endif

#ifdef CONFIG_WITH_ETHERNET
    if (strcmp(category, "ethernet") == 0) {
        glog("\nEthernet Commands:\n\n");
        printf("ethup\n");
        printf("    Description: Initialize and bring up Ethernet interface.\n");
        printf("    Usage: ethup\n");
        printf("    Note: Waits for link establishment and DHCP assignment.\n\n");
        printf("ethdown\n");
        printf("    Description: Deinitialize and bring down Ethernet interface.\n");
        printf("    Usage: ethdown\n\n");
        printf("ethinfo\n");
        printf("    Description: Display Ethernet connection information.\n");
        printf("    Usage: ethinfo\n");
        printf("    Shows: Status, IP address, netmask, gateway, DNS servers, DHCP server\n\n");
        printf("ethfp\n");
        printf("    Description: Fingerprint network hosts using mDNS, NetBIOS, and SSDP.\n");
        printf("    Usage: ethfp\n");
        printf("    Discovers: Apple devices, Chromecasts, printers, Windows PCs, routers, smart TVs\n\n");
        printf("etharp\n");
        printf("    Description: Perform ARP scan on local Ethernet network.\n");
        printf("    Usage: etharp\n");
        printf("    Scans: Local subnet (1-254) to discover active hosts\n\n");
        printf("ethports\n");
        printf("    Description: Scan TCP ports on a target IP address.\n");
        printf("    Usage: ethports [IP] [all | start-end]\n");
        printf("    Arguments:\n");
        printf("        [IP]      : Target IP address (default: gateway)\n");
        printf("        all       : Scan all ports (1-65535)\n");
        printf("        start-end  : Custom port range (e.g., 80-443)\n");
        printf("        (no range): Scan common ports (default)\n");
        printf("    Examples:\n");
        printf("        ethports\n");
        printf("        ethports 192.168.1.1\n");
        printf("        ethports 192.168.1.1 all\n");
        printf("        ethports 192.168.1.1 80-443\n\n");
        printf("ethping\n");
        printf("    Description: Perform ICMP ping scan on local Ethernet network.\n");
        printf("    Usage: ethping\n");
        printf("    Scans: Local subnet (1-254) to find alive hosts\n\n");
        printf("ethdns\n");
        printf("    Description: Perform DNS lookup or reverse DNS lookup.\n");
        printf("    Usage: ethdns <hostname>\n");
        printf("           ethdns reverse <ip_address>\n");
        printf("    Examples:\n");
        printf("        ethdns google.com\n");
        printf("        ethdns reverse 8.8.8.8\n\n");
        printf("ethtrace\n");
        printf("    Description: Perform traceroute to a target host.\n");
        printf("    Usage: ethtrace <hostname_or_ip> [max_hops]\n");
        printf("    Arguments:\n");
        printf("        hostname_or_ip : Target hostname or IP address\n");
        printf("        max_hops       : Maximum number of hops (default: 30, max: 64)\n");
        printf("    Examples:\n");
        printf("        ethtrace 8.8.8.8\n");
        printf("        ethtrace google.com 30\n\n");
        printf("ethstats\n");
        printf("    Description: Display Ethernet network statistics.\n");
        printf("    Usage: ethstats\n");
        printf("    Shows: Link status, IP info, MAC address, packet statistics, ARP statistics\n\n");
        printf("ethconfig\n");
        printf("    Description: Configure Ethernet IP settings (DHCP or static).\n");
        printf("    Usage: ethconfig <command>\n");
        printf("    Commands:\n");
        printf("        dhcp                    - Use DHCP (automatic IP)\n");
        printf("        static <ip> <netmask> <gateway> - Set static IP\n");
        printf("        show                    - Show current configuration\n");
        printf("    Examples:\n");
        printf("        ethconfig dhcp\n");
        printf("        ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1\n");
        printf("        ethconfig show\n\n");
        printf("ethmac\n");
        printf("    Description: View or set Ethernet MAC address.\n");
        printf("    Usage: ethmac\n");
        printf("           ethmac set <xx:xx:xx:xx:xx:xx>\n");
        printf("    Examples:\n");
        printf("        ethmac\n");
        printf("        ethmac set 02:00:00:00:00:01\n");
        printf("    Note: MAC address changes may require reinitialization\n\n");
        printf("ethserv\n");
        printf("    Description: Service discovery and banner grabbing on a target IP.\n");
        printf("    Usage: ethserv [ip_address]\n");
        printf("    Arguments:\n");
        printf("        [ip_address] : Target IP address (default: gateway)\n");
        printf("    Scans: Common services (FTP, SSH, Telnet, SMTP, HTTP, HTTPS, etc.)\n");
        printf("    Example: ethserv 192.168.1.1\n\n");
        printf("ethntp\n");
        printf("    Description: Query NTP server and synchronize system time.\n");
        printf("    Usage: ethntp [ntp_server]\n");
        printf("    Arguments:\n");
        printf("        [ntp_server] : NTP server hostname or IP (default: pool.ntp.org)\n");
        printf("    Examples:\n");
        printf("        ethntp\n");
        printf("        ethntp pool.ntp.org\n");
        printf("        ethntp time.google.com\n");
        printf("    Note: Requires Ethernet connection to be active\n\n");
        printf("ethhttp\n");
        printf("    Description: Send HTTP/HTTPS GET request to a server and display response.\n");
        printf("    Usage: ethhttp <url> [lines|all]\n");
        printf("    Arguments:\n");
        printf("        <url>  : Full URL including protocol (http:// or https://)\n");
        printf("        [lines]: Optional - show first N lines (default: 25, use 'all' for full)\n");
        printf("    Examples:\n");
        printf("        ethhttp http://example.com  (shows first 25 lines)\n");
        printf("        ethhttp https://www.google.com 50  (shows first 50 lines)\n");
        printf("        ethhttp http://192.168.1.1/index.html all  (shows full response)\n");
        printf("        ethhttp https://example.com:8443/api/data 100\n");
        printf("    Note: Default is 25 lines. Use 'all' for complete responses. HTTPS uses TLS 1.2.\n\n");
        TERMINAL_VIEW_ADD_TEXT("ethup, ethdown, ethinfo, ethfp, etharp, ethports, ethping, ethdns, ethtrace, ethstats, ethconfig, ethmac, ethserv, ethntp, ethhttp\n");
        return;
    }
#endif

    glog("\nGhost ESP Command Categories:\n\n");

    glog("  help wifi      - Wi-Fi commands\n");
    glog("  help ble       - Bluetooth/BLE commands\n");
    glog("  help comm      - ESP32 communication commands\n");
    glog("  help sd        - SD card commands\n");
    glog("  help led       - LED/RGB commands\n");
    glog("  help gps       - GPS commands\n");
    glog("  help wigle     - WiGLE commands\n");
    glog("  help misc      - Miscellaneous commands\n");
    glog("  help portal    - Evil Portal commands\n");
    glog("  help printer   - Printer commands\n");
    glog("  help cast      - YouTube cast commands\n");
    glog("  help capture   - Wi-Fi packet capture commands\n");
    glog("  help beacon    - Beacon spam commands\n");
    glog("  help attack    - Attack/flood commands\n");
#ifdef CONFIG_HAS_INFRARED
    glog("  help ir        - Infrared commands\n");
#endif
#ifdef CONFIG_WITH_ETHERNET
    printf("  help ethernet  - Ethernet commands\n");
#endif
    glog("  help all      - All commands\n\n");

    glog(
        "  help wifi      - Wi-Fi commands\n"
        "  help ble       - Bluetooth/BLE commands\n"
        "  help comm      - ESP32 communication commands\n"
        "  help sd        - SD card commands\n"
        "  help led       - LED/RGB commands\n"
        "  help gps       - GPS commands\n"
        "  help wigle     - WiGLE commands\n"
        "  help misc      - Miscellaneous commands\n");
    glog("  help portal    - Evil Portal commands\n"
         "  help printer   - Printer commands\n"
         "  help cast      - YouTube cast commands\n"
         "  help capture   - Wi-Fi packet capture commands\n"
         "  help beacon    - Beacon spam commands\n"
         "  help attack    - Attack/flood commands\n"
#ifdef CONFIG_HAS_INFRARED
         "  help ir        - Infrared commands\n"
#endif
#ifdef CONFIG_WITH_ETHERNET
         "  help ethernet  - Ethernet commands\n"
#endif
         "  help all      - All commands\n\n");

    glog("Type 'help <category>' for details on that category.\n\n");
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

void handle_gps_pin(int argc, char **argv) {
    if (argc < 2) {
        uint8_t current_pin = settings_get_gps_rx_pin(&G_Settings);
        if (current_pin > 0) {
            glog("GPS RX pin: IO%d\n", current_pin);
        } else {
            glog("GPS RX pin: not set (using default)\n");
        }
        glog("Usage: gpspin <pin>\n");
        return;
    }

    int pin = atoi(argv[1]);
    if (pin < 0 || pin > 48) {
        glog("Invalid pin. Must be 0-48.\n");
        return;
    }

    settings_set_gps_rx_pin(&G_Settings, (uint8_t)pin);
    settings_save(&G_Settings);
    glog("GPS RX pin set to IO%d. Restart GPS to apply.\n", pin);
    TERMINAL_VIEW_ADD_TEXT("GPS pin set to IO%d\n", pin);
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
            
            // Free the manually allocated stack and TCB
            if (gps_task_stack) {
                heap_caps_free(gps_task_stack);
                gps_task_stack = NULL;
            }
            if (gps_task_tcb) {
                heap_caps_free(gps_task_tcb);
                gps_task_tcb = NULL;
            }
            
            gps_manager_deinit(&g_gpsManager);
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_clear_peer_fix();
            printf("GPS info display stopped.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display stopped.\n");
            status_display_show_status("GPS Info Off");
        }
    } else {
        if (gps_info_task_handle == NULL) {
            bool peer_connected = esp_comm_manager_is_connected();
            gps_manager_set_peer_gps_preferred(peer_connected);
            if (!peer_connected) {
                gps_manager_clear_peer_fix();
            }
            if (!peer_connected) {
                gps_manager_init(&g_gpsManager);
            } else if (g_gpsManager.isinitilized) {
                gps_manager_deinit(&g_gpsManager);
            }

            // Wait a moment for GPS initialization
            vTaskDelay(pdMS_TO_TICKS(100));

            // Start info display task with PSRAM preference
            gps_info_task_handle = NULL;
            
            // Allocate stack in PSRAM if available, fallback to internal RAM
            const size_t stack_bytes_target = 8192;
            const size_t stack_words = (stack_bytes_target + sizeof(StackType_t) - 1) / sizeof(StackType_t);
            const size_t stack_size = stack_words * sizeof(StackType_t);
            gps_task_stack = NULL;
            
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
            gps_task_stack = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
            if (!gps_task_stack) {
                gps_task_stack = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            
            if (!gps_task_stack) {
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to allocate stack.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to allocate stack.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }
            
            gps_task_tcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!gps_task_tcb) {
                heap_caps_free(gps_task_stack);
                gps_task_stack = NULL;
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to allocate TCB.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to allocate TCB.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }
            
            TaskHandle_t created_task = xTaskCreateStatic(gps_info_display_task, "gps_info", stack_size, NULL, 1, gps_task_stack, gps_task_tcb);
            if (created_task == NULL) {
                heap_caps_free(gps_task_stack);
                heap_caps_free(gps_task_tcb);
                gps_task_stack = NULL;
                gps_task_tcb = NULL;
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to start.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to start.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }
            gps_info_task_handle = created_task;
            printf("GPS info started.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info started.\n");
            if (peer_connected) {
                printf("GPS source: peer stream preferred.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS source: peer stream preferred.\n");
            } else {
                printf("GPS source: local parser.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS source: local parser.\n");
            }
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
        if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        gps_manager_deinit(&g_gpsManager);
        gps_manager_set_peer_gps_preferred(false);
        gps_manager_clear_peer_fix();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
        status_display_show_status("BLE Drive Off");
    } else {
        bool peer_connected = esp_comm_manager_is_connected();
        gps_manager_set_peer_gps_preferred(peer_connected);
        if (!peer_connected) {
            gps_manager_clear_peer_fix();
        }
        if (!peer_connected && !g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        } else if (peer_connected && g_gpsManager.isinitilized) {
            gps_manager_deinit(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            status_display_show_status("CSV Open Fail");
            return;
        }

        ble_start_scanning();
        ble_register_handler(ble_wardriving_callback);
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
        if (peer_connected) {
            printf("BLE wardriving GPS source: peer stream preferred.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: peer stream preferred.\n");
        } else {
            printf("BLE wardriving GPS source: local parser.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: local parser.\n");
        }
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
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK
#else
            .authmode = WIFI_AUTH_WPA2_PSK
#endif
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
        glog("Usage: rgbmode <rainbow|police|strobe|knight|off|color>\n");
        status_display_show_status("RGB Usage");
        return;
    }

    // Cancel any currently running LED effect task safely.
    if (rgb_effect_task_handle != NULL) {
        rgb_manager_signal_rainbow_exit();
        for (int i = 0; i < 20 && rgb_effect_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (rgb_effect_task_handle != NULL) {
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
    } else if (strcasecmp(argv[1], "knight") == 0) {
        if (!(rgb_manager.is_separate_pins || rgb_manager.strip)) {
            glog("RGB not initialized\n");
            status_display_show_status("RGB Not Ready");
            return;
        }
        xTaskCreate(knightrider_task, "knightrider_effect", 2048, &rgb_manager, 5, &rgb_effect_task_handle);
        last_effect_is_rainbow = false;
        glog("Knight Rider mode activated\n");
        status_display_show_status("RGB Knight");
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
            RGBMode mode;
        } color_t;
        static const color_t supported_colors[] = {
            { "red",         255, 0,   0,   RGB_MODE_RED },
            { "green",       0,   255, 0,   RGB_MODE_GREEN },
            { "blue",        0,   0,   255, RGB_MODE_BLUE },
            { "yellow",      255, 255, 0,   RGB_MODE_YELLOW },
            { "twh-purple",  115, 0,   225, RGB_MODE_PURPLE }, // #7300E1
            { "cyan",        0,   255, 255, RGB_MODE_CYAN },
            { "orange",      255, 165, 0,   RGB_MODE_ORANGE },
            { "white",       255, 255, 255, RGB_MODE_WHITE },
            { "pink",        255, 192, 203, RGB_MODE_PINK }
        };
        const int num_colors = sizeof(supported_colors) / sizeof(supported_colors[0]);
        int found = 0;
        uint8_t r, g, b;
        RGBMode chosen_mode = RGB_MODE_NORMAL;
        for (int i = 0; i < num_colors; i++) {
            // Use case-insensitive compare.
            if (strcasecmp(argv[1], supported_colors[i].name) == 0) {
                r = supported_colors[i].r;
                g = supported_colors[i].g;
                b = supported_colors[i].b;
                chosen_mode = supported_colors[i].mode;
                found = 1;
                break;
            }
        }
        if (!found) {
            glog("Unknown color '%s'. Supported colors: red, green, blue, yellow, twh-purple, cyan, orange, white, pink.\n", argv[1]);
            status_display_show_status("Color Invalid");
            return;
        }
        // Set each LED to the selected static color.
        for (int i = 0; i < rgb_manager.num_leds; i++) {
            rgb_manager_set_color(&rgb_manager, i, r, g, b, false);
        }
        led_strip_refresh(rgb_manager.strip);
        // Persist selection so it remains active after other effects/off are toggled
        settings_set_rgb_mode(&G_Settings, chosen_mode);
        settings_save(&G_Settings);
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

    int num_leds = settings_get_rgb_led_count(&G_Settings);
    if (num_leds <= 0) {
        if (rgb_manager.num_leds > 0) {
            num_leds = rgb_manager.num_leds;
        } else if (CONFIG_NUM_LEDS > 0) {
            num_leds = CONFIG_NUM_LEDS;
        } else {
            num_leds = 1;
        }
    }

    esp_err_t ret;
    if (red_pin == green_pin && green_pin == blue_pin) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, red_pin, num_leds, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, red_pin);
            settings_set_rgb_separate_pins(&G_Settings, -1, -1, -1);
            settings_save(&G_Settings);
            glog("Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            status_display_show_status("RGB Single");
        } else {
            glog("Failed to init RGB on pin %d: %s\n", red_pin, esp_err_to_name(ret));
            status_display_show_status("RGB Init Fail");
        }
    } else {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, num_leds, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               red_pin, green_pin, blue_pin);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, -1);
            settings_set_rgb_separate_pins(&G_Settings, red_pin, green_pin, blue_pin);
            settings_save(&G_Settings);
            glog("RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            status_display_show_status("RGB Pins Set");
        } else {
            glog("Failed to init RGB pins R:%d G:%d B:%d: %s\n", red_pin, green_pin, blue_pin, esp_err_to_name(ret));
            status_display_show_status("RGB Init Fail");
        }
    }
}

void handle_setrgbcount(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: setrgbcount <1-512>\n");
        status_display_show_status("Count Usage");
        return;
    }

    int count = atoi(argv[1]);
    if (count < 1 || count > 512) {
        glog("RGB LED count must be between 1 and 512\n");
        status_display_show_status("Count Invalid");
        return;
    }

    settings_set_rgb_led_count(&G_Settings, (uint16_t)count);

    int32_t data_pin = settings_get_rgb_data_pin(&G_Settings);
    int32_t red_pin, green_pin, blue_pin;
    settings_get_rgb_separate_pins(&G_Settings, &red_pin, &green_pin, &blue_pin);

    esp_err_t ret = ESP_OK;
    bool attempted_reinit = false;

    if (data_pin != GPIO_NUM_NC) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, (gpio_num_t)data_pin, count, LED_PIXEL_FORMAT_GRB,
                               LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        attempted_reinit = true;
    } else if (red_pin != GPIO_NUM_NC && green_pin != GPIO_NUM_NC && blue_pin != GPIO_NUM_NC) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, count, LED_PIXEL_FORMAT_GRB,
                               LED_MODEL_WS2812, (gpio_num_t)red_pin, (gpio_num_t)green_pin, (gpio_num_t)blue_pin);
        attempted_reinit = true;
    }

    settings_save(&G_Settings);

    if (attempted_reinit && ret == ESP_OK) {
        glog("RGB LED count set to %d and applied.\n", count);
        status_display_show_status("RGB Count Set");
    } else if (attempted_reinit) {
        glog("RGB count saved but reinit failed: %s\n", esp_err_to_name(ret));
        status_display_show_status("RGB Reinit NG");
    } else {
        glog("RGB count set to %d. Configure pins with setrgbpins to apply.\n", count);
        status_display_show_status("RGB Count Saved");
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

#define SD_CLI_MAX_ENTRIES 128
static char *g_sd_cli_paths[SD_CLI_MAX_ENTRIES];
static uint8_t g_sd_cli_types[SD_CLI_MAX_ENTRIES];
static size_t g_sd_cli_count = 0;

static void sd_cli_clear_index(void) {
    for (size_t i = 0; i < g_sd_cli_count; ++i) {
        free(g_sd_cli_paths[i]);
        g_sd_cli_paths[i] = NULL;
    }
    g_sd_cli_count = 0;
}

static bool sd_cli_is_number(const char *s) {
    if (!s || !*s) return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

static const char *sd_cli_resolve_path(const char *arg, char *buf, size_t bufsize) {
    if (sd_cli_is_number(arg) && g_sd_cli_count > 0) {
        int idx = atoi(arg);
        if (idx >= 0 && (size_t)idx < g_sd_cli_count) {
            strncpy(buf, g_sd_cli_paths[idx], bufsize - 1);
            buf[bufsize - 1] = '\0';
            return buf;
        }
        return NULL;
    }
    if (arg[0] == '/') {
        strncpy(buf, arg, bufsize - 1);
    } else {
        snprintf(buf, bufsize, "/mnt/ghostesp/%s", arg);
    }
    buf[bufsize - 1] = '\0';
    return buf;
}

static bool sd_cli_jit_mounted = false;
static bool sd_cli_display_suspended = false;

static bool sd_cli_ensure_mounted(void) {
    if (sd_card_manager.is_initialized) return true;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (sd_card_mount_for_flush(&sd_cli_display_suspended) == ESP_OK) {
            sd_cli_jit_mounted = true;
            return true;
        }
    }
#endif
    return false;
}

static void sd_cli_cleanup(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (sd_cli_jit_mounted) {
        sd_card_unmount_after_flush(sd_cli_display_suspended);
        sd_cli_jit_mounted = false;
        sd_cli_display_suspended = false;
    }
#endif
}

void handle_sd_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("SD:USAGE\n");
        glog("  sd status                        - Show SD card status\n");
        glog("  sd list [path]                   - List files/dirs with indices\n");
        glog("  sd info <idx|path>               - Show file/dir info\n");
        glog("  sd size <idx|path>               - Get file size\n");
        glog("  sd read <idx|path> [off] [len]   - Read file (offset, length)\n");
        glog("  sd write <path> <base64>         - Write base64 data to file\n");
        glog("  sd append <path> <base64>        - Append base64 data to file\n");
        glog("  sd mkdir <path>                  - Create directory\n");
        glog("  sd rm <idx|path>                 - Delete file or empty directory\n");
        glog("  sd tree [path] [depth]           - Recursive listing\n");
        return;
    }

    const char *sub = argv[1];
    char path[256];

    if (strcmp(sub, "status") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:STATUS:mounted=false\n");
            sd_cli_cleanup();
            return;
        }
        glog("SD:STATUS:mounted=true\n");
        if (sd_card_is_virtual_storage()) {
            glog("SD:STATUS:type=virtual\n");
        } else if (sd_card_manager.card) {
            glog("SD:STATUS:type=physical\n");
            glog("SD:STATUS:name=%s\n", sd_card_manager.card->cid.name);
            uint64_t cap_mb = ((uint64_t)sd_card_manager.card->csd.capacity * 
                               sd_card_manager.card->csd.sector_size) / (1024 * 1024);
            glog("SD:STATUS:capacity_mb=%llu\n", (unsigned long long)cap_mb);
        }
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info("/mnt", &total, &free_bytes) == ESP_OK && total > 0) {
            glog("SD:STATUS:total=%llu\n", (unsigned long long)total);
            glog("SD:STATUS:free=%llu\n", (unsigned long long)free_bytes);
            glog("SD:STATUS:total_mb=%llu\n", (unsigned long long)(total / (1024 * 1024)));
            glog("SD:STATUS:free_mb=%llu\n", (unsigned long long)(free_bytes / (1024 * 1024)));
            glog("SD:STATUS:used_pct=%d\n", (int)(((total - free_bytes) * 100) / total));
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "list") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        const char *list_path = (argc >= 3) ? argv[2] : "/mnt/ghostesp";
        if (list_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", list_path);
        } else {
            strncpy(path, list_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        DIR *d = opendir(path);
        if (!d) {
            glog("SD:ERR:cannot_open:%s\n", path);
            sd_cli_clear_index();
            return;
        }

        sd_cli_clear_index();
        glog("SD:LIST:%s\n", path);

        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

            struct stat st;
            bool is_dir = false;
            long fsize = 0;

            if (stat(fullpath, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                fsize = is_dir ? 0 : (long)st.st_size;
            } else if (entry->d_type == DT_DIR) {
                is_dir = true;
            }

            int idx = (int)g_sd_cli_count;
            if (g_sd_cli_count < SD_CLI_MAX_ENTRIES) {
                g_sd_cli_paths[g_sd_cli_count] = strdup(fullpath);
                g_sd_cli_types[g_sd_cli_count] = is_dir ? 1 : 0;
                if (g_sd_cli_paths[g_sd_cli_count]) g_sd_cli_count++;
            }

            if (is_dir) {
                glog("SD:DIR:[%d] %s\n", idx, entry->d_name);
            } else {
                glog("SD:FILE:[%d] %s %ld\n", idx, entry->d_name, fsize);
            }
        }
        closedir(d);

        if (g_sd_cli_count == 0) {
            glog("SD:EMPTY\n");
        }
        glog("SD:OK:listed %zu entries\n", g_sd_cli_count);
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "info") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }

        glog("SD:INFO:path=%s\n", resolved);
        glog("SD:INFO:type=%s\n", S_ISDIR(st.st_mode) ? "dir" : "file");
        glog("SD:INFO:size=%ld\n", (long)st.st_size);
        glog("SD:OK\n");
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "cat") == 0 || strcmp(sub, "read") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        bool raw_output = (strcmp(sub, "cat") == 0);
        long offset = 0;
        size_t max_bytes = 0;
        int numeric_arg_count = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--raw") == 0) {
                raw_output = true;
                continue;
            }

            char *endptr = NULL;
            long v = strtol(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0') {
                glog("SD:ERR:invalid_arg:%s\n", argv[i]);
                sd_cli_cleanup();
                return;
            }

            if (numeric_arg_count == 0) {
                offset = v;
                if (offset < 0) offset = 0;
            } else if (numeric_arg_count == 1) {
                if (v > 0) {
                    max_bytes = (size_t)v;
                }
            } else {
                glog("SD:ERR:too_many_args\n");
                sd_cli_cleanup();
                return;
            }
            numeric_arg_count++;
        }

        FILE *f = fopen(resolved, "rb");
        if (!f) {
            glog("SD:ERR:cannot_open:%s\n", resolved);
            return;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        if (offset > file_size) offset = file_size;
        fseek(f, offset, SEEK_SET);

        if (max_bytes == 0 || max_bytes > (size_t)(file_size - offset)) {
            max_bytes = (size_t)(file_size - offset);
        }

        if (!raw_output) {
            glog("SD:READ:BEGIN:%s\n", resolved);
            glog("SD:READ:SIZE:%ld\n", file_size);
            glog("SD:READ:OFFSET:%ld\n", offset);
            glog("SD:READ:LENGTH:%zu\n", max_bytes);
        }

        char *buf = malloc(1024);
        if (!buf) {
            fclose(f);
            glog("SD:ERR:oom\n");
            return;
        }

        size_t total_read = 0;
        size_t n;
        bool read_failed = false;
        while (total_read < max_bytes && (n = fread(buf, 1, 1024, f)) > 0) {
            size_t to_write = n;
            if (total_read + to_write > max_bytes) {
                to_write = max_bytes - total_read;
            }
            size_t out_written = fwrite(buf, 1, to_write, stdout);
            if (out_written != to_write) {
                if (raw_output) {
                    glog("SD:ERR:stdout_write_failed\n");
                } else {
                    glog("\nSD:ERR:stdout_write_failed\n");
                }
                read_failed = true;
                break;
            }
            total_read += to_write;
        }
        if (ferror(f)) {
            if (raw_output) {
                glog("SD:ERR:file_read_failed\n");
            } else {
                glog("\nSD:ERR:file_read_failed\n");
            }
            read_failed = true;
        }
        free(buf);
        fclose(f);

        if (!raw_output) {
            glog("\nSD:READ:END:bytes=%zu\n", total_read);
            if (!read_failed) {
                glog("SD:OK\n");
            }
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "write") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 4) {
            glog("SD:ERR:usage: sd write <path> <base64data>\n");
            sd_cli_cleanup();
            return;
        }
        const char *write_path = argv[2];
        if (write_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", write_path);
        } else {
            strncpy(path, write_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        const char *b64data = argv[3];
        size_t b64len = strlen(b64data);
        size_t decoded_len = (b64len * 3) / 4 + 4;
        unsigned char *decoded = malloc(decoded_len);
        if (!decoded) {
            glog("SD:ERR:oom\n");
            sd_cli_cleanup();
            return;
        }

        size_t olen = 0;
        int ret = mbedtls_base64_decode(decoded, decoded_len, &olen, (const unsigned char *)b64data, b64len);
        if (ret != 0) {
            free(decoded);
            glog("SD:ERR:base64_decode_failed\n");
            sd_cli_cleanup();
            return;
        }

        FILE *f = fopen(path, "wb");
        if (!f) {
            free(decoded);
            glog("SD:ERR:cannot_create:%s\n", path);
            sd_cli_cleanup();
            return;
        }

        size_t written = fwrite(decoded, 1, olen, f);
        int write_failed = ferror(f);
        fclose(f);
        free(decoded);

        glog("SD:WRITE:bytes=%zu\n", written);
        if (write_failed || written != olen) {
            glog("SD:ERR:short_write:%s\n", path);
        } else {
            glog("SD:OK:created:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "append") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 4) {
            glog("SD:ERR:usage: sd append <path> <base64data>\n");
            sd_cli_cleanup();
            return;
        }
        const char *append_path = argv[2];
        if (append_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", append_path);
        } else {
            strncpy(path, append_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        const char *b64data = argv[3];
        size_t b64len = strlen(b64data);
        size_t decoded_len = (b64len * 3) / 4 + 4;
        unsigned char *decoded = malloc(decoded_len);
        if (!decoded) {
            glog("SD:ERR:oom\n");
            sd_cli_cleanup();
            return;
        }

        size_t olen = 0;
        int ret = mbedtls_base64_decode(decoded, decoded_len, &olen, (const unsigned char *)b64data, b64len);
        if (ret != 0) {
            free(decoded);
            glog("SD:ERR:base64_decode_failed\n");
            sd_cli_cleanup();
            return;
        }

        FILE *f = fopen(path, "ab");
        if (!f) {
            free(decoded);
            glog("SD:ERR:cannot_open:%s\n", path);
            sd_cli_cleanup();
            return;
        }

        size_t written = fwrite(decoded, 1, olen, f);
        int write_failed = ferror(f);
        fclose(f);
        free(decoded);

        glog("SD:APPEND:bytes=%zu\n", written);
        if (write_failed || written != olen) {
            glog("SD:ERR:short_write:%s\n", path);
        } else {
            glog("SD:OK:appended:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "size") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }
        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }
        glog("SD:SIZE:%ld\n", (long)st.st_size);
        glog("SD:OK\n");
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "mkdir") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *mk_path = argv[2];
        if (mk_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", mk_path);
        } else {
            strncpy(path, mk_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        if (mkdir(path, 0777) == 0) {
            glog("SD:OK:created:%s\n", path);
        } else {
            glog("SD:ERR:mkdir_failed:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "rm") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }

        int ret;
        if (S_ISDIR(st.st_mode)) {
            ret = rmdir(resolved);
        } else {
            ret = unlink(resolved);
        }

        if (ret == 0) {
            glog("SD:OK:removed:%s\n", resolved);
        } else {
            glog("SD:ERR:rm_failed:%s\n", resolved);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "tree") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        const char *tree_path = (argc >= 3) ? argv[2] : "/mnt/ghostesp";
        int max_depth = 2;
        if (argc >= 4) {
            int d = atoi(argv[3]);
            if (d > 0 && d <= 10) max_depth = d;
        }

        if (tree_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", tree_path);
        } else {
            strncpy(path, tree_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        glog("SD:TREE:%s\n", path);
        
        typedef struct { char p[256]; int lvl; } stack_item_t;
        stack_item_t *stack = malloc(sizeof(stack_item_t) * 64);
        if (!stack) {
            glog("SD:ERR:oom\n");
            return;
        }
        int sp = 0;
        strncpy(stack[sp].p, path, 255);
        stack[sp].lvl = 0;
        sp++;

        size_t count = 0;
        while (sp > 0 && count < 500) {
            sp--;
            char *cur = stack[sp].p;
            int lvl = stack[sp].lvl;

            DIR *d = opendir(cur);
            if (!d) continue;

            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && count < 500) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

                char full[512];
                snprintf(full, sizeof(full), "%s/%s", cur, entry->d_name);

                struct stat st;
                bool is_dir = false;
                if (stat(full, &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                } else if (entry->d_type == DT_DIR) {
                    is_dir = true;
                }

                for (int i = 0; i < lvl; i++) printf("  ");
                if (is_dir) {
                    printf("[D] %s/\n", entry->d_name);
                    if (lvl + 1 < max_depth && sp < 63) {
                        strncpy(stack[sp].p, full, 255);
                        stack[sp].lvl = lvl + 1;
                        sp++;
                    }
                } else {
                    printf("[F] %s (%ld)\n", entry->d_name, (long)st.st_size);
                }
                count++;
            }
            closedir(d);
        }
        free(stack);
        glog("SD:OK:tree %zu items\n", count);
        sd_cli_cleanup();
        return;
    }

    glog("SD:ERR:unknown_subcommand:%s\n", sub);
    sd_cli_cleanup();
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

    ap_manager_start_services();
    status_display_show_status("ScanAll Done");
}

static int get_next_sweep_file_index(void) {
    int next = 0;
    char path[64];
    while (next < 9999) {
        snprintf(path, sizeof(path), "/mnt/ghostesp/sweeps/sweep_%d.csv", next);
        FILE *f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        next++;
    }
    return next;
}

static const char* sweep_get_auth_str(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Unknown";
    }
}

static const char* sweep_get_cipher_str(wifi_cipher_type_t cipher) {
    switch (cipher) {
        case WIFI_CIPHER_TYPE_NONE: return "None";
        case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
        case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
        case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
        case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
        case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP/CCMP";
        case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
        case WIFI_CIPHER_TYPE_GCMP256: return "GCMP256";
        default: return "Unknown";
    }
}

static void sweep_get_phy_modes(wifi_ap_record_t *ap, char *buf, size_t len) {
    buf[0] = '\0';
    if (ap->phy_11ax) strcat(buf, "ax/");
    if (ap->phy_11ac) strcat(buf, "ac/");
    if (ap->phy_11n) strcat(buf, "n/");
    if (ap->phy_11a) strcat(buf, "a/");
    if (ap->phy_11g) strcat(buf, "g/");
    if (ap->phy_11b) strcat(buf, "b/");
    size_t l = strlen(buf);
    if (l > 0) buf[l - 1] = '\0';
}

static void sweep_write_csv_escaped(FILE *f, const char *str) {
    bool needs_quote = false;
    for (const char *p = str; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n') { needs_quote = true; break; }
    }
    if (needs_quote) {
        fputc('"', f);
        for (const char *p = str; *p; p++) {
            if (*p == '"') fputc('"', f);
            fputc(*p, f);
        }
        fputc('"', f);
    } else {
        fputs(str, f);
    }
}

void handle_sweep_cmd(int argc, char **argv) {
    int wifi_seconds = 10;
    int ble_seconds = 10;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            wifi_seconds = atoi(argv[++i]);
            if (wifi_seconds < 1) wifi_seconds = 10;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            ble_seconds = atoi(argv[++i]);
            if (ble_seconds < 1) ble_seconds = 10;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "help") == 0) {
            glog("Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
            glog("  -w: WiFi scan duration per phase (default 10s)\n");
            glog("  -b: BLE scan duration per phase (default 10s)\n");
            glog("Performs AP scan, STA scan, BLE scans and saves to SD.\n");
            return;
        }
    }
    
    glog("=== Starting Full Environment Sweep ===\n");
    status_display_show_status("Sweep Start");
    
    FILE *report = NULL;
    char report_path[64] = {0};
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32] = "";
    if (tm_info && tm_info->tm_year >= 120) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }
    
    int open_networks = 0, weak_networks = 0, secure_networks = 0;
    
    if (sd_card_exists("/mnt/ghostesp")) {
        mkdir("/mnt/ghostesp/sweeps", 0755);
        int idx = get_next_sweep_file_index();
        snprintf(report_path, sizeof(report_path), "/mnt/ghostesp/sweeps/sweep_%d.csv", idx);
        report = fopen(report_path, "w");
        if (report) {
            fprintf(report, "Type,Name,MAC,Associated MAC,Channel,Frequency,RSSI,Auth,Cipher,802.11,WPS,Latitude,Longitude,Altitude,First Seen\n");
            glog("Saving report to: %s\n", report_path);
        }
    }
    
    // --- WiFi AP Scan ---
    glog("\n--- Phase 1: WiFi AP Scan (%ds) ---\n", wifi_seconds);
    
    wifi_manager_start_scan_with_time(wifi_seconds);
    
    uint16_t ap_cnt = 0;
    wifi_ap_record_t *aps = NULL;
    wifi_manager_get_scan_results_data(&ap_cnt, &aps);
    
    glog("Found %d access points\n", ap_cnt);
    
    uint16_t limit = ap_cnt > 100 ? 100 : ap_cnt;
    for (uint16_t i = 0; i < limit && aps; i++) {
        char ssid_safe[33];
        strncpy(ssid_safe, (char*)aps[i].ssid, 32);
        ssid_safe[32] = '\0';
        for (int j = 0; ssid_safe[j]; j++) {
            if (ssid_safe[j] < 32 || ssid_safe[j] > 126) ssid_safe[j] = '?';
        }
        if (ssid_safe[0] == '\0') strcpy(ssid_safe, "");
        
        const char *auth = sweep_get_auth_str(aps[i].authmode);
        const char *cipher = sweep_get_cipher_str(aps[i].pairwise_cipher);
        char phy_modes[24];
        sweep_get_phy_modes(&aps[i], phy_modes, sizeof(phy_modes));
        int freq = aps[i].primary > 14 ? 5000 + (aps[i].primary * 5) : 2407 + (aps[i].primary * 5);
        
        if (aps[i].authmode == WIFI_AUTH_OPEN) open_networks++;
        else if (aps[i].authmode == WIFI_AUTH_WEP || aps[i].authmode == WIFI_AUTH_WPA_PSK) weak_networks++;
        else secure_networks++;
        
        if (report) {
            fprintf(report, "WiFi AP,");
            sweep_write_csv_escaped(report, ssid_safe);
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,%d,%d,%d,%s,%s,%s,%s,",
                    aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                    aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                    aps[i].primary, freq, aps[i].rssi, auth, cipher, phy_modes,
                    aps[i].wps ? "Yes" : "No");
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
    
    // --- WiFi Station Scan ---
    glog("\n--- Phase 2: WiFi Station Scan (%ds) ---\n", wifi_seconds);
    
    station_count = 0;
    wifi_manager_start_station_scan();
    vTaskDelay(pdMS_TO_TICKS(wifi_seconds * 1000));
    wifi_manager_stop_monitor_mode();
    
    glog("Found %d stations\n", station_count);
    
    for (int i = 0; i < station_count; i++) {
        if (report) {
            fprintf(report, "WiFi Client,,%02X:%02X:%02X:%02X:%02X:%02X,%02X:%02X:%02X:%02X:%02X:%02X,,,,,,,",
                    station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1],
                    station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3],
                    station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
                    station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1],
                    station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3],
                    station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
    
#ifndef CONFIG_IDF_TARGET_ESP32S2
    // --- BLE Scans ---
    glog("\n--- Phase 3: BLE Flipper Scan (%ds) ---\n", ble_seconds);
    
    flipper_scan_start();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    flipper_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    flipper_scan_print_results();
    
    int flipper_cnt = flipper_scan_get_count();
    for (int i = 0; i < flipper_cnt && report; i++) {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (flipper_scan_get_device_data(i, mac, &rssi, name, sizeof(name)) == 0) {
            fprintf(report, "Flipper,");
            sweep_write_csv_escaped(report, name[0] ? name : "");
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,,,%d,,,,",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
    
    glog("\n--- Phase 4: BLE GATT Device Scan (%ds) ---\n", ble_seconds);
    
    ble_start_gatt_scan();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    ble_stop_gatt_scan();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ble_list_gatt_devices();
    
    int gatt_cnt = ble_get_gatt_device_count();
    for (int i = 0; i < gatt_cnt && report; i++) {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (ble_get_gatt_device_data(i, mac, &rssi, name, sizeof(name)) == 0) {
            fprintf(report, "BLE Device,");
            sweep_write_csv_escaped(report, name[0] ? name : "");
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,,,%d,,,,",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
    
    glog("\n--- Phase 5: BLE Raw Packet Scan (%ds) ---\n", ble_seconds);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ble_start_raw_ble_packetscan();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    ble_stop();
#endif
    
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    glog("\n--- Phase 6: 802.15.4 Scan (%ds) ---\n", ble_seconds);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    zigbee_manager_clear_devices();
    zigbee_manager_start_capture(0);
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    zigbee_manager_stop_capture();
    
    int zb_cnt = zigbee_manager_get_device_count();
    glog("Found %d 802.15.4 devices\n", zb_cnt);
    
    for (int i = 0; i < zb_cnt && report; i++) {
        zigbee_device_t dev;
        if (zigbee_manager_get_device_data(i, &dev) == 0) {
            fprintf(report, "802.15.4,");
            if (dev.addr_len == 8) {
                fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X,,%d,,%d,,,,",
                        dev.addr[0], dev.addr[1], dev.addr[2], dev.addr[3],
                        dev.addr[4], dev.addr[5], dev.addr[6], dev.addr[7],
                        dev.channel, dev.rssi);
            } else {
                fprintf(report, ",%02X:%02X,,%d,,%d,,,,",
                        dev.addr[0], dev.addr[1], dev.channel, dev.rssi);
            }
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
#endif
    
    if (report) {
        fclose(report);
        glog("\nReport saved to: %s\n", report_path);
    }
    
    ap_manager_start_services();
    glog("\n=== Sweep Complete ===\n");
    glog("WiFi: %d APs, %d stations | Security: %d open, %d weak, %d secure\n", 
         ap_cnt, station_count, open_networks, weak_networks, secure_networks);
    status_display_show_status("Sweep Done");
}

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
    flipper_scan_print_results();
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
        flipper_scan_select(num);
        status_display_show_status("Flipper Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("Flipper Bad");
    }
}

void handle_list_gatt_cmd(int argc, char **argv) {
    ble_list_gatt_devices();
    status_display_show_status("List GATT");
}

void handle_select_gatt_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectgatt <index>\n");
        status_display_show_status("GATT Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_gatt_device(num);
        status_display_show_status("GATT Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("GATT Bad");
    }
}

void handle_enum_gatt_cmd(int argc, char **argv) {
    ble_enumerate_gatt_services();
    status_display_show_status("GATT Enum");
}

void handle_track_gatt_cmd(int argc, char **argv) {
    ble_track_gatt_device();
}

#endif

void handle_track_ap_cmd(int argc, char **argv) {
    wifi_manager_track_ap();
}

void handle_track_sta_cmd(int argc, char **argv) {
    wifi_manager_track_sta();
}

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
        dhcp_starvation_help();
        status_display_show_status("DHCP Usage");
    } else if (strcmp(argv[1], "start") == 0) {
        int thr = (argc >= 3) ? atoi(argv[2]) : 1;
        dhcp_starvation_start(thr);
        status_display_show_status("DHCP Start");
    } else if (strcmp(argv[1], "stop") == 0) {
        dhcp_starvation_stop();
        status_display_show_status("DHCP Stop");
    } else if (strcmp(argv[1], "display") == 0) {
        dhcp_starvation_display();
        status_display_show_status("DHCP Stats");
    } else {
        dhcp_starvation_help();
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

    char cc_upper[4];
    if (!str_copy_upper(cc_upper, sizeof(cc_upper), argv[1])) {
        glog("failed to set country: invalid country code\n");
        status_display_show_status("Country Fail");
        return;
    }

    esp_err_t err = esp_wifi_set_country_code(cc_upper, true);
    if (err == ESP_OK) {
        glog("country set to %s\n", cc_upper);
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

void handle_webuiap_cmd(int argc, char **argv) {
    bool enabled = settings_get_webui_restrict_to_ap(&G_Settings);

    if (argc == 1) {
        enabled = !enabled;
        settings_set_webui_restrict_to_ap(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("WebUI AP-only restriction %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            enabled = true;
        } else if (strcmp(argv[1], "off") == 0) {
            enabled = false;
        } else if (strcmp(argv[1], "toggle") == 0) {
            enabled = !enabled;
        } else if (strcmp(argv[1], "status") == 0) {
            glog("WebUI AP-only restriction is %s.\n", enabled ? "enabled" : "disabled");
            return;
        } else {
            glog("Usage: webuiap [on|off|toggle|status]\n");
            return;
        }

        settings_set_webui_restrict_to_ap(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("WebUI AP-only restriction %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    glog("Usage: webuiap [on|off|toggle|status]\n");
}

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
    
    if (state == COMM_STATE_SCANNING) {
        glog("Already in discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Scanning");
        return;
    }
    
    if (state == COMM_STATE_CONNECTED) {
        glog("Connected to peer. Ready to send commands.\n");
        status_display_show_status("Comm Connected");
        return;
    }
    
    glog("Not connected. Use 'commdiscovery' to find peers.\n");
    status_display_show_status("Comm Idle");
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
    
#ifdef CONFIG_WITH_ETHERNET
    if (eth_comm_handler_handle_command(command, data)) {
        return;
    }

    if (strcmp(command, "stop") == 0) {
        g_eth_scan_cancel = true;
    }
#endif
    
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
    vTaskDelay(pdMS_TO_TICKS(50));
    
    glog("[CHIPINFO_START]\n");
    
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
    glog("  Firmware: %s %s %s\n", GHOSTESP_NAME, GHOSTESP_FLAVOR, GHOSTESP_VERSION);
#ifdef GIT_COMMIT_HASH
    glog("  Git Commit: %s\n", GIT_COMMIT_HASH);
#endif
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

    glog("\n  Enabled Features:\n");
#ifdef CONFIG_WITH_SCREEN
    glog("    Display\n");
#endif
#ifdef CONFIG_USE_TOUCHSCREEN
    glog("    Touchscreen\n");
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    glog("    Status Display (OLED)\n");
#endif
#ifdef CONFIG_HAS_NFC
    glog("    NFC\n");
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    glog("    BadUSB\n");
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    glog("    NRF24\n");
#endif
#ifdef CONFIG_HAS_INFRARED
    glog("    Infrared TX\n");
#endif
#ifdef CONFIG_HAS_INFRARED_RX
    glog("    Infrared RX\n");
#endif
#ifdef CONFIG_HAS_GPS
    glog("    GPS\n");
#endif
#ifdef CONFIG_WITH_ETHERNET
    glog("    Ethernet\n");
#endif
#ifdef CONFIG_HAS_BATTERY
    glog("    Battery (Power Save)\n");
#endif
#ifdef CONFIG_HAS_BATTERY_ADC
    glog("    Battery ADC\n");
#endif
#ifdef CONFIG_HAS_FUEL_GAUGE
    glog("    Fuel Gauge\n");
#endif
#ifdef CONFIG_HAS_RTC_CLOCK
    glog("    RTC Clock\n");
#endif
#ifdef CONFIG_HAS_COMPASS
    glog("    Compass\n");
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    glog("    Accelerometer\n");
#endif
#ifdef CONFIG_USE_JOYSTICK
    glog("    Joystick\n");
#endif
#ifdef CONFIG_USE_CARDPUTER
    glog("    Cardputer\n");
#endif
#ifdef CONFIG_USE_TDECK
    glog("    T-Deck\n");
#endif
#ifdef CONFIG_USE_ENCODER
    glog("    Rotary Encoder\n");
#endif
#ifdef CONFIG_USE_USB_KEYBOARD
    glog("    USB Keyboard (Host)\n");
#endif
#ifdef CONFIG_IS_GHOST_BOARD
    glog("    Ghost Board\n");
#endif
#ifdef CONFIG_IS_S3TWATCH
    glog("    S3TWatch\n");
#endif
#ifdef CONFIG_USING_SPI
    glog("    SD Card (SPI)\n");
#endif
#if defined(CONFIG_USING_MMC) || defined(CONFIG_USING_MMC_1_BIT)
    glog("    SD Card (MMC)\n");
#endif
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    glog("    Core Dump\n");
#endif
    
    glog("[CHIPINFO_END]\n");
    
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
        glog("    auto_save_scans   - Auto save scan results to SD (true/false)\n");
        glog("  Custom Settings:\n");
        glog("    flappy_name       - Flappy Ghost name\n");
        glog("    timezone          - Selected timezone\n");
        glog("    accent_color      - Accent color (hex)\n");
        glog("  IO expander buttons (P10/P11/P12):\n");
        glog("    io_btn_p10_cmd    - Command to run when P10 pressed (empty = joystick)\n");
        glog("    io_btn_p11_cmd    - Command to run when P11 pressed (empty = joystick)\n");
        glog("    io_btn_p12_cmd    - Command to run when P12 pressed (empty = joystick)\n");
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

#ifdef CONFIG_NFC_CHAMELEON
void handle_chameleon_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: chameleon <command>\n");
        printf("Commands:\n");
        printf("Connection:\n");
        printf("  connect [timeout] [pin] - Connect to Chameleon Ultra (default timeout: 10s)\n");
        printf("  disconnect        - Disconnect from Chameleon Ultra\n");
        printf("  status           - Check connection status\n");
        printf("Device Info:\n");
        printf("  firmware         - Get firmware version\n");
        printf("  devicemode       - Get current device mode\n");
        printf("  activeslot       - Get active slot number\n");
        printf("  setslot <1-8>    - Set active slot number\n");
        printf("  slotinfo <1-8>   - Get slot information\n");
        printf("  battery          - Get battery information\n");
        printf("Scanning:\n");
        printf("  scanhf           - Scan for HF tags\n");
        printf("  scanlf           - Scan for LF EM410X tags\n");
        printf("  scanlfall        - Scan for all LF tag types\n");
        printf("  scanhidprox      - Scan for HID Prox tags\n");
        printf("MIFARE Classic:\n");
        printf("  mfdetect         - Detect MIFARE Classic support\n");
        printf("  mfprng           - Detect MIFARE Classic PRNG type\n");
        printf("NTAG Cards:\n");
        printf("  ntagdetect       - Detect and identify NTAG card type\n");
        printf("  ntagdump         - Dump complete NTAG card data\n");
        printf("  saventag [filename] - Save NTAG dump to SD card\n");
        printf("Mode Control:\n");
        printf("  reader           - Set to reader mode\n");
        printf("  emulator         - Set to emulator mode\n");
        printf("Data Management:\n");
        printf("  savehf [filename] - Save last HF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        printf("  savelf [filename] - Save last LF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        printf("  readhf           - Basic MIFARE Classic card detection and information collection\n");
        printf("  savedump [filename] - Save last card dump to SD card\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: chameleon <command>\n");
        TERMINAL_VIEW_ADD_TEXT("Commands:\n");
        TERMINAL_VIEW_ADD_TEXT("Connection:\n");
        TERMINAL_VIEW_ADD_TEXT("  connect [timeout] [pin] - Connect to Chameleon Ultra (default timeout: 10s)\n");
        TERMINAL_VIEW_ADD_TEXT("  disconnect        - Disconnect from Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("  status           - Check connection status\n");
        TERMINAL_VIEW_ADD_TEXT("Device Info:\n");
        TERMINAL_VIEW_ADD_TEXT("  firmware         - Get firmware version\n");
        TERMINAL_VIEW_ADD_TEXT("  devicemode       - Get current device mode\n");
        TERMINAL_VIEW_ADD_TEXT("  activeslot       - Get active slot number\n");
        TERMINAL_VIEW_ADD_TEXT("  setslot <1-8>    - Set active slot number\n");
        TERMINAL_VIEW_ADD_TEXT("  slotinfo <1-8>   - Get slot information\n");
        TERMINAL_VIEW_ADD_TEXT("  battery          - Get battery information\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning:\n");
        TERMINAL_VIEW_ADD_TEXT("  scanhf           - Scan for HF tags\n");
        TERMINAL_VIEW_ADD_TEXT("  scanlf           - Scan for LF EM410X tags\n");
        TERMINAL_VIEW_ADD_TEXT("  scanlfall        - Scan for all LF tag types\n");
        TERMINAL_VIEW_ADD_TEXT("  scanhidprox      - Scan for HID Prox tags\n");
        TERMINAL_VIEW_ADD_TEXT("MIFARE Classic:\n");
        TERMINAL_VIEW_ADD_TEXT("  mfdetect         - Detect MIFARE Classic support\n");
        TERMINAL_VIEW_ADD_TEXT("  mfprng           - Detect MIFARE Classic PRNG type\n");
        TERMINAL_VIEW_ADD_TEXT("NTAG Cards:\n");
        TERMINAL_VIEW_ADD_TEXT("  ntagdetect       - Detect and identify NTAG card type\n");
        TERMINAL_VIEW_ADD_TEXT("  ntagdump         - Dump complete NTAG card data\n");
        TERMINAL_VIEW_ADD_TEXT("  saventag [filename] - Save NTAG dump to SD card\n");
        TERMINAL_VIEW_ADD_TEXT("Mode Control:\n");
        TERMINAL_VIEW_ADD_TEXT("  reader           - Set to reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("  emulator         - Set to emulator mode\n");
        TERMINAL_VIEW_ADD_TEXT("Data Management:\n");
        TERMINAL_VIEW_ADD_TEXT("  savehf [filename] - Save last HF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        TERMINAL_VIEW_ADD_TEXT("  savelf [filename] - Save last LF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        TERMINAL_VIEW_ADD_TEXT("  readhf           - Basic MIFARE Classic card detection and information collection\n");
        TERMINAL_VIEW_ADD_TEXT("  savedump [filename] - Save last card dump to SD card\n");
        return;
    }

    const char *subcommand = argv[1];

    if (strcmp(subcommand, "connect") == 0) {
        uint32_t timeout = 10; // Default timeout of 10 seconds
        const char* pin = NULL;
        
        // Parse arguments: connect [timeout] [pin]
        if (argc > 2) {
            // Check if second argument is a number (timeout) or PIN
            if (strlen(argv[2]) <= 2 && atoi(argv[2]) > 0) {
                // Second argument is timeout
                timeout = (uint32_t)atoi(argv[2]);
                if (timeout == 0) {
                    timeout = 10;
                }
                // Check for PIN as third argument
                if (argc > 3) {
                    pin = argv[3];
                }
            } else {
                // Second argument is PIN, use default timeout
                pin = argv[2];
            }
        }
        
        if (pin != NULL) {
            printf("Connecting to Chameleon Ultra with %lu second timeout and PIN...\n", (unsigned long)timeout);
            TERMINAL_VIEW_ADD_TEXT("Connecting to Chameleon Ultra with PIN...\n");
        } else {
            printf("Connecting to Chameleon Ultra with %lu second timeout...\n", (unsigned long)timeout);
            TERMINAL_VIEW_ADD_TEXT("Connecting to Chameleon Ultra...\n");
        }
        
        chameleon_manager_connect(timeout, pin);
    }
    else if (strcmp(subcommand, "disconnect") == 0) {
        printf("Disconnecting from Chameleon Ultra...\n");
        TERMINAL_VIEW_ADD_TEXT("Disconnecting from Chameleon Ultra...\n");
        chameleon_manager_disconnect();
    }
    else if (strcmp(subcommand, "status") == 0) {
        if (chameleon_manager_is_connected()) {
            printf("Status: Connected to Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Status: Connected to Chameleon Ultra\n");
        } else {
            printf("Status: Not connected to Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Status: Not connected to Chameleon Ultra\n");
        }
    }
    else if (strcmp(subcommand, "scanhf") == 0) {
        bool skip_dict = false;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--skip-dict") == 0 || strcmp(argv[i], "--skipdict") == 0) {
                skip_dict = true;
            } else {
                printf("Unknown option for scanhf: %s\n", argv[i]);
                TERMINAL_VIEW_ADD_TEXT("Unknown option for scanhf\n");
                return;
            }
        }

        if (!chameleon_manager_scan_hf()) {
            return;
        }

        bool classic_tag = false;
        uint8_t uid_len = 0;
        uint16_t atqa = 0;
        uint8_t sak = 0;
        if (chameleon_manager_get_last_hf_scan(NULL, &uid_len, &atqa, &sak)) {
            if (sak == 0x08 || sak == 0x09 || sak == 0x18) {
                classic_tag = true;
            }
        }

        if (classic_tag) {
            chameleon_cli_progress_state_t progress_state = {0};
            progress_state.last_percent = -1;
            chameleon_manager_set_progress_callback(chameleon_cli_progress_cb, &progress_state);

            if (skip_dict) {
                glog("Reading MIFARE Classic without dictionary brute-force...\n");
                glog("Dictionary brute-force skipped by user flag.\n");
            } else {
                glog("Reading MIFARE Classic with dictionary brute-force...\n");
            }

            bool classic_ok = chameleon_manager_mf1_read_classic_with_dict(skip_dict);
            chameleon_manager_set_progress_callback(NULL, NULL);

            if (!classic_ok) {
                glog("MIFARE Classic read failed.\n");
            } else {
                glog("MIFARE Classic read complete.\n");
            }
        } else if (chameleon_manager_last_scan_is_ntag()) {
            glog("Refreshing NTAG cache...\n");

            if (!chameleon_manager_read_ntag_card()) {
                glog("Failed to read NTAG card.\n");
            } else {
                glog("NTAG read complete.\n");
            }
        }

        const char *details = chameleon_manager_get_cached_details();
        if (details && details[0]) {
            glog("%s\n", details);
        }
    }
    else if (strcmp(subcommand, "scanlf") == 0) {
        chameleon_manager_scan_lf();
    }
    else if (strcmp(subcommand, "scanlfall") == 0) {
        // Try multiple LF scan types
        printf("Scanning for all LF tag types...\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning for all LF tag types...\n");
        
        // First try EM410X
        printf("1. Trying EM410X scan...\n");
        TERMINAL_VIEW_ADD_TEXT("1. Trying EM410X scan...\n");
        if (chameleon_manager_scan_lf()) {
            return;  // Found something, stop here
        }
        
        // Then try HID Prox
        printf("2. Trying HID Prox scan...\n");
        TERMINAL_VIEW_ADD_TEXT("2. Trying HID Prox scan...\n");
        chameleon_manager_scan_hidprox();
    }
    else if (strcmp(subcommand, "battery") == 0) {
        chameleon_manager_get_battery_info();
    }
    else if (strcmp(subcommand, "reader") == 0) {
        chameleon_manager_set_reader_mode();
    }
    else if (strcmp(subcommand, "emulator") == 0) {
        chameleon_manager_set_emulator_mode();
    }
    else if (strcmp(subcommand, "savehf") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_last_hf_scan(filename);
    }
    else if (strcmp(subcommand, "savelf") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_last_lf_scan(filename);
    }
    else if (strcmp(subcommand, "readhf") == 0) {
        chameleon_manager_read_hf_card();
    }
    else if (strcmp(subcommand, "savedump") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_card_dump(filename);
    }
    else if (strcmp(subcommand, "firmware") == 0) {
        chameleon_manager_get_firmware_version();
    }
    else if (strcmp(subcommand, "devicemode") == 0) {
        chameleon_manager_get_device_mode();
    }
    else if (strcmp(subcommand, "activeslot") == 0) {
        chameleon_manager_get_active_slot();
    }
    else if (strcmp(subcommand, "setslot") == 0) {
        if (argc < 3) {
            printf("Usage: chameleon setslot <1-8>\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: chameleon setslot <1-8>\n");
            return;
        }
        uint8_t user_slot = (uint8_t)atoi(argv[2]);
        if (user_slot < 1 || user_slot > 8) {
            printf("Error: Slot must be between 1-8\n");
            TERMINAL_VIEW_ADD_TEXT("Error: Slot must be between 1-8\n");
            return;
        }
        uint8_t device_slot = user_slot - 1; // Convert 1-8 to 0-7
        chameleon_manager_set_active_slot(device_slot);
    }
    else if (strcmp(subcommand, "slotinfo") == 0) {
        if (argc < 3) {
            printf("Usage: chameleon slotinfo <1-8>\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: chameleon slotinfo <1-8>\n");
            return;
        }
        uint8_t user_slot = (uint8_t)atoi(argv[2]);
        if (user_slot < 1 || user_slot > 8) {
            printf("Error: Slot must be between 1-8\n");
            TERMINAL_VIEW_ADD_TEXT("Error: Slot must be between 1-8\n");
            return;
        }
        uint8_t device_slot = user_slot - 1; // Convert 1-8 to 0-7
        chameleon_manager_get_slot_info(device_slot);
    }
    else if (strcmp(subcommand, "scanhidprox") == 0) {
        chameleon_manager_scan_hidprox();
    }
    else if (strcmp(subcommand, "mfdetect") == 0) {
        chameleon_manager_mf1_detect_support();
    }
    else if (strcmp(subcommand, "mfprng") == 0) {
        chameleon_manager_mf1_detect_prng();
    }
    else if (strcmp(subcommand, "ntagdetect") == 0) {
        chameleon_manager_detect_ntag();
    }
    else if (strcmp(subcommand, "ntagdump") == 0) {
        chameleon_manager_read_ntag_card();
    }
    else if (strcmp(subcommand, "saventag") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_ntag_dump(filename);
    }
    else {
        printf("Unknown chameleon command: %s\n", subcommand);
        TERMINAL_VIEW_ADD_TEXT("Unknown chameleon command: %s\n", subcommand);
        printf("Use 'chameleon' without arguments to see available commands.\n");
        TERMINAL_VIEW_ADD_TEXT("Use 'chameleon' without arguments to see available commands.\n");
    }
}
#else
void handle_chameleon_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Chameleon support is disabled in this build.\n");
    TERMINAL_VIEW_ADD_TEXT("Chameleon support is disabled in this build.\n");
}
#endif

#define IR_CLI_MAX_REMOTES 128
static char *g_ir_cli_remote_paths[IR_CLI_MAX_REMOTES];
static size_t g_ir_cli_remote_count = 0;

typedef struct {
    bool use_builtin;
    char path[256];
    char name[64];
    uint32_t delay_ms;
} IrUniversalSendArgs;

typedef enum {
    IR_BG_MODE_RX,
    IR_BG_MODE_LEARN,
} IrRxLearnMode;

typedef struct {
    IrRxLearnMode mode;
    int timeout_sec;
    char path[256];
} IrRxLearnArgs;

static void ir_universal_send_task(void *arg);
static void ir_rx_learn_task(void *arg);

static void ir_cli_clear_remote_index(void) {
    for (size_t i = 0; i < g_ir_cli_remote_count; ++i) {
        free(g_ir_cli_remote_paths[i]);
        g_ir_cli_remote_paths[i] = NULL;
    }
    g_ir_cli_remote_count = 0;
}

static bool ir_cli_is_number(const char *s) {
    if (!s || !*s) return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        ++s;
    }
    return true;
}

static void resolve_ir_path(const char *input, char *output, size_t max_len) {
    if (!input || strlen(input) == 0) {
        snprintf(output, max_len, "/mnt/ghostesp/infrared/remotes");
    } else if (input[0] == '/') {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
    } else {
        snprintf(output, max_len, "/mnt/ghostesp/infrared/remotes/%s", input);
    }
}

static void resolve_ir_universal_path(const char *input, char *output, size_t max_len) {
    const char *base = "/mnt/ghostesp/infrared/universals";
    if (!input || strlen(input) == 0) {
        strncpy(output, base, max_len - 1);
        output[max_len - 1] = '\0';
    } else if (input[0] == '/') {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
    } else {
        snprintf(output, max_len, "%s/%s", base, input);
    }
}

static void ir_universal_send_task(void *arg) {
    IrUniversalSendArgs *args = (IrUniversalSendArgs *)arg;
    bool use_builtin = args->use_builtin;
    uint32_t delay_ms = args->delay_ms ? args->delay_ms : 150;

    char path[256];
    char button[64];
    path[0] = '\0';
    strncpy(button, args->name, sizeof(button) - 1);
    button[sizeof(button) - 1] = '\0';
    if (!use_builtin) {
        strncpy(path, args->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    free(args);

    g_ir_universal_send_cancel = false;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    bool poltergeist_held = false;
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "poltergeist") == 0) {
        infrared_manager_poltergeist_hold_io24_begin();
        poltergeist_held = true;
    }
#endif

    if (use_builtin) {
        size_t total = universal_ir_get_signal_count();
        size_t sent = 0;

        if (total == 0) {
            glog("IR: no built-in universal signals.\n");
        } else {
            glog("IR: universal sendall builtin '%s'\n", button);
            for (size_t i = 0; i < total && !g_ir_universal_send_cancel; ++i) {
                infrared_signal_t sig;
                if (!universal_ir_get_signal(i, &sig)) continue;
                if (strcmp(sig.name, button) != 0) {
                    infrared_manager_free_signal(&sig);
                    continue;
                }
                glog("IR: universal sendall %s [builtin %d]\n", button, (int)i);
                bool ok = infrared_manager_transmit(&sig);
                glog("IR: universal sendall %s -> %s\n", button, ok ? "OK" : "FAIL");
                sent++;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            if (sent == 0) {
                glog("IR: no builtin signals named '%s'\n", button);
            }
        }

    } else {
        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read universal file %s\n", path);
        } else if (count == 0) {
            infrared_manager_free_list(signals, count);
            glog("IR: no signals in %s\n", path);
        } else {
            size_t sent = 0;
            glog("IR: universal sendall '%s' from %s (%zu signals)\n", button, path, count);
            for (size_t i = 0; i < count && !g_ir_universal_send_cancel; ++i) {
                if (strcmp(signals[i].name, button) != 0) continue;
                glog("IR: universal sendall %s [index %d]\n", button, (int)i);
                bool ok = infrared_manager_transmit(&signals[i]);
                glog("IR: universal sendall %s -> %s\n", button, ok ? "OK" : "FAIL");
                sent++;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            if (sent == 0) {
                glog("IR: no signals named '%s' in %s\n", button, path);
            }
            infrared_manager_free_list(signals, count);
        }
    }

    if (g_ir_universal_send_cancel) {
        glog("IR: universal sendall stopped.\n");
    } else {
        glog("IR: universal sendall finished.\n");
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (poltergeist_held) {
        infrared_manager_poltergeist_hold_io24_end();
    }
#endif

    g_ir_universal_send_task = NULL;
    vTaskDelete(NULL);
}

static void ir_rx_learn_task(void *arg) {
    IrRxLearnArgs *args = (IrRxLearnArgs *)arg;
    IrRxLearnMode mode = args->mode;
    int timeout_sec = args->timeout_sec;
    char path[256];
    path[0] = '\0';
    if (mode == IR_BG_MODE_LEARN) {
        strncpy(path, args->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    free(args);

    if (!infrared_manager_rx_init()) {
        if (mode == IR_BG_MODE_RX) {
            glog("IR: failed to init RX (hardware busy?)\n");
        } else {
            glog("IR: failed to init RX\n");
        }
        g_ir_rx_learn_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (mode == IR_BG_MODE_RX) {
        if (timeout_sec <= 0) timeout_sec = 60;
        glog("IR RX mode started. Press Ctrl+C or reset to stop (or wait for timeout).\n");

        bool received_any = false;
        int64_t end = esp_timer_get_time() + (int64_t)timeout_sec * 1000000;

        while (true) {
            int64_t now = esp_timer_get_time();
            if (now >= end) break;

            int remaining_ms = (int)((end - now) / 1000);
            if (remaining_ms <= 0) break;

            infrared_signal_t sig;
            memset(&sig, 0, sizeof(sig));

            if (infrared_manager_rx_receive(&sig, remaining_ms)) {
                if (sig.is_raw) {
                    glog("Received RAW: %zu samples, %lu Hz\n",
                         sig.payload.raw.timings_size,
                         (unsigned long)sig.payload.raw.frequency);
                    infrared_manager_free_signal(&sig);
                } else {
                    glog("Received %s: Addr: 0x%lX Cmd: 0x%lX\n",
                         sig.payload.message.protocol,
                         (unsigned long)sig.payload.message.address,
                         (unsigned long)sig.payload.message.command);
                }
                received_any = true;
                break;
            } else {
                break;
            }
        }

        infrared_manager_rx_deinit();
        if (received_any) {
            glog("IR RX stopped after first signal.\n");
        } else {
            glog("IR RX timed out.\n");
        }
    } else {
        if (timeout_sec <= 0) timeout_sec = 10;
        glog("Waiting for IR signal (10s timeout)...\n");

        infrared_signal_t sig;
        memset(&sig, 0, sizeof(sig));
        if (infrared_manager_rx_receive(&sig, timeout_sec * 1000)) {
            if (sig.is_raw) {
                glog("Captured RAW signal (%zu samples)\n", sig.payload.raw.timings_size);
            } else {
                glog("Captured: %s A:0x%lX C:0x%lX\n", 
                     sig.payload.message.protocol,
                     (unsigned long)sig.payload.message.address,
                     (unsigned long)sig.payload.message.command);
            }

            if (path[0] == '\0') {
                const char *base_dir = "/mnt/ghostesp/infrared/remotes";
                char name_part[96];
                if (!sig.is_raw && sig.payload.message.protocol[0] != '\0') {
                    snprintf(name_part, sizeof(name_part), "Learned_%s_%08lX_%08lX",
                             sig.payload.message.protocol,
                             (unsigned long)sig.payload.message.address,
                             (unsigned long)sig.payload.message.command);
                } else {
                    unsigned long t_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
                    snprintf(name_part, sizeof(name_part), "Learned_RAW_%lu", t_ms);
                }
                snprintf(path, sizeof(path), "%s/%s.ir", base_dir, name_part);
            }

            FILE *f = fopen(path, "a");
            if (f) {
                fprintf(f, "\nname: %s\n", sig.name[0] ? sig.name : "Learned");
                if (sig.is_raw) {
                    fprintf(f, "type: raw\nfrequency: %lu\nduty_cycle: %f\ndata: ", 
                            (unsigned long)sig.payload.raw.frequency, sig.payload.raw.duty_cycle);
                    for(size_t i=0; i<sig.payload.raw.timings_size; i++) {
                        fprintf(f, "%lu%s", (unsigned long)sig.payload.raw.timings[i], (i<sig.payload.raw.timings_size-1)?" ":"\n");
                    }
                } else {
                    fprintf(f, "type: parsed\nprotocol: %s\naddress: %02lX %02lX %02lX %02lX\ncommand: %02lX %02lX %02lX %02lX\n",
                            sig.payload.message.protocol,
                            (unsigned long)((sig.payload.message.address >> 24) & 0xFF),
                            (unsigned long)((sig.payload.message.address >> 16) & 0xFF),
                            (unsigned long)((sig.payload.message.address >> 8) & 0xFF),
                            (unsigned long)(sig.payload.message.address & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 24) & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 16) & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 8) & 0xFF),
                            (unsigned long)(sig.payload.message.command & 0xFF));
                }
                fclose(f);
                glog("Saved to %s\n", path);
            } else {
                glog("Error: Failed to open file %s for writing\n", path);
            }
            infrared_manager_free_signal(&sig);
        } else {
            glog("Timeout, no signal received.\n");
        }
        infrared_manager_rx_deinit();
    }

    g_ir_rx_learn_task = NULL;
    vTaskDelete(NULL);
}

static size_t badusb_join_args(char *out, size_t out_len, int argc, char **argv, int start_idx) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    for (int i = start_idx; i < argc; i++) {
        const char *arg = argv[i] ? argv[i] : "";
        size_t arg_len = strlen(arg);
        if (used + arg_len + 1 >= out_len) break;
        if (used > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, arg, arg_len);
        used += arg_len;
        out[used] = '\0';
    }
    return used;
}

void handle_nrf24_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: nrf24 <start|stop|pause|resume|status|state>\n");
        return;
    }

    const char *sub = argv[1];
    bool remote_request = esp_comm_manager_is_remote_command();

#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
    if (strcmp(sub, "state") == 0) {
        if (argc >= 3) {
            nrf24_analyzer_view_update_remote_state(argv[2]);
        }
        return;
    }
#endif

#ifdef CONFIG_HAS_NRF24
    bool stream_to_peer = remote_request && esp_comm_manager_is_connected();

    if (strcmp(sub, "start") == 0) {
        bool ok = nrf24_remote_manager_start(stream_to_peer);
        if (ok) {
            glog("NRF24 analyzer started\n");
            glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
                 CONFIG_NRF24_SPI_HOST,
                 CONFIG_NRF24_SPI_MOSI_PIN,
                 CONFIG_NRF24_SPI_MISO_PIN,
                 CONFIG_NRF24_SPI_SCK_PIN,
                 CONFIG_NRF24_CSN_PIN,
                 CONFIG_NRF24_CE_PIN);
            if (stream_to_peer) {
                esp_comm_manager_send_command("nrf24", "state started");
            }
        } else {
            glog("NRF24 analyzer failed to start: %s\n", nrf24_remote_manager_get_last_error());
            glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
                 CONFIG_NRF24_SPI_HOST,
                 CONFIG_NRF24_SPI_MOSI_PIN,
                 CONFIG_NRF24_SPI_MISO_PIN,
                 CONFIG_NRF24_SPI_SCK_PIN,
                 CONFIG_NRF24_CSN_PIN,
                 CONFIG_NRF24_CE_PIN);
            if (stream_to_peer) {
                esp_comm_manager_send_command("nrf24", "state error");
            }
        }
        return;
    }

    if (strcmp(sub, "stop") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer already stopped\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("nrf24", "state stopped");
            }
            return;
        }
        nrf24_remote_manager_stop();
        glog("NRF24 analyzer stopping\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("nrf24", "state stopped");
        }
        return;
    }

    if (strcmp(sub, "pause") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer is not running\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("nrf24", "state error");
            }
            return;
        }
        nrf24_remote_manager_set_paused(true);
        glog("NRF24 analyzer paused\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("nrf24", "state paused");
        }
        return;
    }

    if (strcmp(sub, "resume") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer is not running\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("nrf24", "state error");
            }
            return;
        }
        nrf24_remote_manager_set_paused(false);
        glog("NRF24 analyzer resumed\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("nrf24", "state resumed");
        }
        return;
    }

    if (strcmp(sub, "status") == 0) {
        glog("NRF24 running: %s\n", nrf24_remote_manager_is_running() ? "yes" : "no");
        glog("NRF24 paused: %s\n", nrf24_remote_manager_is_paused() ? "yes" : "no");
        glog("NRF24 last error: %s\n", nrf24_remote_manager_get_last_error());
        glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
             CONFIG_NRF24_SPI_HOST,
             CONFIG_NRF24_SPI_MOSI_PIN,
             CONFIG_NRF24_SPI_MISO_PIN,
             CONFIG_NRF24_SPI_SCK_PIN,
             CONFIG_NRF24_CSN_PIN,
             CONFIG_NRF24_CE_PIN);
        return;
    }

    glog("Unknown nrf24 subcommand: %s\n", sub);
#else
#ifdef CONFIG_HAS_NRF24_REMOTE
    glog("NRF24 local scanner not enabled on this build (remote/display role only)\n");
#else
    glog("NRF24 not enabled on this build\n");
#endif
    if (remote_request && esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("nrf24", "state error");
    }
#endif
}

static void badusb_strip_quotes(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    if (len < 2) return;
    if ((text[0] == '"' && text[len - 1] == '"') || (text[0] == '\'' && text[len - 1] == '\'')) {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

void handle_badusb_cmd(int argc, char **argv) {
#ifdef CONFIG_HAS_BADUSB
    if (argc < 2) {
        glog("Usage: badusb <run|list|stop|exec|set_vid|set_pid|set_mfr|set_prod|set_rand|set_layout>\n");
        glog("  badusb run <filename>  - Execute a DuckyScript from /mnt/ghostesp/badusb/\n");
        glog("  badusb list            - List available scripts\n");
        glog("  badusb stop            - Stop current execution\n");
        glog("  badusb exec <size>     - Prepare to receive a script via stream\n");
        glog("  badusb set_vid <hex>   - Set USB VID for next run\n");
        glog("  badusb set_pid <hex>   - Set USB PID for next run\n");
        glog("  badusb set_mfr <text>  - Set USB manufacturer for next run\n");
        glog("  badusb set_prod <text> - Set USB product for next run\n");
        glog("  badusb set_rand <0|1>  - Toggle USB detail randomization\n");
        glog("  badusb set_layout <n>  - Set keyboard layout for next run\n");
        return;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "list") == 0) {
        char scripts[32][64];
        int count = badusb_manager_list_scripts(scripts, 32);
        if (count == 0) {
            glog("No scripts found in /mnt/ghostesp/badusb/\n");
        } else {
            glog("BadUSB scripts (%d):\n", count);
            for (int i = 0; i < count; i++) {
                glog("  [%d] %s\n", i, scripts[i]);
            }
        }
    } else if (strcmp(sub, "run") == 0) {
        if (argc < 3) {
            glog("Usage: badusb run <filename|builtin>\n");
            return;
        }
        if (strcmp(argv[2], "builtin") == 0) {
            char *buf = strdup(badusb_builtin_script);
            if (buf) {
                esp_err_t ret = badusb_manager_execute_buffer(buf, BADUSB_BUILTIN_SCRIPT_LEN);
                if (ret != ESP_OK) {
                    glog("BadUSB: Failed to execute built-in script\n");
                }
            } else {
                glog("BadUSB: Out of memory\n");
            }
        } else {
            char path[256];
            snprintf(path, sizeof(path), "/mnt/ghostesp/badusb/%s", argv[2]);
            esp_err_t ret = badusb_manager_execute_file(path);
            if (ret != ESP_OK) {
                glog("BadUSB: Failed to execute %s\n", argv[2]);
            }
        }
    } else if (strcmp(sub, "exec") == 0) {
        if (argc < 3) {
            glog("Usage: badusb exec <size>\n");
            return;
        }
        size_t size = (size_t)atoi(argv[2]);
        esp_err_t ret = badusb_manager_prepare_receive(size);
        if (ret != ESP_OK) {
            glog("BadUSB: Failed to prepare receive\n");
        }
    } else if (strcmp(sub, "stop") == 0) {
        badusb_manager_stop();
        glog("BadUSB: Stopped\n");
    } else if (strcmp(sub, "set_vid") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_vid <hex>\n");
            return;
        }
        uint16_t vid = (uint16_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_vid(&G_Settings, vid);
        glog("BadUSB: VID set\n");
    } else if (strcmp(sub, "set_pid") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_pid <hex>\n");
            return;
        }
        uint16_t pid = (uint16_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_pid(&G_Settings, pid);
        glog("BadUSB: PID set\n");
    } else if (strcmp(sub, "set_mfr") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_mfr <text>\n");
            return;
        }
        char value[64];
        badusb_join_args(value, sizeof(value), argc, argv, 2);
        badusb_strip_quotes(value);
        settings_set_badusb_manufacturer(&G_Settings, value);
        glog("BadUSB: Manufacturer set\n");
    } else if (strcmp(sub, "set_prod") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_prod <text>\n");
            return;
        }
        char value[64];
        badusb_join_args(value, sizeof(value), argc, argv, 2);
        badusb_strip_quotes(value);
        settings_set_badusb_product(&G_Settings, value);
        glog("BadUSB: Product set\n");
    } else if (strcmp(sub, "set_rand") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_rand <0|1>\n");
            return;
        }
        bool enabled = atoi(argv[2]) != 0;
        settings_set_badusb_randomize(&G_Settings, enabled);
        glog("BadUSB: Randomize set to %u\n", enabled ? 1 : 0);
    } else if (strcmp(sub, "set_layout") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_layout <n>\n");
            return;
        }
        uint8_t layout = (uint8_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_kb_layout(&G_Settings, layout);
        glog("BadUSB: Layout set to %u\n", layout);
    } else if (strcmp(sub, "status") == 0) {
        // Status update from peer - forward to view
#ifdef CONFIG_WITH_SCREEN
        if (argc >= 3) {
            extern void badusb_view_update_status(const char *status);
            badusb_view_update_status(argv[2]);
        }
#endif
    } else {
        glog("Unknown badusb subcommand: %s\n", sub);
    }
#elif defined(CONFIG_HAS_BADUSB_REMOTE)
    if (argc < 2) {
        glog("BadUSB remote: no subcommand\n");
        return;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "status") == 0) {
        // Status update from S3 peer - forward to display view
#ifdef CONFIG_WITH_SCREEN
        if (argc >= 3) {
            extern void badusb_view_update_status(const char *status);
            badusb_view_update_status(argv[2]);
        }
#endif
    } else {
        glog("Unknown badusb subcommand: %s\n", sub);
    }
#else
    glog("BadUSB not enabled on this build\n");
#endif
}

void handle_ir_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: ir <send|inline|list|show|universals|rx|dazzler>\n");
        glog("  ir send <path|remote_index> [button_index]\n");
        glog("  ir inline\n");
        glog("  ir list [path]\n");
        glog("  ir show <path|remote_index>\n");
        glog("  ir universals <list|send|sendall> ...\n");
        glog("  ir rx\n");
        glog("  ir dazzler [stop]\n");
        return;
    }

    const char *sub = argv[1];
    char path[256];

    if (strcmp(sub, "send") == 0) {
        if (argc < 3) {
            glog("Usage: ir send <path|remote_index> [button_index]\n");
            return;
        }
        const char *arg = argv[2];
        int button_index = 0;
        if (argc >= 4) {
            button_index = atoi(argv[3]);
        }

        if (ir_cli_is_number(arg) && g_ir_cli_remote_count > 0) {
            int remote_index = atoi(arg);
            if (remote_index < 0 || (size_t)remote_index >= g_ir_cli_remote_count) {
                glog("IR: remote index out of range (0-%d). Run 'ir list' to see indices.\n",
                     (int)(g_ir_cli_remote_count ? g_ir_cli_remote_count - 1 : 0));
                return;
            }
            strncpy(path, g_ir_cli_remote_paths[remote_index], sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            resolve_ir_path(arg, path, sizeof(path));
        }

        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read list from %s\n", path);
            return;
        }

        if (count == 0) {
            infrared_manager_free_list(signals, count);
            glog("IR: no signals in %s\n", path);
            return;
        }

        if (button_index < 0 || (size_t)button_index >= count) {
            infrared_manager_free_list(signals, count);
            glog("IR: index out of range (0-%d)\n", (int)(count - 1));
            return;
        }

        infrared_signal_t *sig = &signals[button_index];
        bool ok = infrared_manager_transmit(sig);
        glog("IR: send %s\n", ok ? "OK" : "FAIL");
        if (sig->is_raw) {
            if (sig->payload.raw.timings && sig->payload.raw.timings_size > 0) {
                glog("IR: signal raw len=%u freq=%luHz duty=%.2f\n",
                     (unsigned)sig->payload.raw.timings_size,
                     (unsigned long)sig->payload.raw.frequency,
                     (double)sig->payload.raw.duty_cycle);
            }
        } else {
            const char *proto = sig->payload.message.protocol;
            if (proto && proto[0] != '\0') {
                uint32_t addr = sig->payload.message.address;
                uint32_t cmd = sig->payload.message.command;
                if (sig->name[0] != '\0') {
                    glog("IR: signal [%s] protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                         sig->name, proto, (unsigned long)addr, (unsigned long)cmd);
                } else {
                    glog("IR: signal protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                         proto, (unsigned long)addr, (unsigned long)cmd);
                }
            }
        }
        infrared_manager_free_list(signals, count);
        return;
    }

    if (strcmp(sub, "inline") == 0) {
        glog("IR inline mode:\n");
        glog("  Send IR content between [IR/BEGIN] and [IR/CLOSE] markers.\n");
        glog("  Content may be a JSON object or .ir-style text block.\n");
        return;
    }

    if (strcmp(sub, "list") == 0) {
        resolve_ir_path((argc >= 3) ? argv[2] : NULL, path, sizeof(path));
        DIR *d = opendir(path);
        if (!d) {
            glog("IR: failed to open directory %s\n", path);
            ir_cli_clear_remote_index();
            return;
        }
        ir_cli_clear_remote_index();
        struct dirent *dir;
        glog("IR files in %s:\n", path);
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG) {
                 if (strstr(dir->d_name, ".ir") || strstr(dir->d_name, ".json")) {
                     int idx = (int)g_ir_cli_remote_count;
                     if (g_ir_cli_remote_count < IR_CLI_MAX_REMOTES) {
                         char full[512];
                         snprintf(full, sizeof(full), "%s/%s", path, dir->d_name);
                         g_ir_cli_remote_paths[g_ir_cli_remote_count] = strdup(full);
                         if (g_ir_cli_remote_paths[g_ir_cli_remote_count]) {
                             g_ir_cli_remote_count++;
                         }
                     }
                     glog("  [%d] %s\n", idx, dir->d_name);
                 }
            }
        }
        closedir(d);
        if (g_ir_cli_remote_count == 0) {
            glog("  (none)\n");
        }
        return;
    }

    if (strcmp(sub, "show") == 0) {
        if (argc < 3) {
            glog("Usage: ir show <path|remote_index>\n");
            return;
        }
        const char *arg = argv[2];
        if (ir_cli_is_number(arg) && g_ir_cli_remote_count > 0) {
            int remote_index = atoi(arg);
            if (remote_index < 0 || (size_t)remote_index >= g_ir_cli_remote_count) {
                glog("IR: remote index out of range (0-%d). Run 'ir list' first.\n",
                     (int)(g_ir_cli_remote_count ? g_ir_cli_remote_count - 1 : 0));
                return;
            }
            strncpy(path, g_ir_cli_remote_paths[remote_index], sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            resolve_ir_path(arg, path, sizeof(path));
        }
        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read/parse %s\n", path);
            return;
        }
        bool is_universal_file = (strstr(path, "/infrared/universals") != NULL);
        if (is_universal_file) {
            size_t unique = 0;
            for (size_t i = 0; i < count; i++) {
                const char *name = signals[i].name;
                bool seen = false;
                for (size_t j = 0; j < i; j++) {
                    if (strcmp(signals[j].name, name) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) unique++;
            }

            glog("Unique buttons in %s (%zu):\n", path, unique);
            size_t idx = 0;
            for (size_t i = 0; i < count; i++) {
                const char *name = signals[i].name;
                bool seen = false;
                for (size_t j = 0; j < i; j++) {
                    if (strcmp(signals[j].name, name) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (seen) continue;
                glog("  [%d] %s\n", (int)idx, name);
                idx++;
            }
        } else {
            glog("Signals in %s (%zu):\n", path, count);
            for (size_t i = 0; i < count; i++) {
                 const char *proto = signals[i].is_raw ? "RAW" : signals[i].payload.message.protocol;
                 glog("  [%d] %s (%s)", (int)i, signals[i].name, proto);
                 if (!signals[i].is_raw) {
                     glog(" Addr: 0x%lX Cmd: 0x%lX", 
                          (unsigned long)signals[i].payload.message.address, 
                          (unsigned long)signals[i].payload.message.command);
                 }
                 glog("\n");
            }
        }
        infrared_manager_free_list(signals, count);
        return;
    }

    if (strcmp(sub, "universals") == 0) {
        if (argc < 3) {
            glog("Usage: ir universals <list|send|sendall>\n");
            return;
        }
        const char *u_sub = argv[2];

        if (strcmp(u_sub, "list") == 0) {
            bool show_all = (argc >= 4 && strcmp(argv[3], "-all") == 0);

            const char *uni_path = "/mnt/ghostesp/infrared/universals";
            DIR *d = opendir(uni_path);
            if (d) {
                glog("Universal Files in %s:\n", uni_path);
                struct dirent *dir;
                int file_count = 0;
                while ((dir = readdir(d)) != NULL) {
                    if (dir->d_type == DT_REG) {
                         if (strstr(dir->d_name, ".ir") || strstr(dir->d_name, ".json")) {
                             glog("  %s\n", dir->d_name);
                             file_count++;
                         }
                    }
                }
                closedir(d);
                if (file_count == 0) glog("  (none)\n");
            }

            size_t count = universal_ir_get_signal_count();
            if (show_all) {
                glog("\nBuilt-in Universal Signals (%zu):\n", count);
                for (size_t i = 0; i < count; i++) {
                    infrared_signal_t sig;
                    if (universal_ir_get_signal(i, &sig)) {
                         glog("  [%d] %s (%s) Addr: 0x%lX Cmd: 0x%lX\n", (int)i, 
                              sig.name, sig.payload.message.protocol,
                              (unsigned long)sig.payload.message.address,
                              (unsigned long)sig.payload.message.command);
                    }
                }
            } else {
                glog("\nBuilt-in Universal Signals: %zu available.\n", count);
                glog("Use 'ir universals list -all' to list them.\n");
            }
            return;
        }
        if (strcmp(u_sub, "send") == 0) {
            if (argc < 4) {
                glog("Usage: ir universals send <index>\n");
                return;
            }
            int idx = atoi(argv[3]);
            infrared_signal_t sig;
            if (universal_ir_get_signal(idx, &sig)) {
                bool ok = infrared_manager_transmit(&sig);
                glog("IR: universal send %s\n", ok ? "OK" : "FAIL");
            } else {
                glog("IR: invalid universal index\n");
            }
            return;
        }
        if (strcmp(u_sub, "sendall") == 0) {
            if (g_ir_universal_send_task != NULL) {
                glog("IR: universal sendall already running; use 'stop' to cancel.\n");
                return;
            }
            if (argc < 4) {
                glog("Usage: ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]\n");
                return;
            }
            const char *arg = argv[3];
            const char *button_name = NULL;
            uint32_t delay_ms = 150;

            if (strcmp(arg, "TURNHISTVOFF") == 0) {
                if (argc >= 5) {
                    button_name = argv[4];
                    if (argc >= 6) {
                        int d = atoi(argv[5]);
                        if (d > 0) delay_ms = (uint32_t)d;
                    }
                } else {
                    button_name = "Power Off";
                    if (argc >= 5) {
                        int d = atoi(argv[4]);
                        if (d > 0) delay_ms = (uint32_t)d;
                    }
                }
            } else {
                if (argc < 5) {
                    glog("Usage: ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]\n");
                    return;
                }
                button_name = argv[4];
                if (argc >= 6) {
                    int d = atoi(argv[5]);
                    if (d > 0) delay_ms = (uint32_t)d;
                }
            }

            IrUniversalSendArgs *args = (IrUniversalSendArgs *)malloc(sizeof(IrUniversalSendArgs));
            if (!args) {
                glog("IR: failed to allocate sendall args.\n");
                return;
            }
            memset(args, 0, sizeof(*args));
            args->delay_ms = delay_ms;
            strncpy(args->name, button_name, sizeof(args->name) - 1);
            args->name[sizeof(args->name) - 1] = '\0';
            if (strcmp(arg, "TURNHISTVOFF") == 0) {
                args->use_builtin = true;
            } else {
                args->use_builtin = false;
                resolve_ir_universal_path(arg, args->path, sizeof(args->path));
            }
            g_ir_universal_send_cancel = false;
            if (xTaskCreate(ir_universal_send_task, "ir_uni_sendall", 4096, args, 5, &g_ir_universal_send_task) != pdPASS) {
                glog("IR: failed to start universal sendall task.\n");
                free(args);
                g_ir_universal_send_task = NULL;
                return;
            }
            glog("IR: universal sendall started for '%s'; use 'stop' to cancel.\n", button_name);
            return;
        }
    }

    if (strcmp(sub, "rx") == 0) {
        if (g_ir_rx_learn_task != NULL) {
            glog("IR RX/learn already running; use 'stop' to cancel.\n");
            return;
        }
        int timeout_sec = 60;
        if (argc >= 3) {
            int t = atoi(argv[2]);
            if (t > 0) timeout_sec = t;
        }
        IrRxLearnArgs *args = (IrRxLearnArgs *)malloc(sizeof(IrRxLearnArgs));
        if (!args) {
            glog("IR: failed to allocate RX task args.\n");
            return;
        }
        memset(args, 0, sizeof(*args));
        args->mode = IR_BG_MODE_RX;
        args->timeout_sec = timeout_sec;
        args->path[0] = '\0';
        if (xTaskCreate(ir_rx_learn_task, "ir_rx", 4096, args, 5, &g_ir_rx_learn_task) != pdPASS) {
            glog("IR: failed to start RX task.\n");
            free(args);
            g_ir_rx_learn_task = NULL;
            return;
        }
        glog("IR RX task started; use 'stop' to cancel.\n");
        return;
    }

    if (strcmp(sub, "learn") == 0) {
        if (g_ir_rx_learn_task != NULL) {
            glog("IR RX/learn already running; use 'stop' to cancel.\n");
            return;
        }
        if (argc >= 3) {
            resolve_ir_path(argv[2], path, sizeof(path));
        } else {
            path[0] = '\0';
        }
        IrRxLearnArgs *args = (IrRxLearnArgs *)malloc(sizeof(IrRxLearnArgs));
        if (!args) {
            glog("IR: failed to allocate learn task args.\n");
            return;
        }
        memset(args, 0, sizeof(*args));
        args->mode = IR_BG_MODE_LEARN;
        args->timeout_sec = 10;
        strncpy(args->path, path, sizeof(args->path) - 1);
        args->path[sizeof(args->path) - 1] = '\0';
        if (xTaskCreate(ir_rx_learn_task, "ir_learn", 5120, args, 5, &g_ir_rx_learn_task) != pdPASS) {
            glog("IR: failed to start learn task.\n");
            free(args);
            g_ir_rx_learn_task = NULL;
            return;
        }
        glog("IR learn task started; use 'stop' to cancel.\n");
        return;
    }

    if (strcmp(sub, "dazzler") == 0) {
        if (argc >= 3 && strcmp(argv[2], "stop") == 0) {
            if (infrared_manager_dazzler_is_active()) {
                infrared_manager_dazzler_stop();
                glog("IR_DAZZLER:STOPPING\n");
            } else {
                glog("IR_DAZZLER:NOT_RUNNING\n");
            }
            return;
        }
        if (infrared_manager_dazzler_is_active()) {
            glog("IR_DAZZLER:ALREADY_RUNNING\n");
            return;
        }
        if (infrared_manager_dazzler_start()) {
            glog("IR_DAZZLER:STARTED\n");
        } else {
            glog("IR_DAZZLER:FAILED\n");
        }
        return;
    }

    glog("Unknown ir subcommand: %s\n", sub);
}

void handle_mirror_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: mirror <on|off|refresh|status>\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        screen_mirror_set_enabled(true);
        glog("Screen mirror enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        screen_mirror_set_enabled(false);
        glog("Screen mirror disabled\n");
    } else if (strcmp(argv[1], "refresh") == 0) {
        screen_mirror_refresh();
    } else if (strcmp(argv[1], "status") == 0) {
        glog("Screen mirror: %s\n", screen_mirror_is_enabled() ? "on" : "off");
    } else {
        glog("Usage: mirror <on|off|refresh|status>\n");
    }
}

void handle_raveport_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    glog("RAVE_SERIAL A55AC33C 79\n");
}

void handle_rave_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: rave <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        display_manager_switch_view(&music_visualizer_view);
        glog("Visualizer opened\n");
    } else if (strcmp(argv[1], "off") == 0) {
        display_manager_switch_view(&apps_menu_view);
        glog("Visualizer closed\n");
    } else {
        glog("Usage: rave <on|off>\n");
    }
}

void handle_identify_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    glog("GHOSTESP_OK\n");
}

void handle_input_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: input <left|right|up|down|select>\n");
        return;
    }
    int joystick_index = -1;
    if (strcmp(argv[1], "left") == 0) joystick_index = 0;
    else if (strcmp(argv[1], "select") == 0 || strcmp(argv[1], "ok") == 0) joystick_index = 1;
    else if (strcmp(argv[1], "up") == 0) joystick_index = 2;
    else if (strcmp(argv[1], "right") == 0) joystick_index = 3;
    else if (strcmp(argv[1], "down") == 0) joystick_index = 4;
    
    if (joystick_index < 0) {
        glog("Unknown input: %s\n", argv[1]);
        return;
    }
    
    if (input_queue) {
        InputEvent evt = {
            .type = INPUT_TYPE_JOYSTICK,
            .data.joystick_index = joystick_index,
            .data.joystick_pressed = true
        };
        xQueueSend(input_queue, &evt, 0);
    }
}

void handle_iobtn_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: iobtn <1|2|3> [command]\n");
        glog("  Button 1 = P10, 2 = P11, 3 = P12. Show or set command run on press.\n");
        glog("  Omit [command] to show current; use \"\" to clear (then button sends joystick event).\n");
        return;
    }
    int btn = atoi(argv[1]);
    if (btn < 1 || btn > 3) {
        glog("Button must be 1, 2, or 3 (P10, P11, P12)\n");
        return;
    }
    const char *cur = NULL;
    if (btn == 1) cur = settings_get_io_btn_p10_cmd(&G_Settings);
    else if (btn == 2) cur = settings_get_io_btn_p11_cmd(&G_Settings);
    else cur = settings_get_io_btn_p12_cmd(&G_Settings);
    if (argc == 2) {
        glog("P1%u: \"%s\"\n", (unsigned)(btn + 9), cur && cur[0] ? cur : "(none)");
        return;
    }
    if (btn == 1) settings_set_io_btn_p10_cmd(&G_Settings, argv[2]);
    else if (btn == 2) settings_set_io_btn_p11_cmd(&G_Settings, argv[2]);
    else settings_set_io_btn_p12_cmd(&G_Settings, argv[2]);
    settings_save(&G_Settings);
    glog("P1%u command set to \"%s\"\n", (unsigned)(btn + 9), argv[2][0] ? argv[2] : "(cleared)");
}

void handle_usb_kbd_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: usbkbd <on|off|status>\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        usb_keyboard_manager_set_host_mode(true);
        glog("USB keyboard host mode enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        usb_keyboard_manager_set_host_mode(false);
        glog("USB keyboard host mode disabled\n");
    } else if (strcmp(argv[1], "status") == 0) {
        glog("USB keyboard host mode: %s\n", usb_keyboard_manager_is_host_mode() ? "on" : "off");
    } else {
        glog("Usage: usbkbd <on|off|status>\n");
    }
}

void handle_aerial_scan_cmd(int argc, char **argv) {
    uint32_t duration = 30000;  // default 30 seconds

    if (argc > 1) {
        duration = atoi(argv[1]) * 1000;
        if (duration < 1000) duration = 1000;
        if (duration > 300000) duration = 300000;
    }

    aerial_detector_init();
    esp_err_t ret = aerial_detector_start_scan(duration);

    if (ret == ESP_OK) {
        glog("Scan Started (%lu sec)\n", duration / 1000);
        glog("Phase 1: WiFi | Phase 2: BLE\n");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        glog("Scan already running\n");
    } else {
        glog("Failed to start scan\n");
    }
}

void handle_aerial_list_cmd(int argc, char **argv) {
    aerial_detector_compact_known_devices();
    
    int total = aerial_detector_get_device_count();
    int shown = 0;
    
    for (int i = 0; i < total; i++) {
        AerialDevice *dev = aerial_detector_get_device(i);
        if (!dev || dev->type == AERIAL_TYPE_UNKNOWN) continue;
        
        if (shown == 0) {
            glog("Detected aerial device(s):\n\n");
        }
        shown++;
        
        glog("[%d] %s\n", i, dev->device_id);
        glog("    MAC: %s\n", dev->mac);
        glog("    Type: %s\n", aerial_detector_get_type_string(dev->type));
        glog("    RSSI: %d dBm\n", dev->rssi);
        
        if (dev->vendor[0] != '\0') {
            glog("    Vendor: %s\n", dev->vendor);
        }
        
        if (dev->has_location) {
            glog("    Location: %.6f, %.6f\n", dev->latitude, dev->longitude);
            if (dev->altitude > -1000.0f) {
                glog("    Altitude: %.1f m\n", dev->altitude);
            }
            if (dev->speed_horizontal < 255.0f) {
                glog("    Speed: %.1f m/s @ %.0f°\n", dev->speed_horizontal, dev->direction);
            }
            glog("    Status: %s\n", aerial_detector_get_status_string(dev->status));
        }
        
        if (dev->has_operator_location) {
            glog("    Operator: %.6f, %.6f", dev->operator_latitude, dev->operator_longitude);
            if (dev->operator_altitude > -1000.0f) {
                glog(" @ %.1f m", dev->operator_altitude);
            }
            glog("\n");
        }
        
        if (strcmp(dev->operator_id, "N/A") != 0) {
            glog("    Operator ID: %s\n", dev->operator_id);
        }
        
        if (strcmp(dev->description, "N/A") != 0 && dev->description[0] != '\0') {
            glog("    Description: %s\n", dev->description);
        }
        
        uint32_t age_sec = (esp_timer_get_time() / 1000 - dev->last_seen_ms) / 1000;
        glog("    Last seen: %lu sec ago\n", age_sec);
        glog("\n");
    }

    if (shown == 0) {
        glog("No aerial devices detected\n");
    }
}

void handle_aerial_track_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: aerialtrack <device_index|mac_address>\n");
        glog("Use 'aeriallist' to see available devices\n");
        return;
    }
    
    AerialDevice *dev = NULL;
    
    // check if argument is a number (device index)
    if (argv[1][0] >= '0' && argv[1][0] <= '9') {
        int index = atoi(argv[1]);
        dev = aerial_detector_get_device(index);
        if (!dev) {
            glog("Invalid device index. Use 'aeriallist' to see available devices\n");
            return;
        }
    } else {
        // assume mac address
        dev = aerial_detector_find_device_by_mac(argv[1]);
        if (!dev) {
            glog("Device not found: %s\n", argv[1]);
            return;
        }
    }
    
    // ensure scanning is running to keep updates flowing
    if (!aerial_detector_is_scanning()) {
        aerial_detector_start_scan(30000); // default 30s; tracking will refresh each phase
        glog("Started aerial scan for tracking\n");
    }
    
    esp_err_t ret = aerial_detector_track_device(dev->mac);
    if (ret == ESP_OK) {
        glog("Now tracking: %s (%s)\n", dev->device_id, dev->mac);
        glog("RSSI: %d dBm\n", dev->rssi);
        
        if (dev->has_location) {
            glog("Location: %.6f, %.6f @ %.1f m\n", 
                 dev->latitude, dev->longitude, dev->altitude);
        }
    } else {
        glog("Failed to track device\n");
    }
}

void handle_aerial_stop_cmd(int argc, char **argv) {
    if (aerial_detector_is_scanning()) {
        aerial_detector_stop_scan();
        glog("Scan Stopped\n");
    } else {
        glog("No scan running\n");
    }

    aerial_detector_untrack_device();
}

void handle_aerial_spoof_cmd(int argc, char **argv) {
    const char *device_id;
    double lat;
    double lon;
    float alt;
    
    if (argc < 2) {
        // default test mode - no args needed
        device_id = "GHOST-TEST";
        lat = 37.7749;   // san francisco
        lon = -122.4194;
        alt = 100.0f;
        glog("Using default test drone:\n");
        glog("Device ID: %s\n", device_id);
        glog("Location: %.6f, %.6f @ %.1fm\n\n", lat, lon, alt);
    } else if (argc < 5) {
        glog("Usage: aerialspoof [device_id latitude longitude altitude]\n");
        glog("Examples:\n");
        glog("  aerialspoof                              # Use defaults\n");
        glog("  aerialspoof DRONE-1234 40.7128 -74.0060 100\n");
        glog("\nBroadcasts fake drone RemoteID for testing purposes.\n");
        glog("Complies with ASTM F3411 OpenDroneID standard.\n");
        return;
    } else {
        device_id = argv[1];
        lat = atof(argv[2]);
        lon = atof(argv[3]);
        alt = atof(argv[4]);
    }
    
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        glog("Invalid coordinates. Lat: -90 to 90, Lon: -180 to 180\n");
        return;
    }
    
    // stop existing spoof if running
    if (aerial_detector_is_emulating()) {
        aerial_detector_stop_emulation();
    }
    
    aerial_detector_init();
    esp_err_t ret = aerial_detector_start_emulation(device_id, lat, lon, alt);
    
    if (ret == ESP_OK) {
        glog("Spoofing Started\n");
        glog("ID: %s | Pos: %.6f, %.6f @ %.1fm\n", device_id, lat, lon, alt);
    } else {
        glog("Failed to start spoofing\n");
    }
}

void handle_aerial_spoof_stop_cmd(int argc, char **argv) {
    if (aerial_detector_is_emulating()) {
        aerial_detector_stop_emulation();
        glog("Spoofing Stopped\n");
    } else {
        glog("No spoofing active\n");
    }
}

void handle_loadconfig_cmd(int argc, char **argv) {
    glog("Loading configuration from SD card...\n");
    
    esp_err_t config_err = config_manager_load_from_sd();
    
    if (config_err == ESP_OK) {
        glog("\n=== Configuration Loaded Successfully ===\n");
        
        // Show what was loaded
        const char *ssid = settings_get_sta_ssid(&G_Settings);
        if (ssid && ssid[0]) {
            glog("WiFi SSID: %s\n", ssid);
            glog("WiFi Password: (set)\n");
        }
        
        if (G_Settings.wigle_api_key[0]) {
            glog("Wigle Token: (set)\n");
        }
        
        glog("Wigle Auto Upload: %s\n", settings_get_wigle_auto_upload(&G_Settings) ? "on" : "off");
        glog("Wigle Donate: %s\n", settings_get_wigle_donate(&G_Settings) ? "on" : "off");
        
        glog("\nSettings saved to NVS.\n");
        glog("Reconfiguring WiFi...\n");
        
        wifi_manager_configure_sta_from_settings();
        
        glog("Configuration applied successfully!\n");
    } else if (config_err == ESP_ERR_NOT_FOUND) {
        glog("Error: config.cfg not found on SD card\n");
        glog("Expected location: /mnt/ghostesp/config.cfg\n");
    } else {
        glog("Error: Failed to load config.cfg: %s\n", esp_err_to_name(config_err));
    }
}

void handle_wigle_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("wigle API <encoded|name:token>  - Set Wigle API key from wigle.net/account\n");
        glog("wigle auto on/off          - Auto-upload at boot\n");
        glog("wigle donate on/off        - Donate data to Wigle\n");
        glog("wigle show                 - Show current settings\n");
        glog("wigle list                 - List stored uploaded CSV memory\n");
        glog("wigle files [page]         - List GPS CSV files for manual upload\n");
        glog("wigle upload <filename>    - Upload one CSV from /mnt/ghostesp/gps\n");
        glog("wigle upload all           - Upload all pending CSVs\n");
        glog("wigle stats                - Show WiGLE account stats\n");
        return;
    }
    if (strcmp(argv[1], "API") == 0 || strcmp(argv[1], "api") == 0) {
        if (argc < 3) {
            glog("Usage: wigle API <EncodedForUseToken|APIName:APIToken>\n");
            glog("Get credentials from https://wigle.net/account\n");
            return;
        }
        wigle_set_api_key(argv[2]);
        glog("Wigle API key set\n");
        return;
    }
    if (strcmp(argv[1], "auto") == 0) {
        if (argc < 3) {
            glog("Usage: wigle auto on/off\n");
            return;
        }
        bool enabled = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
        settings_set_wigle_auto_upload(&G_Settings, enabled);
        settings_persist_setting(SETTING_WIGLE_AUTO_UPLOAD);
        glog("Wigle auto-upload: %s\n", enabled ? "on" : "off");
        return;
    }
    if (strcmp(argv[1], "donate") == 0) {
        if (argc < 3) {
            glog("Usage: wigle donate on/off\n");
            return;
        }
        bool enabled = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
        settings_set_wigle_donate(&G_Settings, enabled);
        settings_persist_setting(SETTING_WIGLE_DONATE);
        glog("Wigle donate: %s\n", enabled ? "on" : "off");
        return;
    }
    if (strcmp(argv[1], "show") == 0) {
        glog("API Key: %s\n", G_Settings.wigle_api_key[0] ? "(set)" : "(not set)");
        glog("Auto Upload: %s\n", settings_get_wigle_auto_upload(&G_Settings) ? "on" : "off");
        glog("Donate: %s\n", settings_get_wigle_donate(&G_Settings) ? "on" : "off");
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        wigle_uploaded_list();
        return;
    }
    if (strcmp(argv[1], "files") == 0) {
        int page = 1;
        if (argc >= 3) {
            page = atoi(argv[2]);
            if (page < 1) page = 1;
        }

        const int page_size = 8;
        int offset = (page - 1) * page_size;
        char names[8][MAX_PORTAL_NAME];
        bool has_more = false;
        int count = wigle_list_csv_files_paged(offset, page_size, names, &has_more);
        if (count < 0) {
            glog("Failed to list CSV files in /mnt/ghostesp/gps\n");
            return;
        }
        if (count == 0) {
            glog("No CSV files found in /mnt/ghostesp/gps\n");
            return;
        }

        glog("WiGLE CSV files (page %d):\n", page);
        for (int i = 0; i < count; i++) {
            int wifi_rows = 0;
            int total_rows = 0;
            esp_err_t info_err = wigle_get_csv_info(names[i], &wifi_rows, &total_rows);
            if (info_err == ESP_OK) {
                glog("  %s  [wifi=%d total=%d]\n", names[i], wifi_rows, total_rows);
            } else {
                glog("  %s\n", names[i]);
            }
        }
        if (page > 1) {
            glog("Previous: wigle files %d\n", page - 1);
        }
        if (has_more) {
            glog("Next: wigle files %d\n", page + 1);
        }
        return;
    }
    if (strcmp(argv[1], "upload") == 0) {
        if (argc < 3 || strcmp(argv[2], "all") == 0) {
            wigle_upload_all_async();
            glog("Wigle upload started\n");
            return;
        }

        char message[256];
        esp_err_t err = wigle_upload_single_csv(argv[2], message, sizeof(message));
        if (message[0]) {
            glog("%s\n", message);
        } else {
            glog("%s\n", err == ESP_OK ? "Upload completed" : "Upload failed");
        }
        return;
    }
    if (strcmp(argv[1], "stats") == 0) {
        char message[512];
        esp_err_t err = wigle_get_stats(message, sizeof(message));
        if (message[0]) {
            glog("%s\n", message);
        } else {
            glog("%s\n", err == ESP_OK ? "Stats loaded" : "Failed to load stats");
        }
        return;
    }
    glog("Unknown wigle command: %s\n", argv[1]);
}

#ifdef CONFIG_HAS_MIC
static void handle_mic_cal_cmd(int argc, char **argv) {
    extern void goertzel_restart_cal(void);
    extern void mic_restart_noise_cal(void);
    goertzel_restart_cal();
    mic_restart_noise_cal();
    glog("MIC calibration restarted\n");
}
#endif

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
    register_command("wifistatus", handle_wifi_status);
    register_command("stopportal", stop_portal);
    register_command("connect", handle_wifi_connection);
    register_command("dialconnect", handle_dial_command);
    register_command("powerprinter", handle_printer_command);
    register_command("tplinktest", handle_tp_link_test);
    register_command("stop", handle_stop_flipper);
    register_command("reboot", handle_reboot);
    register_command("startwd", handle_startwd);
    register_command("gpsinfo", handle_gps_info);
    register_command("gpspin", handle_gps_pin);
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
    register_command("chameleon", handle_chameleon_cmd);
#endif
#ifdef DEBUG
    register_command("crash", handle_crash);
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    register_command("coredump", handle_coredump_cmd);
#endif
    register_command("pineap", handle_pineap_detection);
    register_command("apcred", handle_apcred);
    register_command("apenable", handle_ap_enable_cmd);
    register_command("chipinfo", handle_chip_info_cmd);
    register_command("rgbmode", handle_rgb_mode);
    register_command("setrgbpins", handle_setrgb);
    register_command("setrgbcount", handle_setrgbcount);
    register_command("sd_config", handle_sd_config);
    register_command("sd_pins_mmc", handle_sd_pins_mmc);
    register_command("sd_pins_spi", handle_sd_pins_spi);
    register_command("sd_save_config", handle_sd_save_config);
    register_command("sd", handle_sd_cmd);
    register_command("scanall", handle_scanall);
    register_command("sweep", handle_sweep_cmd);
    register_command("timezone", handle_timezone_cmd);
    register_command("settime", handle_settime_cmd);
    register_command("time", handle_time_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("listflippers", handle_list_flippers_cmd);
    register_command("selectflipper", handle_select_flipper_cmd);
    register_command("listgatt", handle_list_gatt_cmd);
    register_command("selectgatt", handle_select_gatt_cmd);
    register_command("enumgatt", handle_enum_gatt_cmd);
    register_command("trackgatt", handle_track_gatt_cmd);
#endif
    register_command("trackap", handle_track_ap_cmd);
    register_command("tracksta", handle_track_sta_cmd);
    #ifdef CONFIG_WITH_STATUS_DISPLAY
    register_command("statusidle", handle_status_idle_cmd);
    #endif
    register_command("dhcpstarve", handle_dhcpstarve_cmd);
    register_command("saeflood", handle_sae_flood_cmd);
    register_command("stopsaeflood", handle_stop_sae_flood_cmd);
    register_command("saefloodhelp", handle_sae_flood_help_cmd);
#if CONFIG_IDF_TARGET_ESP32C5
    register_command("setcountry", handle_setcountry);
#endif
    register_command("webauth", handle_web_auth_cmd);
    register_command("webuiap", handle_webuiap_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blespam", handle_ble_spam_cmd);
#endif
    register_command("setrgbmode", handle_set_rgb_mode_cmd);
    register_command("karma", handle_karma_cmd);
    register_command("setneopixelbrightness", handle_set_neopixel_brightness_cmd);
    register_command("getneopixelbrightness", handle_get_neopixel_brightness_cmd);
#ifdef CONFIG_HAS_INFRARED
    register_command("ir", handle_ir_cmd);
#endif
    register_command("nrf24", handle_nrf24_cmd);
    register_command("badusb", handle_badusb_cmd);
#ifdef CONFIG_WITH_ETHERNET
    register_command("ethup", handle_eth_up_cmd);
    register_command("ethdown", handle_eth_down_cmd);
    register_command("ethinfo", handle_eth_info_cmd);
    register_command("ethfp", handle_eth_fingerprint_cmd);
    register_command("etharp", handle_eth_arp_cmd);
    register_command("ethports", handle_eth_ports_cmd);
    register_command("ethping", handle_eth_ping_cmd);
    register_command("ethdns", handle_eth_dns_cmd);
    register_command("ethtrace", handle_eth_trace_cmd);
    register_command("ethstats", handle_eth_stats_cmd);
    register_command("ethconfig", handle_eth_config_cmd);
    register_command("ethmac", handle_eth_mac_cmd);
    register_command("ethserv", handle_eth_serv_cmd);
    register_command("ethntp", handle_eth_ntp_cmd);
    register_command("ethhttp", handle_eth_http_cmd);
    register_command("ethpoison", handle_eth_poison_cmd);
#endif
    register_command("mirror", handle_mirror_cmd);
    register_command("rave", handle_rave_cmd);
    register_command("raveport", handle_raveport_cmd);
    register_command("input", handle_input_cmd);
    register_command("iobtn", handle_iobtn_cmd);
    register_command("identify", handle_identify_cmd);
#if CONFIG_IDF_TARGET_ESP32S3
    register_command("usbkbd", handle_usb_kbd_cmd);
#endif
    register_command("aerialscan", handle_aerial_scan_cmd);
    register_command("aeriallist", handle_aerial_list_cmd);
    register_command("aerialtrack", handle_aerial_track_cmd);
    register_command("aerialstop", handle_aerial_stop_cmd);
    register_command("aerialspoof", handle_aerial_spoof_cmd);
    register_command("aerialspoofstop", handle_aerial_spoof_stop_cmd);
    register_command("wigle", handle_wigle_cmd);
#ifdef CONFIG_HAS_MIC
    register_command("mic_cal", handle_mic_cal_cmd);
#endif
    register_command("loadconfig", handle_loadconfig_cmd);

    esp_comm_manager_set_command_callback(comm_command_callback, NULL);

    glog("Registered Commands\n");
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_spam_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-apple") == 0) {
            glog("Starting Apple BLE spam...\n");
            ble_spam_start(BLE_SPAM_APPLE);
            return;
        }
        if (strcmp(argv[1], "-ms") == 0 || strcmp(argv[1], "-microsoft") == 0) {
            glog("Starting Microsoft BLE spam...\n");
            ble_spam_start(BLE_SPAM_MICROSOFT);
            return;
        }
        if (strcmp(argv[1], "-samsung") == 0) {
            glog("Starting Samsung BLE spam...\n");
            ble_spam_start(BLE_SPAM_SAMSUNG);
            return;
        }
        if (strcmp(argv[1], "-google") == 0) {
            glog("Starting Google BLE spam...\n");
            ble_spam_start(BLE_SPAM_GOOGLE);
            return;
        }
        if (strcmp(argv[1], "-random") == 0) {
            glog("Starting Random BLE spam...\n");
            ble_spam_start(BLE_SPAM_RANDOM);
            return;
        }
        if (strcmp(argv[1], "-s") == 0) {
            glog("Stopping BLE spam...\n");
            ble_spam_stop();
            return;
        }
    }
    glog("Usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
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
        glog("Error: Unsupported command '%s'\n", argv[2]);
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
