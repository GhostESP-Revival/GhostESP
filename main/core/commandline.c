// command.c

#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/serial_manager.h"
#include "esp_sntp.h"
#include "managers/ap_manager.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include "managers/dial_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "core/esp_comm_manager.h"
#include "vendor/pcap.h"
#include "vendor/printer.h"
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
#include <time.h>
#include <dirent.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

static Command *command_list_head = NULL;
TaskHandle_t VisualizerHandle = NULL;
TaskHandle_t gps_info_task_handle = NULL;

// Forward declarations for command handlers
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
    printf("Unknown command: %s\n", cmd);
    TERMINAL_VIEW_ADD_TEXT("Unknown command: %s\n", cmd);
}

void cmd_wifi_scan_start(int argc, char **argv) {
    if (argc > 1) {
        int seconds = atoi(argv[1]);
        wifi_manager_start_scan_with_time(seconds);
    } else {
        wifi_manager_start_scan();
    }
    wifi_manager_print_scan_results_with_oui();
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
    
    printf("WiFi scan stopped.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan stopped.\n");
}

void cmd_wifi_scan_results(int argc, char **argv) {
    printf("WiFi scan results displaying with OUI matching.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan results displaying with OUI matching.\n");
    wifi_manager_print_scan_results_with_oui();
}

void handle_list(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        cmd_wifi_scan_results(argc, argv);
        return;
    } else if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        wifi_manager_list_stations();
        printf("Listed Stations...\n");
        TERMINAL_VIEW_ADD_TEXT("Listed Stations...\n");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    else if (argc > 1 && strcmp(argv[1], "-airtags") == 0) {
        ble_list_airtags();
        return;
    }
#endif
    else {
        printf("Usage: list -a (for Wi-Fi scan results)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: list -a (for Wi-Fi scan results)\n");
    }
}

void handle_beaconspam(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        printf("Starting Random beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Random beacon spam...\n");
        wifi_manager_start_beacon(NULL);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-rr") == 0) {
        printf("Starting Rickroll beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Rickroll beacon spam...\n");
        wifi_manager_start_beacon("RICKROLL");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        printf("Starting AP List beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting AP List beacon spam...\n");
        wifi_manager_start_beacon("APLISTMODE");
        return;
    }

    if (argc > 1) {
        wifi_manager_start_beacon(argv[1]);
        return;
    } else {
        printf("Usage: beaconspam -r (for Beacon Spam Random)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconspam -r (for Beacon Spam Random)\n");
    }
}

void handle_stop_spam(int argc, char **argv) {
    wifi_manager_stop_beacon();
    printf("Beacon Spam Stopped...\n");
    TERMINAL_VIEW_ADD_TEXT("Beacon Spam Stopped...\n");
}

void handle_sta_scan(int argc, char **argv) {
    wifi_manager_start_station_scan();
}

void handle_attack_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-d") == 0) {
            printf("Deauthentication starting...\n");
            TERMINAL_VIEW_ADD_TEXT("Deauthentication starting...\n");
            wifi_manager_deauth_station();
            return;
        } else if (strcmp(argv[1], "-e") == 0) {
            printf("EAPOL Logoff attack starting...\n");
            TERMINAL_VIEW_ADD_TEXT("EAPOL Logoff attack starting...\n");
            wifi_manager_start_eapollogoff_attack();
            return;
        } else if (strcmp(argv[1], "-s") == 0) {
            printf("SAE flood attack starting...\n");
            TERMINAL_VIEW_ADD_TEXT("SAE flood attack starting...\n");
            wifi_manager_start_sae_flood();
            return;
        }
    }
    printf("Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
}

void handle_sae_flood_cmd(int argc, char **argv) {
    printf("Starting SAE flood attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting SAE flood attack...\n");
    wifi_manager_start_sae_flood();
}

void handle_stop_sae_flood_cmd(int argc, char **argv) {
    printf("Stopping SAE flood attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Stopping SAE flood attack...\n");
    wifi_manager_stop_sae_flood();
}

void handle_sae_flood_help_cmd(int argc, char **argv) {
    wifi_manager_sae_flood_help();
}

void handle_stop_deauth(int argc, char **argv) {
    wifi_manager_stop_deauth();
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
    printf("Deauth/EAPOL/SAE attacks stopped...\n");
    TERMINAL_VIEW_ADD_TEXT("Deauth/EAPOL/SAE attacks stopped...\n");
}

void handle_select_cmd(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: select -a <number[,number,...]> or select -s <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: select -a <number[,number,...]> or select -s <number>\n");
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
                printf("Error: is not a valid number.\n");
                TERMINAL_VIEW_ADD_TEXT("Error: is not a valid number.\n");
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
                    printf("Error: '%s' is not a valid number.\n", token);
                    TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", token);
                    return;
                }
                token = strtok(NULL, ",");
            }
            
            if (count > 0) {
                wifi_manager_select_multiple_aps(indices, count);
            } else {
                printf("Error: No valid indices found.\n");
                TERMINAL_VIEW_ADD_TEXT("Error: No valid indices found.\n");
            }
        }
    } else if (strcmp(argv[1], "-s") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            wifi_manager_select_station(num);
        } else {
            printf("Error: is not a valid number.\n");
            TERMINAL_VIEW_ADD_TEXT("Error: is not a valid number.\n");
        }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    } else if (strcmp(argv[1], "-airtag") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            ble_select_airtag(num);
        } else {
            printf("Error: '%s' is not a valid number.\n", argv[2]);
            TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[2]);
        }
#endif
    } else {
        printf("Invalid option. Usage: select -a <number[,number,...]> or select -s <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid option. Usage: select -a <number[,number,...]> or select -s <number>\n");
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
        printf("Failed to init DIAL client.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to init DIAL client.\n");
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
    printf("Stopped activities.\nClosed files.\n");
    TERMINAL_VIEW_ADD_TEXT("Stopped activities.\nClosed files.\n");
}

void handle_dial_command(int argc, char **argv) {
    // Usage: dial [device_name]
    if (argc > 2) {
        printf("Usage: %s [device_name]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s [device_name]\n", argv[0]);
        return;
    }
    // If a device name is provided, set it before discovery
    if (argc == 2) {
        dial_manager_set_device_name(argv[1]);
    }
    xTaskCreate(&discover_task, "discover_task", 10240, NULL, 5, NULL);
}

void handle_wifi_connection(int argc, char **argv) {
    const char *ssid;
    const char *password;
    if (argc == 1) {
        // No args: use saved NVS credentials
        ssid = settings_get_sta_ssid(&G_Settings);
        password = settings_get_sta_password(&G_Settings);
        if (ssid == NULL || strlen(ssid) == 0) {
            printf("No saved SSID. Usage: %s \"<SSID>\" [\"<PASSWORD>\"]\n", argv[0]);
            TERMINAL_VIEW_ADD_TEXT("No saved SSID. Usage: %s \"<SSID>\" [\"<PASSWORD>\"]\n", argv[0]);
            return;
        }
        printf("Connecting using saved credentials: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Connecting using saved credentials: %s\n", ssid);
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
                printf("Error: Missing closing quote for SSID\n");
                TERMINAL_VIEW_ADD_TEXT("Error: Missing closing quote for SSID\n");
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
                    printf("Error: Missing closing quote for password\n");
                    TERMINAL_VIEW_ADD_TEXT("Error: Missing closing quote for password\n");
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
        printf("WiFi disconnect command sent successfully\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi disconnect command sent successfully\n");
    } else {
        printf("Failed to send disconnect command: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to send disconnect command\n");
    }
}

#ifndef CONFIG_IDF_TARGET_ESP32S2

void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        printf("Starting Find the Flippers.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Find the Flippers.\n");
        ble_start_find_flippers();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-ds") == 0) {
        printf("Starting BLE Spam Detector.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE Spam Detector.\n");
        ble_start_blespam_detector();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("Starting AirTag Scanner.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting AirTag Scanner.\n");
        ble_start_airtag_scanner();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        printf("Scanning for Raw Packets\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning for Raw Packets\n");
        ble_start_raw_ble_packetscan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping BLE Scan.\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping BLE Scan.\n");
        ble_stop();
        return;
    }

    printf("Invalid Command Syntax.\n");
    TERMINAL_VIEW_ADD_TEXT("Invalid Command Syntax.\n");
}

