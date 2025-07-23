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

//Import handlers for commands
#include "core/cli-handlers/rgb-cli-handler.h"
#include "core/cli-handlers/sd-cli-handler.h"
#include "core/cli-handlers/system-cli-handler.h"
#include "core/cli-handlers/portal-cli-handler.h"
#include "core/cli-handlers/gps-cli-handler.h"
#include "core/cli-handlers/comm-cli-handler.h"
#include "core/cli-handlers/wifi-cli-handler.h"
#include "core/cli-handlers/ble-cli-handler.h"




static Command *command_list_head = NULL;
TaskHandle_t VisualizerHandle = NULL;
TaskHandle_t gps_info_task_handle = NULL;


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



// Forward declaration for the new print function
void wifi_manager_scanall_chart();



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
    register_command("stopportal", handle_stop_portal);
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
    
    esp_comm_manager_set_command_callback(comm_command_callback, NULL);
    
    printf("Registered Commands\n");
    TERMINAL_VIEW_ADD_TEXT("Registered Commands\n");
}