#endif

void handle_start_portal(int argc, char **argv) {
    if (argc < 3 || argc > 4) { // Accept 3 or 4 arguments
        printf("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        printf("PSK is optional for an open AP.\n");
        TERMINAL_VIEW_ADD_TEXT("PSK is optional for an open AP.\n");
        return;
    }
    const char *url = argv[1];
    const char *ap_ssid = argv[2];
    const char *psk = (argc == 4) ? argv[3] : ""; // Set PSK to empty if not provided
    if (strlen(url) >= MAX_PORTAL_PATH_LEN) {
        printf("Error: Provided Path is too long.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Path too long.\n");
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
            printf("Error: Path too long after prepending %s.\n", prefix);
            TERMINAL_VIEW_ADD_TEXT("Error: Path too long.\n");
            return;
        }
        memmove(final_url_or_path + prefix_len, final_url_or_path, current_len + 1);
        memcpy(final_url_or_path, prefix, prefix_len);
        printf("Prepended %s to path: %s\n", prefix, final_url_or_path);
        TERMINAL_VIEW_ADD_TEXT("Prepended %s to path: %s\n", prefix, final_url_or_path);
    }
    const char *domain = settings_get_portal_domain(&G_Settings);
    printf("Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, psk, domain ? domain : "(default)");
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
        printf("Usage: tp_link_test <on|off|loop>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: tp_link_test <on|off|loop>\n");
        return;
    }

    bool isloop = false;

    if (strcmp(argv[1], "loop") == 0) {
        isloop = true;
    } else if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        printf("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid argument. Use 'on', 'off', or 'loop'.\n");
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
            printf("Command too large to encrypt\n");
            TERMINAL_VIEW_ADD_TEXT("Command too large to encrypt\n");
            return;
        }

        encrypt_tp_link_command(command, encrypted_command, command_len);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("Failed to create socket: errno %d\n", errno);
            char err_buf[64];
            snprintf(err_buf, sizeof(err_buf), "Failed to create socket: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT(err_buf);
            return;
        }

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr,
                         sizeof(dest_addr));
        if (err < 0) {
            printf("Error occurred during sending: errno %d\n", errno);
            char err_buf[64];
            snprintf(err_buf, sizeof(err_buf), "Error occurred during sending: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT(err_buf);
            close(sock);
            return;
        }

        printf("Broadcast message sent: %s\n", command);
        TERMINAL_VIEW_ADD_TEXT("Broadcast message sent: %s\n", command);

        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr,
                           &addr_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("No response from any device\n");
                TERMINAL_VIEW_ADD_TEXT("No response from any device\n");
            } else {
                printf("Error receiving response: errno %d\n", errno);
                char err_buf[64];
                snprintf(err_buf, sizeof(err_buf), "Error receiving response: errno %d\n", errno);
                TERMINAL_VIEW_ADD_TEXT(err_buf);
            }
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            printf("Response: %s\n", decrypted_response);
            char resp_buf[140];
            snprintf(resp_buf, sizeof(resp_buf), "Response: %s\n", decrypted_response);
            TERMINAL_VIEW_ADD_TEXT(resp_buf);
        }

        close(sock);

        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}

void handle_ip_lookup(int argc, char **argv) {
    printf("Starting IP lookup...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting IP lookup...\n");
    wifi_manager_start_ip_lookup();
}

void handle_capture_scan(int argc, char **argv) {
    if (argc != 2) {
        printf("Error: Incorrect number of arguments.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Incorrect number of arguments.\n");
        return;
    }

    char *capturetype = argv[1];

    if (capturetype == NULL || capturetype[0] == '\0') {
        printf("Error: Capture Type cannot be empty.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Capture Type cannot be empty.\n");
        return;
    }

    if (strcmp(capturetype, "-probe") == 0) {
        printf("Starting probe request\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting probe request\npacket capture...\n");
        int err = pcap_file_open("probescan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_probe_scan_callback);
    }

    if (strcmp(capturetype, "-deauth") == 0) {
        int err = pcap_file_open("deauthscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_deauth_scan_callback);
    }

    if (strcmp(capturetype, "-beacon") == 0) {
        printf("Starting beacon\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting beacon\npacket capture...\n");
        int err = pcap_file_open("beaconscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_beacon_scan_callback);
    }

    if (strcmp(capturetype, "-raw") == 0) {
        printf("Starting raw\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting raw\npacket capture...\n");
        int err = pcap_file_open("rawscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
    }

    if (strcmp(capturetype, "-eapol") == 0) {
        printf("Starting EAPOL\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting EAPOL\npacket capture...\n");
        int err = pcap_file_open("eapolscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
    }

    if (strcmp(capturetype, "-pwn") == 0) {
        printf("Starting PWN\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting PWN\npacket capture...\n");
        int err = pcap_file_open("pwnscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_pwn_scan_callback);
    }

    if (strcmp(capturetype, "-wps") == 0) {
        printf("Starting WPS\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting WPS\npacket capture...\n");
        int err = pcap_file_open("wpsscan", PCAP_CAPTURE_WIFI);

        should_store_wps = 0;

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_wps_detection_callback);
    }

    if (strcmp(capturetype, "-stop") == 0) {
        printf("Stopping packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping packet capture...\n");
        wifi_manager_stop_monitor_mode();
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
        ble_stop_skimmer_detection();
#endif
        pcap_file_close();
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
    }

    if (strcmp(capturetype, "-skimmer") == 0) {
        printf("Skimmer detection started.\n");
        TERMINAL_VIEW_ADD_TEXT("Skimmer detection started.\n");
        int err = pcap_file_open("skimmer_scan", PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            printf("Warning: PCAP capture failed to start\n");
            TERMINAL_VIEW_ADD_TEXT("Warning: PCAP capture failed to start\n");
        } else {
            printf("PCAP capture started\nMonitoring devices\n");
            TERMINAL_VIEW_ADD_TEXT("PCAP capture started\nMonitoring devices\n");
        }
        // Start skimmer detection
        ble_start_skimmer_detection();

    }
#endif
}

void stop_portal(int argc, char **argv) {
    wifi_manager_stop_evil_portal();
    printf("Stopping evil portal...\n");
    TERMINAL_VIEW_ADD_TEXT("Stopping evil portal...\n");
}

void handle_reboot(int argc, char **argv) {
    printf("Rebooting system...\n");
    TERMINAL_VIEW_ADD_TEXT("Rebooting system...\n");
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
        printf("Wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving stopped.\n");
    } else {
        gps_manager_init(&g_gpsManager);
        if (sd_card_exists("/mnt/ghostesp/gps")) {
            esp_err_t err = csv_file_open("wardriving");
            if (err != ESP_OK) {
                printf("Failed to open CSV for wardriving\n");
                TERMINAL_VIEW_ADD_TEXT("Failed to open CSV for wardriving\n");
            }
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        printf("Wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving started.\n");
    }
}

void handle_timezone_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: timezone <TZ_STRING>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: timezone <TZ_STRING>\n");
        return;
    }
    const char *tz = argv[1];
    settings_set_timezone_str(&G_Settings, tz);
    settings_save(&G_Settings);
    setenv("TZ", tz, 1);
    tzset();
    printf("Timezone set to: %s\n", tz);
    TERMINAL_VIEW_ADD_TEXT("Timezone set to: %s\n", tz);
}

void handle_scan_ports(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n");
        TERMINAL_VIEW_ADD_TEXT("Usage:\n");
        printf("scanports local [-C/-A/start_port-end_port]\n");
        TERMINAL_VIEW_ADD_TEXT("scanports local [-C/-A/start_port-end_port]\n");
        printf("scanports [IP] [-C/-A/start_port-end_port]\n");
        TERMINAL_VIEW_ADD_TEXT("scanports [IP] [-C/-A/start_port-end_port]\n");
        return;
    }

    bool is_local = strcmp(argv[1], "local") == 0;
    const char *target_ip = NULL;
    const char *port_arg = NULL;

    // Parse arguments based on whether it's a local scan
    if (is_local) {
        if (argc < 3) {
            printf("Missing port argument for local scan\n");
            TERMINAL_VIEW_ADD_TEXT("Missing port argument for local scan\n");
            return;
        }
        port_arg = argv[2];
    } else {
        if (argc < 3) {
            printf("Missing port argument for IP scan\n");
            TERMINAL_VIEW_ADD_TEXT("Missing port argument for IP scan\n");
            return;
        }
        target_ip = argv[1];
        port_arg = argv[2];
    }

    if (is_local) {
        wifi_manager_scan_subnet();
        return;
    }

    host_result_t result;
    if (strcmp(port_arg, "-C") == 0) {
        scan_ports_on_host(target_ip, &result);
        if (result.num_open_ports > 0) {
            printf("Open ports on %s:\n", target_ip);
            char open_buf[64];
            snprintf(open_buf, sizeof(open_buf), "Open ports on %s:\n", target_ip);
            TERMINAL_VIEW_ADD_TEXT(open_buf);
            for (int i = 0; i < result.num_open_ports; i++) {
                printf("Port %d\n", result.open_ports[i]);
                char port_buf[32];
                snprintf(port_buf, sizeof(port_buf), "Port %d\n", result.open_ports[i]);
                TERMINAL_VIEW_ADD_TEXT(port_buf);
            }
        }
    } else {
        int start_port, end_port;
        if (strcmp(port_arg, "-A") == 0) {
            start_port = 1;
            end_port = 65535;
        } else if (sscanf(port_arg, "%d-%d", &start_port, &end_port) != 2 || start_port < 1 ||
                   end_port > 65535 || start_port > end_port) {
            printf("Invalid port range\n");
            TERMINAL_VIEW_ADD_TEXT("Invalid port range\n");
            return;
        }
        scan_ip_port_range(target_ip, start_port, end_port);
    }
}

void handle_crash(int argc, char **argv) {
    int *ptr = NULL;
    *ptr = 42;
}

void handle_help(int argc, char **argv) {
    printf("\n Ghost ESP Commands:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n Ghost ESP Commands:\n\n");

    printf("help\n");
    printf("    Description: Display this help message.\n");
    printf("    Usage: help\n\n");
    TERMINAL_VIEW_ADD_TEXT("help\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display this help message.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: help\n\n");

    printf("scanap\n");
    printf("    Description: Start a Wi-Fi access point (AP) scan.\n");
    printf("    Usage: scanap [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a Wi-Fi access point (AP) scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanap [seconds]\n\n");

    printf("scansta\n");
    printf("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    printf("    Usage: scansta\n\n");
    TERMINAL_VIEW_ADD_TEXT("scansta\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scansta\n\n");

    printf("stopscan\n");
    printf("    Description: Stop any ongoing Wi-Fi scan.\n");
    printf("    Usage: stopscan\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopscan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop any ongoing Wi-Fi scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopscan\n\n");

    printf("attack\n");
    printf("    Description: Launch an attack (e.g., deauthentication attack).\n");
    printf("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
    printf("    Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
    printf("    Arguments:\n");
    printf("        -d  : Start deauth attack (supports multiple APs)\n");
    printf("        -e  : Start EAPOL logoff attack\n");
    printf("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n");
    printf("\n");
    TERMINAL_VIEW_ADD_TEXT("attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Launch an attack (e.g., deauthentication attack).\n");
    TERMINAL_VIEW_ADD_TEXT("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -d  : Start deauth attack (supports multiple APs)\n");
    TERMINAL_VIEW_ADD_TEXT("        -e  : Start EAPOL logoff attack\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n");
    TERMINAL_VIEW_ADD_TEXT("\n");

    printf("list\n");
    printf("    Description: List Wi-Fi scan results or connected stations.\n");
    printf("    Usage: list -a | list -s | list -airtags\n");
    printf("    Arguments:\n");
    printf("        -a  : Show access points from Wi-Fi scan\n");
    printf("        -s  : List connected stations\n");
    printf("        -airtags: List discovered AirTags\n\n");
    TERMINAL_VIEW_ADD_TEXT("list\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: List Wi-Fi scan results or connected stations.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: list -a | list -s | list -airtags\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a  : Show access points from Wi-Fi scan\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : List connected stations\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtags: List discovered AirTags\n\n");

    printf("beaconspam\n");
    printf("    Description: Start beacon spam with different modes.\n");
    printf("    Usage: beaconspam [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -r   : Start random beacon spam\n");
    printf("        -rr  : Start Rickroll beacon spam\n");
    printf("        -l   : Start AP List beacon spam\n");
    printf("        [SSID]: Use specified SSID for beacon spam\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spam with different modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspam [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Start random beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -rr  : Start Rickroll beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -l   : Start AP List beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        [SSID]: Use specified SSID for beacon spam\n\n");

    printf("stopspam\n");
    printf("    Description: Stop ongoing beacon spam.\n");
    printf("    Usage: stopspam\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing beacon spam.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopspam\n\n");

    printf("stopdeauth\n");
    printf("    Description: Stop ongoing deauthentication attack.\n");
    printf("    Usage: stopdeauth\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopdeauth\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing deauthentication attack.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopdeauth\n\n");

    printf("select\n");
    printf("    Description: Select access point(s), station, or AirTag by index from the scan "
           "results.\n");
    printf("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
    printf("    Arguments:\n");
    printf("        -a      : AP selection index (supports multiple: 1,3,5)\n");
    printf("        -s      : Station selection index\n");
    printf("        -airtag : AirTag selection index\n");
    printf("    Examples:\n");
    printf("        select -a 4      : Select single AP at index 4\n");
    printf("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
    TERMINAL_VIEW_ADD_TEXT("select\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Select access point(s), station, or AirTag by index "
                           "from the scan results.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a      : AP selection index (supports multiple: 1,3,5)\n");
    TERMINAL_VIEW_ADD_TEXT("        -s      : Station selection index\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtag : AirTag selection index\n");
    TERMINAL_VIEW_ADD_TEXT("    Examples:\n");
    TERMINAL_VIEW_ADD_TEXT("        select -a 4      : Select single AP at index 4\n");
    TERMINAL_VIEW_ADD_TEXT("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");

    printf("startportal\n");
    printf("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
    printf("                 /mnt/ prefix is added automatically to file paths if missing.\n");
    printf("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    printf("           PSK is optional for an open network.\n");
    printf("    Use 'default' as the file path for the default Evil Portal.");
    TERMINAL_VIEW_ADD_TEXT("startportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Start Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          Use 'default' as the file path for the default Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          /mnt/ added to paths automatically.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    TERMINAL_VIEW_ADD_TEXT("           PSK is optional for an open network.\n");

    printf("evilportal\n");
    printf("    Description: Configure Evil Portal HTML content via UART buffer.\n");
    printf("    Usage: evilportal -c sethtmlstr\n");
    printf("    Steps:\n");
    printf("      1. Run: evilportal -c sethtmlstr\n");
    printf("      2. Send [HTML/BEGIN] marker over UART\n");
    printf("      3. Send HTML content over UART\n");
    printf("      4. Send [HTML/CLOSE] marker over UART\n");
    printf("      5. Run startportal (will use buffered HTML)\n");
    TERMINAL_VIEW_ADD_TEXT("evilportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Configure Evil Portal HTML via UART buffer.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: evilportal -c sethtmlstr\n");
    TERMINAL_VIEW_ADD_TEXT("    Steps: 1) evilportal -c sethtmlstr\n");
    TERMINAL_VIEW_ADD_TEXT("           2) Send [HTML/BEGIN] marker\n");
    TERMINAL_VIEW_ADD_TEXT("           3) Send HTML content\n");
    TERMINAL_VIEW_ADD_TEXT("           4) Send [HTML/CLOSE] marker\n");
    TERMINAL_VIEW_ADD_TEXT("           5) Run startportal\n");


    printf("stopportal\n");
    printf("    Description: Stop Evil Portal\n");
    printf("    Usage: stopportal\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop Evil Portal\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopportal\n\n");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    printf("blescan\n");
    printf("    Description: Handle BLE scanning with various modes.\n");
    printf("    Usage: blescan [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    printf("        -ds  : Start BLE spam detector\n");
    printf("        -a   : Start AirTag scanner\n");
    printf("        -r   : Scan for raw BLE packets\n");
    printf("        -s   : Stop BLE scanning\n\n");
    TERMINAL_VIEW_ADD_TEXT("blescan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Handle BLE scanning with various modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blescan [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    TERMINAL_VIEW_ADD_TEXT("        -ds  : Start BLE spam detector\n");
    TERMINAL_VIEW_ADD_TEXT("        -a   : Start AirTag scanner\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Scan for raw BLE packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -s   : Stop BLE scanning\n\n");

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
    TERMINAL_VIEW_ADD_TEXT("blespam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start BLE advertisement spam attacks.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blespam [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -apple     : Apple device spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -ms        : Microsoft Swift Pair spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -samsung   : Samsung Galaxy Watch spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -google    : Google Fast Pair spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -random    : Random spam (all types)\n");
    TERMINAL_VIEW_ADD_TEXT("        -s         : Stop BLE spam\n\n");
#endif

    printf("capture\n");
    printf("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    printf("    Usage: capture [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -probe   : Start Capturing Probe Packets\n");
    printf("        -beacon  : Start Capturing Beacon Packets\n");
    printf("        -deauth   : Start Capturing Deauth Packets\n");
    printf("        -raw   :   Start Capturing Raw Packets\n");
    printf("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    printf("        -pwn   :   Start Capturing Pwnagotchi Packets");
    printf("        -stop   : Stops the active capture\n\n");
    TERMINAL_VIEW_ADD_TEXT("capture\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: capture [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -probe   : Start Capturing Probe Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -beacon  : Start Capturing Beacon Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -deauth   : Start Capturing Deauth Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -raw   :   Start Capturing Raw Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    TERMINAL_VIEW_ADD_TEXT("        -pwn   :   Start Capturing Pwnagotchi Packets");
    TERMINAL_VIEW_ADD_TEXT("        -stop   : Stops the active capture\n\n");

    printf("connect\n");
    printf("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    printf("    Usage: connect <SSID> [Password]\n");
    TERMINAL_VIEW_ADD_TEXT("connect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: connect <SSID> [Password]\n");

    printf("dialconnect\n");
    printf("    Description: Cast a Random Youtube Video on all Smart TV's on "
           "your LAN (Requires You to Run Connect First)\n");
    printf("    Usage: dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Cast a Random Youtube Video on all Smart TV's on your "
                           "LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dialconnect\n");

    printf("powerprinter\n");
    printf("    Description: Print Custom Text to a Printer on your LAN "
           "(Requires You to Run Connect First)\n");
    printf("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    printf("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
           "Right, BR = Bottom Right, BL = Bottom Left\n\n");
    TERMINAL_VIEW_ADD_TEXT("powerprinter\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Print Custom Text to a Printer on "
                           "your LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    TERMINAL_VIEW_ADD_TEXT("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
                           "Right, BR = Bottom Right, BL = Bottom Left\n\n");

    printf("blewardriving\n");
    printf("    Description: Start/Stop BLE wardriving with GPS logging\n");
    printf("    Usage: blewardriving [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop BLE wardriving\n\n");
    TERMINAL_VIEW_ADD_TEXT("blewardriving\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop BLE wardriving with GPS logging\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blewardriving [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop BLE wardriving\n\n");

    printf("pineap\n");
    printf("    Description: Start/Stop detecting WiFi Pineapples.\n");
    printf("    Usage: pineap [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop PineAP detection\n\n");
    TERMINAL_VIEW_ADD_TEXT("pineap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop detecting WiFi Pineapples.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: pineap [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop PineAP detection\n\n");

    printf("Port Scanner\n");
    printf("    Description: Scan ports on local subnet or specific IP\n");
    printf("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    printf("           scanports [IP] [-C/-A/start_port-end_port]\n");
    printf("    Arguments:\n");
    printf("        -C  : Scan common ports only\n");
    printf("        -A  : Scan all ports (1-65535)\n");
    printf("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");
    TERMINAL_VIEW_ADD_TEXT("Port Scanner\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Scan ports on local subnet or specific IP\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("           scanports [IP] [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -C  : Scan common ports only\n");
    TERMINAL_VIEW_ADD_TEXT("        -A  : Scan all ports (1-65535)\n");
    TERMINAL_VIEW_ADD_TEXT("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");

    printf("congestion\n");
    printf("    Description: Display Wi-Fi channel congestion chart.\n");
    printf("    Usage: congestion\n\n");
    TERMINAL_VIEW_ADD_TEXT("congestion\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display Wi-Fi channel congestion chart.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: congestion\n\n");

    printf("apcred\n");
    printf("    Description: Change or reset the GhostNet AP credentials\n");
    printf("    Usage: apcred <ssid> <password>\n");
    printf("           apcred -r (reset to defaults)\n");
    printf("    Arguments:\n");
    printf("        <ssid>     : New SSID for the AP\n");
    printf("        <password> : New password (min 8 characters)\n");
    printf("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
    TERMINAL_VIEW_ADD_TEXT("apcred\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change or reset the GhostNet AP credentials\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: apcred <ssid> <password>\n");
    TERMINAL_VIEW_ADD_TEXT("           apcred -r (reset to defaults)\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        <ssid>     : New SSID for the AP\n");
    TERMINAL_VIEW_ADD_TEXT("        <password> : New password (min 8 characters)\n");
    TERMINAL_VIEW_ADD_TEXT("        -r        : Reset to default (GhostNet/GhostNet)\n\n");

    printf("apenable\n");
    printf("    Description: Enable or disable the Access Point across reboots\n");
    printf("    Usage: apenable <on|off>\n");
    printf("    Arguments:\n");
    printf("        on  : Enable the Access Point (requires restart)\n");
    printf("        off : Disable the Access Point (requires restart)\n\n");
    TERMINAL_VIEW_ADD_TEXT("apenable\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Enable or disable the Access Point across reboots\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: apenable <on|off>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        on  : Enable the Access Point (requires restart)\n");
    TERMINAL_VIEW_ADD_TEXT("        off : Disable the Access Point (requires restart)\n\n");

    printf("chipinfo\n");
    printf("    Description: Display chip information including model, revision, and features\n");
    printf("    Usage: chipinfo\n");
    printf("    Shows:\n");
    printf("        - Chip model and revision\n");
    printf("        - CPU cores and features\n");
    printf("        - Flash size and memory info\n");
    printf("        - ESP-IDF version\n\n");
    TERMINAL_VIEW_ADD_TEXT("chipinfo\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display chip information including model, revision, and features\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: chipinfo\n");
    TERMINAL_VIEW_ADD_TEXT("    Shows chip model, revision, CPU cores, features, flash size, and memory info\n\n");

    printf("rgbmode\n");
    printf("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    printf("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");
    TERMINAL_VIEW_ADD_TEXT("rgbmode\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");

    printf("setrgbpins\n");
    printf("    Description: Change RGB LED pins\n");
    printf("    Usage: setrgbpins <red> <green> <blue>\n");
    printf("           (use same value for all pins for single-pin LED strips)\n\n");
    TERMINAL_VIEW_ADD_TEXT("setrgbpins\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change RGB LED pins\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: setrgbpins <red> <green> <blue>\n");
    TERMINAL_VIEW_ADD_TEXT("           (use same value for all pins for single-pin LED strips)\n\n");

    // SD Card Commands Help Text
    printf("\n-- SD Card Pin Configuration --\n");
    printf("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    printf("These commands configure pins for the *active* mode.\n");
    printf("Changing the mode requires recompiling firmware.\n");
    TERMINAL_VIEW_ADD_TEXT("\n-- SD Card Pin Configuration --\n");
    TERMINAL_VIEW_ADD_TEXT("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    TERMINAL_VIEW_ADD_TEXT("These commands configure pins for the *active* mode.\n");
    TERMINAL_VIEW_ADD_TEXT("Changing the mode requires recompiling firmware.\n");

    printf("sd_config\n");
    printf("    Description: Show the currently configured GPIO pins for both SDMMC and SPI modes.\n");
    printf("    Usage: sd_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show current SD GPIO pin configuration.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_config\n\n");

    printf("sd_pins_mmc\n");
    printf("    Description: Set GPIO pins for SDMMC mode (1 or 4 bit). Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SDMMC mode.\n");
    printf("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    printf("    Example: sd_pins_mmc 19 18 20 21 22 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_mmc\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SDMMC mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SDMMC.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");

    printf("sd_pins_spi\n");
    printf("    Description: Set GPIO pins for SPI mode. Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SPI mode.\n");
    printf("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    printf("    Example: sd_pins_spi 5 18 19 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_spi\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SPI mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SPI.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");

    printf("sd_save_config\n");
    printf("    Description: Save the current SD pin configuration (both modes) to the SD card.\n");
    printf("                 Requires SD card to be mounted.\n");
    printf("    Usage: sd_save_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_save_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Save current SD pin config to SD card.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_save_config\n\n");

    printf("scanall\n");
    printf("    Description: Perform combined AP and Station scan, display results.\n");
    printf("    Usage: scanall [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanall\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Combined AP/STA scan & display.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanall [seconds]\n\n");

    printf("timezone\n");
    printf("    Description: Set the display timezone for the clock view.\n");
    printf("    Usage: timezone <TZ_STRING>\n\n");
    TERMINAL_VIEW_ADD_TEXT("timezone\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set the display timezone for the clock view.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: timezone <TZ_STRING>\n\n");

    printf("beaconadd\n");
    printf("    Description: Add an SSID to the beacon spam list.\n");
    printf("    Usage: beaconadd <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconadd\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Add an SSID to the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconadd <SSID>\n\n");

    printf("beaconremove\n");
    printf("    Description: Remove an SSID from the beacon spam list.\n");
    printf("    Usage: beaconremove <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconremove\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Remove an SSID from the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconremove <SSID>\n\n");

    printf("beaconclear\n");
    printf("    Description: Clear the beacon spam list.\n");
    printf("    Usage: beaconclear\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconclear\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Clear the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconclear\n\n");

    printf("beaconshow\n");
    printf("    Description: Show the current beacon spam list.\n");
    printf("    Usage: beaconshow\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconshow\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show the current beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconshow\n\n");

    printf("beaconspamlist\n");
    printf("    Description: Start beacon spamming using the beacon spam list.\n");
    printf("    Usage: beaconspamlist\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspamlist\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spamming using the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspamlist\n\n");

    printf("dhcpstarve\n");
    printf("    Description: DHCP starvation flood attack\n");
    printf("    Usage: dhcpstarve start [threads]\n");
    printf("           dhcpstarve stop\n");
    printf("           dhcpstarve display\n\n");
    TERMINAL_VIEW_ADD_TEXT("dhcpstarve\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: DHCP starvation flood attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dhcpstarve start [threads]\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve stop\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve display\n\n");

    printf("saeflood\n");
    printf("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
    printf("    Usage: saeflood (requires selected WPA3 AP)\n\n");
    TERMINAL_VIEW_ADD_TEXT("saeflood\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: saeflood (requires selected WPA3 AP)\n\n");

    printf("stopsaeflood\n");
    printf("    Description: Stop SAE flood attack\n");
    printf("    Usage: stopsaeflood\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopsaeflood\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop SAE flood attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopsaeflood\n\n");

    printf("saefloodhelp\n");
    printf("    Description: Show detailed SAE flood attack help\n");
    printf("    Usage: saefloodhelp\n\n");
    TERMINAL_VIEW_ADD_TEXT("saefloodhelp\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show detailed SAE flood attack help\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: saefloodhelp\n\n");

#if CONFIG_IDF_TARGET_ESP32C5
    printf("setcountry\n");
    printf("    Description: Set the Wi-Fi country code.\n");
    printf("    Usage: setcountry <CC>\n");
    printf("    Arguments:\n");
    printf("        <CC> : Two-letter ISO country code (e.g., US, GB, JP)\n\n");
    TERMINAL_VIEW_ADD_TEXT("setcountry\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set the Wi-Fi country code.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: setcountry <CC>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        <CC> : Two-letter ISO country code (e.g., US, GB, JP)\n\n");
#endif

    printf("listenprobes\n");
    printf("    Description: Listen for and log probe requests.\n");
    printf("    Usage: listenprobes [channel] [stop]\n");
    printf("    Arguments:\n");
    printf("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
    printf("        stop      : Stop probe request listening\n\n");
    TERMINAL_VIEW_ADD_TEXT("listenprobes\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Listen for and log probe requests.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: listenprobes [channel] [stop]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
    TERMINAL_VIEW_ADD_TEXT("        stop      : Stop probe request listening\n\n");

    printf("webauth\n");
    printf("    Description: Enable/disable web authentication.\n");
    printf("    Usage: webauth <enable|disable>\n\n");
    TERMINAL_VIEW_ADD_TEXT("webauth\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Enable/disable web authentication.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: webauth <enable|disable>\n\n");

    printf("commdiscovery\n");
    printf("    Description: Check discovery status (auto-starts on boot).\n");
    printf("    Usage: commdiscovery\n\n");
    TERMINAL_VIEW_ADD_TEXT("commdiscovery\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Check discovery status (auto-starts on boot).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commdiscovery\n\n");

    printf("commconnect\n");
    printf("    Description: Connect to a discovered peer ESP32.\n");
    printf("    Usage: commconnect <peer_name>\n");
    printf("    Example: commconnect ESP_A1B2C3\n\n");
    TERMINAL_VIEW_ADD_TEXT("commconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Connect to a discovered peer ESP32.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commconnect <peer_name>\n\n");

    printf("commsend\n");
    printf("    Description: Send a command to connected peer ESP32.\n");
    printf("    Usage: commsend <command> [data]\n");
    printf("    Example: commsend scanap\n");
    printf("    Example: commsend hello world\n\n");
    TERMINAL_VIEW_ADD_TEXT("commsend\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Send a command to connected peer ESP32.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commsend <command> [data]\n\n");

    printf("commstatus\n");
    printf("    Description: Show communication status and connection state.\n");
    printf("    Usage: commstatus\n\n");
    TERMINAL_VIEW_ADD_TEXT("commstatus\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show communication status and connection state.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commstatus\n\n");

    printf("commdisconnect\n");
    printf("    Description: Disconnect from current peer.\n");
    printf("    Usage: commdisconnect\n\n");
    TERMINAL_VIEW_ADD_TEXT("commdisconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Disconnect from current peer.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commdisconnect\n\n");

    printf("commsetpins\n");
    printf("    Description: Change communication GPIO pins at runtime.\n");
    printf("    Usage: commsetpins <tx_pin> <rx_pin>\n");
    printf("    Example: commsetpins 4 5\n\n");
    TERMINAL_VIEW_ADD_TEXT("commsetpins\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change communication GPIO pins at runtime.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commsetpins <tx_pin> <rx_pin>\n\n");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    printf("blescan\n");
    printf("    Description: Start Bluetooth Low Energy (BLE) scan.\n");
    printf("    Usage: blescan [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("blescan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start Bluetooth Low Energy (BLE) scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blescan [seconds]\n\n");
#endif
}

void handle_capture(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: capture [-probe|-beacon|-deauth|-raw|-ble]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: capture [-probe|-beacon|-deauth|-raw|-ble]\n");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(argv[1], "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
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
    } else {
        if (!g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            return;
        }

        ble_register_handler(ble_wardriving_callback);
        ble_start_scanning();
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
    }
}
#endif

void handle_pineap_detection(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping PineAP detection...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        return;
    }
    // Open PCAP file for logging detections
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        printf("Warning: Failed to open PCAP file for logging\n");
        TERMINAL_VIEW_ADD_TEXT("Warning: Failed to open PCAP file for logging\n");
    }

    // Start PineAP detection with channel hopping
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);

    printf("Monitoring for Pineapples\n");
    TERMINAL_VIEW_ADD_TEXT("Monitoring for Pineapples\n");
}


void handle_apcred(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: apcred <ssid> <password>\n");
        printf("       apcred -r (reset to defaults)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage:\napcred <ssid> <password>\n");
        TERMINAL_VIEW_ADD_TEXT("apcred -r\n");
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
            return;
        }

        printf("AP credentials reset to defaults (SSID: GhostNet, Password: GhostNet)\n");
        TERMINAL_VIEW_ADD_TEXT("AP reset to defaults:\nSSID: GhostNet\nPSK: GhostNet\n");
        return;
    }

    if (argc != 3) {
        printf("Error: Incorrect number of arguments.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Bad args\n");
        return;
    }

    const char *new_ssid = argv[1];
    const char *new_password = argv[2];

    if (strlen(new_password) < 8) {
        printf("Error: Password must be at least 8 characters\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Password must\nbe 8+ chars\n");
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
        printf("Error: Failed to save AP credentials\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Failed to\nsave credentials\n");
        return;
    }

    ap_manager_stop_services();
    esp_err_t err = ap_manager_start_services();
    if (err != ESP_OK) {
        printf("Error restarting AP: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Error restart AP:\n%s\n", esp_err_to_name(err));
        return;
    }

    printf("AP credentials updated - SSID: %s, Password: %s\n", saved_ssid, saved_password);
    TERMINAL_VIEW_ADD_TEXT("AP updated:\nSSID: %s\n", saved_ssid);
}

void handle_rgb_mode(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        return;
    }

    // Cancel any currently running LED effect task.
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }

    // Check for built-in modes first.
    if (strcasecmp(argv[1], "rainbow") == 0) {
        xTaskCreate(rainbow_task, "rainbow_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Rainbow mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Rainbow mode activated\n");
    } else if (strcasecmp(argv[1], "police") == 0) {
        xTaskCreate(police_task, "police_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Police mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Police mode activated\n");
    } else if (strcasecmp(argv[1], "strobe") == 0) {
        printf("SEIZURE WARNING\nPLEASE EXIT NOW IF\nYOU ARE SENSITIVE\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        xTaskCreate(strobe_task, "strobe_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Strobe mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Strobe mode activated\n");
    } else if (strcasecmp(argv[1], "off") == 0) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        if (!rgb_manager.is_separate_pins) {
            led_strip_clear(rgb_manager.strip);
            led_strip_refresh(rgb_manager.strip);
        }
        printf("RGB disabled\n");
        TERMINAL_VIEW_ADD_TEXT("RGB disabled\n");
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
            printf("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            TERMINAL_VIEW_ADD_TEXT("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            return;
        }
        // Set each LED to the selected static color.
        for (int i = 0; i < rgb_manager.num_leds; i++) {
            rgb_manager_set_color(&rgb_manager, i, r, g, b, false);
        }
        led_strip_refresh(rgb_manager.strip);
        printf("Static color mode activated: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Static color mode activated: %s\n", argv[1]);
    }
}

void handle_setrgb(int argc, char **argv) {
    if (argc != 4) {
        printf("Usage: setrgbpins <red> <green> <blue>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: setrgbpins <red> <green> <blue>\n");
        printf("           (use same value for all pins for single-pin LED strips)\n\n");
        TERMINAL_VIEW_ADD_TEXT("           (use same value for all pins for single-pin LED strips)\n\n");
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
            printf("Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            char rgb_buf[64];
            snprintf(rgb_buf, sizeof(rgb_buf), "Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            TERMINAL_VIEW_ADD_TEXT(rgb_buf);
        }
    } else {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               red_pin, green_pin, blue_pin);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, -1);
            settings_set_rgb_separate_pins(&G_Settings, red_pin, green_pin, blue_pin);
            settings_save(&G_Settings);
            printf("RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            char rgb_buf[64];
            snprintf(rgb_buf, sizeof(rgb_buf), "RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            TERMINAL_VIEW_ADD_TEXT(rgb_buf);
        }
    }
}

void handle_sd_config(int argc, char **argv) {
  sd_card_print_config();
}

void handle_sd_pins_mmc(int argc, char **argv) {
  if (argc != 7) {
    printf("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    printf("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
    TERMINAL_VIEW_ADD_TEXT("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
    printf("Example: sd_pins_mmc 19 18 20 21 22 23\n");
    TERMINAL_VIEW_ADD_TEXT("Example: sd_pins_mmc 19 18 20 21 22 23\n");
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
    printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    TERMINAL_VIEW_ADD_TEXT("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    return;
  }
  
  sd_card_set_mmc_pins(clk, cmd, d0, d1, d2, d3);
}

void handle_sd_pins_spi(int argc, char **argv) {
  if (argc != 5) {
    printf("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    printf("Sets pins for SPI mode (only effective if compiled for SPI).\n");
    TERMINAL_VIEW_ADD_TEXT("Sets pins for SPI mode (only effective if compiled for SPI).\n");
    printf("Example: sd_pins_spi 5 18 19 23\n");
    TERMINAL_VIEW_ADD_TEXT("Example: sd_pins_spi 5 18 19 23\n");
    return;
  }
  
  int cs = atoi(argv[1]);
  int clk = atoi(argv[2]);
  int miso = atoi(argv[3]);
  int mosi = atoi(argv[4]);
  
  if (cs < 0 || clk < 0 || miso < 0 || mosi < 0 ||
      cs > 40 || clk > 40 || miso > 40 || mosi > 40) {
    printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    TERMINAL_VIEW_ADD_TEXT("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    return;
  }
  
  sd_card_set_spi_pins(cs, clk, miso, mosi);
}

void handle_sd_save_config(int argc, char **argv) {
  sd_card_save_config();
}

void handle_congestion_cmd(int argc, char **argv) {
    wifi_manager_start_scan();

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_manager_get_scan_results_data(&ap_count, &ap_records);

    if (ap_count == 0 || ap_records == NULL) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
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

    printf("\nChannel Congestion:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\nChannel Congestion:\n\n");
    const char* header = "+----+-------+------------+\n";
    const char* separator = "+----+-------+------------+\n";
    const char* row_format = "| %2d | %5d | %s |\n";
    const char* footer = "+----+-------+------------+\n";

    printf("%s", header);
    TERMINAL_VIEW_ADD_TEXT("%s", header);
    printf("| CH | Count | Bar        |\n");
    TERMINAL_VIEW_ADD_TEXT("| CH | Count | Bar        |\n");
    printf("%s", separator);
    TERMINAL_VIEW_ADD_TEXT("%s", separator);

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
        printf(row_format, ch, cnt, display_bar);
        TERMINAL_VIEW_ADD_TEXT(row_format, ch, cnt, display_bar);
    }
    free(channels);
    free(counts);
    printf("%s", footer);
    TERMINAL_VIEW_ADD_TEXT("%s", footer);
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
            printf("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
            TERMINAL_VIEW_ADD_TEXT("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
        }
    }

    int ap_scan_seconds = total_seconds / 2;
    int sta_scan_seconds = total_seconds - ap_scan_seconds; // Use remaining time

    printf("Starting combined scan (%d sec AP, %d sec STA)...\n", ap_scan_seconds, sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("Starting combined scan (%ds AP, %ds STA)...\n", ap_scan_seconds, sta_scan_seconds);

    // 1. Perform AP Scan
    printf("--- Starting AP Scan (%d seconds) ---\n", ap_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("--- Starting AP Scan (%ds) ---\n", ap_scan_seconds);
    wifi_manager_start_scan_with_time(ap_scan_seconds);
    // Results are now in scanned_aps and ap_count

    // 2. Perform Station Scan
    printf("--- Starting Station Scan (%d seconds) ---\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("--- Starting STA Scan (%ds) ---\n", sta_scan_seconds);
    station_count = 0; // Reset station list before new scan
    wifi_manager_start_station_scan(); // Starts monitor mode + channel hopping
    printf("Station scan running for %d seconds...\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("Station scan running for %ds...\n", sta_scan_seconds);
    vTaskDelay(pdMS_TO_TICKS(sta_scan_seconds * 1000));
    wifi_manager_stop_monitor_mode(); // Stops monitor mode + channel hopping
    // Results are now in station_ap_list and station_count

    printf("--- Scan Complete ---\n");
    TERMINAL_VIEW_ADD_TEXT("--- Scan Complete ---\n");

    // 3. Print Combined Results
    wifi_manager_scanall_chart();

    // Ensure AP mode is restored if it was stopped
    ap_manager_start_services(); // Restore AP for WebUI
}

// Helper function to simplify calling list airtags
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv) {
    ble_list_airtags();
}
#endif

// Select AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_select_airtag(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: selectairtag <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: selectairtag <number>\n");
        return;
    }

    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_airtag(num);
    } else {
        printf("Error: '%s' is not a valid number.\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[1]);
    }
}
#endif

// Spoof AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_spoof_airtag(int argc, char **argv) {
    ble_start_spoofing_selected_airtag();
}
#endif

// Stop Spoof handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_stop_spoof(int argc, char **argv) {
    ble_stop_spoofing();
}
#endif

// Handlers for Flipper commands
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_flippers_cmd(int argc, char **argv) {
    ble_list_flippers();
}

void handle_select_flipper_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: selectflipper <index>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: selectflipper <index>\n");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_flipper(num);
    } else {
        printf("Error: '%s' is not a valid number.\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[1]);
    }
}
#endif

// New beacon list command handlers
void handle_beaconadd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: beaconadd <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconadd <SSID>\n");
        return;
    }
    wifi_manager_add_beacon_ssid(argv[1]);
}

void handle_beaconremove(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: beaconremove <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconremove <SSID>\n");
        return;
    }
    wifi_manager_remove_beacon_ssid(argv[1]);
}

void handle_beaconclear(int argc, char **argv) {
    wifi_manager_clear_beacon_list();
}

void handle_beaconshow(int argc, char **argv) {
    wifi_manager_show_beacon_list();
}

void handle_beaconspamlist(int argc, char **argv) {
    wifi_manager_start_beacon_list();
}

void handle_dhcpstarve_cmd(int argc, char **argv) {
    if (argc < 2) {
        wifi_manager_dhcpstarve_help();
    } else if (strcmp(argv[1], "start") == 0) {
        int thr = (argc >= 3) ? atoi(argv[2]) : 1;
        wifi_manager_start_dhcpstarve(thr);
    } else if (strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_dhcpstarve();
    } else if (strcmp(argv[1], "display") == 0) {
        wifi_manager_dhcpstarve_display();
    } else {
        wifi_manager_dhcpstarve_help();
    }
}
#if CONFIG_IDF_TARGET_ESP32C5
void handle_setcountry(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: setcountry <CC>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: setcountry <CC>\n");
        return;
    }
    wifi_country_t country = {
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
        .wifi_5g_channel_mask = 0
    };
    strncpy(country.cc, argv[1], sizeof(country.cc) - 1);
    country.cc[sizeof(country.cc) - 1] = '\0';
    esp_err_t err = esp_wifi_set_country(&country);
    if (err == ESP_OK) {
        printf("country set to %s\n", country.cc);
        TERMINAL_VIEW_ADD_TEXT("country set to %s\n", country.cc);
    } else {
        printf("failed to set country: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("failed to set country: %s\n", esp_err_to_name(err));
    }
}
#endif

void handle_listen_probes_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        g_listen_probes_save_to_sd = false;
        printf("Probe request listening stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("Probe request listening stopped.\n");
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
            printf("Starting to listen for probe requests on channel %d...\n", channel);
            TERMINAL_VIEW_ADD_TEXT("Starting to listen for probe requests on channel %d...\n", channel);
        } else {
            printf("Invalid channel: %s. Valid range: 1-%d\n", argv[1], MAX_WIFI_CHANNEL);
            TERMINAL_VIEW_ADD_TEXT("Invalid channel: %s. Valid range: 1-%d\n", argv[1], MAX_WIFI_CHANNEL);
            return;
        }
    } else {
        printf("Starting to listen for probe requests (channel hopping)...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting to listen for probe requests (channel hopping)...\n");
    }

    bool sd_available = sd_card_exists("/mnt/ghostesp/pcaps");
    g_listen_probes_save_to_sd = sd_available;
    if (sd_available) {
        int err = pcap_file_open("probelisten", PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            printf("Warning: PCAP file open failed; probes will not be saved to SD card.\n");
            TERMINAL_VIEW_ADD_TEXT("Warning: PCAP file open failed.\n");
            g_listen_probes_save_to_sd = false;
        }
    } else {
        printf("SD card not available; probe PCAP disabled.\n");
        TERMINAL_VIEW_ADD_TEXT("SD card not available; probe PCAP disabled.\n");
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
        printf("Usage: webauth <on|off>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: webauth <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        settings_set_web_auth_enabled(&G_Settings, true);
        settings_save(&G_Settings);
        printf("Web authentication enabled.\n");
        TERMINAL_VIEW_ADD_TEXT("Web authentication enabled.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        settings_set_web_auth_enabled(&G_Settings, false);
        settings_save(&G_Settings);
        printf("Web authentication disabled.\n");
        TERMINAL_VIEW_ADD_TEXT("Web authentication disabled.\n");
    } else {
        printf("Usage: webauth <on|off>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: webauth <on|off>\n");
    }
}


void handle_listportals(int argc, char **argv);
void handle_evilportal(int argc, char **argv);
void handle_wifi_disconnect(int argc, char **argv);
void handle_set_rgb_mode_cmd(int argc, char **argv);

void handle_comm_discovery(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    
    if (state == COMM_STATE_SCANNING) {
        printf("Already in discovery mode. Listening for peers...\n");
        TERMINAL_VIEW_ADD_TEXT("Already in discovery mode. Listening for peers...\n");
        return;
    }
    
    if (esp_comm_manager_start_discovery()) {
        printf("Started discovery mode. Listening for peers...\n");
        TERMINAL_VIEW_ADD_TEXT("Started discovery mode. Listening for peers...\n");
    } else {
        printf("Failed to start discovery. Check if already connected.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to start discovery. Check if already connected.\n");
    }
}

void handle_comm_connect(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: commconnect <peer_name>\n");
        printf("Example: commconnect ESP_A1B2C3\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commconnect <peer_name>\n");
        return;
    }
    
    if (esp_comm_manager_connect_to_peer(argv[1])) {
        printf("Attempting to connect to peer: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Attempting to connect to peer...\n");
    } else {
        printf("Failed to connect. Make sure you're in discovery mode first.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to connect. Start discovery first.\n");
    }
}

void handle_comm_send(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: commsend <command> [data]\n");
        printf("Example: commsend hello world\n");
        printf("Example: commsend scanap\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commsend <command> [data]\n");
        return;
    }
    
    if (!esp_comm_manager_is_connected()) {
        printf("Not connected to any peer. Use 'commdiscovery' and 'commconnect' first.\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected. Connect to a peer first.\n");
        return;
    }
    
    char data_buffer[256] = {0};
    if (argc > 2) {
        int offset = 0;
        for (int i = 2; i < argc; i++) {
            int remaining = sizeof(data_buffer) - offset;
            int written = snprintf(data_buffer + offset, remaining, "%s ", argv[i]);
            if (written >= remaining) {
                printf("W: Command data truncated.\n");
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
            printf("Command sent: %s %s\n", command, data);
        } else {
            printf("Command sent: %s\n", command);
        }
        TERMINAL_VIEW_ADD_TEXT("Command sent successfully.\n");
    } else {
        printf("Failed to send command.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send command.\n");
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
    
    printf("Communication Status: %s\n", state_str);
    if (esp_comm_manager_is_connected()) {
        printf("Connected to peer. Ready to send commands.\n");
        TERMINAL_VIEW_ADD_TEXT("Status: Connected\n");
    } else {
        printf("Not connected. Use 'commdiscovery' to find peers.\n");
        TERMINAL_VIEW_ADD_TEXT("Status: Not connected\n");
    }
}

void handle_comm_disconnect(int argc, char **argv) {
    esp_comm_manager_disconnect();
    printf("Disconnected from peer.\n");
    TERMINAL_VIEW_ADD_TEXT("Disconnected from peer.\n");
}

void handle_comm_setpins(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: commsetpins <tx_pin> <rx_pin>\n");
        printf("Example: commsetpins 4 5\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: commsetpins <tx_pin> <rx_pin>\n");
        return;
    }
    
    int tx_pin = atoi(argv[1]);
    int rx_pin = atoi(argv[2]);
    
    if (tx_pin < 0 || tx_pin > 48 || rx_pin < 0 || rx_pin > 48) {
        printf("Invalid pin numbers. Must be between 0-48.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid pin numbers.\n");
        return;
    }
    
    if (esp_comm_manager_set_pins((gpio_num_t)tx_pin, (gpio_num_t)rx_pin)) {
        settings_set_esp_comm_pins(&G_Settings, tx_pin, rx_pin);
        settings_save(&G_Settings);
        
        printf("Communication pins changed to TX:%d RX:%d and saved to NVS\n", tx_pin, rx_pin);
        TERMINAL_VIEW_ADD_TEXT("Communication pins changed and saved.\n");
    } else {
        printf("Failed to change pins. Make sure not connected or scanning.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to change pins.\n");
    }
}

static void comm_command_callback(const char* command, const char* data, void* user_data) {
    char log_buf[512];
    if (data && strlen(data) > 0) {
        snprintf(log_buf, sizeof(log_buf), "Received command from peer: %s with data: %s\n", command, data);
    } else {
        snprintf(log_buf, sizeof(log_buf), "Received command from peer: %s\n", command);
    }
    printf("%s", log_buf);
    TERMINAL_VIEW_ADD_TEXT(log_buf);
    
    char full_command[256];
    if (data && strlen(data) > 0) {
        snprintf(full_command, sizeof(full_command), "%s %s", command, data);
    } else {
        snprintf(full_command, sizeof(full_command), "%s", command);
    }
    
    snprintf(log_buf, sizeof(log_buf), "Executing received command: %s\n", full_command);
    printf("%s", log_buf);
    TERMINAL_VIEW_ADD_TEXT(log_buf);
    esp_comm_manager_set_remote_command_flag(true);
    handle_serial_command(full_command);
    esp_comm_manager_set_remote_command_flag(false);
}
void handle_ap_enable_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: apenable <on|off>\n");
        printf("Example: apenable on\n");
        printf("         apenable off\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: apenable <on|off>\n");
        return;
    }
    
    bool enable = false;
    if (strcmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        printf("Invalid argument. Use 'on' or 'off'\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid argument. Use 'on' or 'off'\n");
        return;
    }
    
    settings_set_ap_enabled(&G_Settings, enable);
    settings_save(&G_Settings);
    
    printf("Access Point %s. Restart required to take effect.\n", enable ? "enabled" : "disabled");
    TERMINAL_VIEW_ADD_TEXT(enable ? "Access Point enabled. Restart required.\n" : "Access Point disabled. Restart required.\n");
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
    
    printf("Chip Information:\n");
    printf("  Model: %s\n", model_name);
    printf("  Revision: v%d.%d\n", major_rev, minor_rev);
    printf("  CPU Cores: %d\n", chip_info.cores);
    
    printf("  Features: ");
    bool first = true;
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) {
        printf("WiFi");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_BT) {
        if (!first) printf("/");
        printf("BT");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_BLE) {
        if (!first) printf("/");
        printf("BLE");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_IEEE802154) {
        if (!first) printf("/");
        printf("802.15.4");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH) {
        if (!first) printf("/");
        printf("Embedded Flash");
        first = false;
    }
    if (chip_info.features & CHIP_FEATURE_EMB_PSRAM) {
        if (!first) printf("/");
        printf("Embedded PSRAM");
        first = false;
    }
    if (first) {
        printf("None");
    }
    printf("\n");
    
    printf("  Free Heap: %lu bytes\n", esp_get_free_heap_size());
    printf("  Min Free Heap: %lu bytes\n", esp_get_minimum_free_heap_size());
    printf("  IDF Version: %s\n", esp_get_idf_version());
    
    TERMINAL_VIEW_ADD_TEXT("Chip Information:\n");
    char info_buffer[512];
    snprintf(info_buffer, sizeof(info_buffer), 
             "  Model: %s\n  Revision: v%d.%d\n  CPU Cores: %d\n  Free Heap: %lu bytes\n",
             model_name, major_rev, minor_rev, chip_info.cores, esp_get_free_heap_size());
    TERMINAL_VIEW_ADD_TEXT(info_buffer);
}

void register_commands() {
    command_init();
    register_command("help", handle_help);
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
    register_command("congestion", handle_congestion_cmd);
    register_command("listenprobes", handle_listen_probes_cmd);
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
    
    esp_comm_manager_set_command_callback(comm_command_callback, NULL);
    
    printf("Registered Commands\n");
    TERMINAL_VIEW_ADD_TEXT("Registered Commands\n");
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_spam_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-apple") == 0) {
            printf("starting apple ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Apple BLE spam...\n");
            ble_start_ble_spam(BLE_SPAM_APPLE);
            return;
        }
        if (strcmp(argv[1], "-ms") == 0 || strcmp(argv[1], "-microsoft") == 0) {
            printf("starting microsoft ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Microsoft BLE spam...\n");
            ble_start_ble_spam(BLE_SPAM_MICROSOFT);
            return;
        }
        if (strcmp(argv[1], "-samsung") == 0) {
            printf("starting samsung ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Samsung BLE spam...\n");
            ble_start_ble_spam(BLE_SPAM_SAMSUNG);
            return;
        }
        if (strcmp(argv[1], "-google") == 0) {
            printf("starting google ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Google BLE spam...\n");
            ble_start_ble_spam(BLE_SPAM_GOOGLE);
            return;
        }
        if (strcmp(argv[1], "-random") == 0) {
            printf("starting random ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Random BLE spam...\n");
            ble_start_ble_spam(BLE_SPAM_RANDOM);
            return;
        }
        if (strcmp(argv[1], "-s") == 0) {
            printf("stopping ble spam...\n");
            TERMINAL_VIEW_ADD_TEXT("Stopping BLE spam...\n");
            ble_stop_ble_spam();
            return;
        }
    }
    printf("usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
}
#endif

void handle_listportals(int argc, char **argv) {
    char portal_names[MAX_PORTALS][MAX_PORTAL_NAME];
    int count = get_evil_portal_list(portal_names);

    if (count <= 0) {
        printf("No portals found.\n");
        TERMINAL_VIEW_ADD_TEXT("No portals found.\n");
        return;
    }

    printf("Available Evil Portals:\n");
    TERMINAL_VIEW_ADD_TEXT("Available Evil Portals:\n");
    for (int i = 0; i < count; ++i) {
        printf("  %.508s\n", portal_names[i]);
        TERMINAL_VIEW_ADD_TEXT("  %.508s\n", portal_names[i]);
    }
}

void handle_evilportal(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s -c <command>\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s -c <command>\n", argv[0]);
        printf("Commands:\n");
        printf("  sethtmlstr - Set HTML content from buffer (use with UART markers)\n");
        TERMINAL_VIEW_ADD_TEXT("Commands:\n");
        TERMINAL_VIEW_ADD_TEXT("  sethtmlstr - Set HTML content from buffer\n");
        return;
    }

    if (strcmp(argv[1], "-c") != 0) {
        printf("Error: Expected -c flag\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Expected -c flag\n");
        return;
    }

    if (strcmp(argv[2], "sethtmlstr") == 0) {
        wifi_manager_set_html_from_uart();
        printf("HTML buffer mode enabled for evil portal\n");
        TERMINAL_VIEW_ADD_TEXT("HTML buffer mode enabled for evil portal\n");
    } else {
        printf("Error: Unknown command '%s'\n", argv[2]);
        TERMINAL_VIEW_ADD_TEXT("Error: Unknown command '%s'\n", argv[2]);
    }
}

void handle_set_rgb_mode_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: setrgbmode <normal|rainbow|stealth>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: setrgbmode <normal|rainbow|stealth>\n");
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
        printf("Invalid mode '%s'. Supported modes: normal, rainbow, stealth\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Invalid mode '%s'. Supported modes: normal, rainbow, stealth\n", argv[1]);
        return;
    }
    settings_set_rgb_mode(&G_Settings, mode);
    settings_save(&G_Settings);
    printf("RGB mode set to %s\n", argv[1]);
    TERMINAL_VIEW_ADD_TEXT("RGB mode set to %s\n", argv[1]);
}





