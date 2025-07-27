#include "core/cli-handlers/misc-cli-handler.h"
#include "core/callbacks.h"
#include "managers/gps_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/sd_card_manager.h"
#include "managers/wifi_manager.h"
#include "managers/settings_manager.h"
#include "vendor/pcap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

extern GPSManager g_gpsManager;
extern FSettings G_Settings;

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
        printf("\nWi-Fi Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nWi-Fi Commands:\n\n");
        printf("scanap\n    Start a Wi-Fi access point (AP) scan.\n    Usage: scanap [seconds]\n\n");
        printf("scansta\n    Start scanning for Wi-Fi stations.\n    Usage: scansta\n\n");
        printf("attack\n    Launch Wi-Fi attacks (deauth, EAPOL, SAE flood).\n    Usage: attack -d|-e|-s\n\n");
        printf("list\n    List Wi-Fi scan results or stations.\n    Usage: list -a|-s\n\n");
        printf("beaconspam\n    Start beacon spam.\n    Usage: beaconspam [option]\n\n");
        printf("stopspam\n    Stop beacon spam.\n    Usage: stopspam\n\n");
        printf("stopdeauth\n    Stop deauth/EAPOL/SAE attacks.\n    Usage: stopdeauth\n\n");
        printf("select\n    Select APs or stations.\n    Usage: select -a|-s <index>\n\n");
        printf("apcred\n    Change/reset AP credentials.\n    Usage: apcred <ssid> <password> | apcred -r\n\n");
        printf("apenable\n    Enable/disable AP.\n    Usage: apenable <on|off>\n\n");
        printf("scanall\n    Combined AP/STA scan.\n    Usage: scanall [seconds]\n\n");
        printf("congestion\n    Show Wi-Fi channel congestion.\n    Usage: congestion\n\n");
        printf("stopscan\n    Stop any ongoing Wi-Fi scan.\n    Usage: stopscan\n\n");
        printf("connect\n    Connects to Specific WiFi Network and saves credentials.\n    Usage: connect <SSID> [Password]\n\n");
        printf("listenprobes\n    Listen for and log probe requests.\n    Usage: listenprobes [channel] [stop]\n    Arguments:\n        [channel] : Listen on specific channel (1-165), omit for channel hopping\n        stop      : Stop probe request listening\n\n");
#if CONFIG_IDF_TARGET_ESP32C5
        printf("setcountry\n    Set the Wi-Fi country code.\n    Usage: setcountry <CC>\n    Arguments:\n        <CC> : Two-letter ISO country code (e.g., US, GB, JP)\n\n");
#endif
        TERMINAL_VIEW_ADD_TEXT("scanap, scansta, attack, list, beaconspam, stopspam, stopdeauth, select, apcred, apenable, scanall, congestion, stopscan, connect, listenprobes");
#if CONFIG_IDF_TARGET_ESP32C5
        TERMINAL_VIEW_ADD_TEXT(", setcountry");
#endif
        TERMINAL_VIEW_ADD_TEXT("\n");
        return;
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(category, "ble") == 0) {
        printf("\nBLE Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nBLE Commands:\n\n");
        printf("blescan\n    Start BLE scan.\n    Usage: blescan [option]\n\n");
        printf("blespam\n    Start BLE spam.\n    Usage: blespam [option]\n\n");
        printf("blewardriving\n    Start BLE wardriving.\n    Usage: blewardriving [-s]\n\n");
        printf("list -airtags\n    List AirTags.\n    Usage: list -airtags\n\n");
        printf("select -airtag <index>\n    Select AirTag by index.\n\n");
        TERMINAL_VIEW_ADD_TEXT("blescan, blespam, blewardriving, list -airtags, select -airtag\n");
        return;
    }
#endif

    if (strcmp(category, "comm") == 0) {
        printf("\nCommunication Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nCommunication Commands:\n\n");
        printf("commdiscovery\n    Check discovery status.\n    Usage: commdiscovery\n\n");
        printf("commconnect\n    Connect to a discovered peer ESP32.\n    Usage: commconnect <peer_name>\n\n");
        printf("commsend\n    Send a command to connected peer ESP32.\n    Usage: commsend <command> [data]\n\n");
        printf("commstatus\n    Show communication status.\n    Usage: commstatus\n\n");
        printf("commdisconnect\n    Disconnect from current peer.\n    Usage: commdisconnect\n\n");
        printf("commsetpins\n    Change communication GPIO pins.\n    Usage: commsetpins <tx_pin> <rx_pin>\n\n");
        TERMINAL_VIEW_ADD_TEXT("commdiscovery, commconnect, commsend, commstatus, commdisconnect, commsetpins\n");
        return;
    }

    if (strcmp(category, "sd") == 0) {
        printf("\nSD Card Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nSD Card Commands:\n\n");
        printf("sd_config\n    Show current SD GPIO pin configuration.\n    Usage: sd_config\n\n");
        printf("sd_pins_mmc\n    Set GPIO pins for SDMMC mode.\n    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");
        printf("sd_pins_spi\n    Set GPIO pins for SPI mode.\n    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");
        printf("sd_save_config\n    Save current SD pin config to SD card.\n    Usage: sd_save_config\n\n");
        TERMINAL_VIEW_ADD_TEXT("sd_config, sd_pins_mmc, sd_pins_spi, sd_save_config\n");
        return;
    }

    if (strcmp(category, "led") == 0) {
        printf("\nLED & RGB Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nLED & RGB Commands:\n\n");
        printf("rgbmode\n    Control LED effects (rainbow, police, strobe, off)\n    Usage: rgbmode <rainbow|police|strobe|off|color>\n\n");
        printf("setrgbpins\n    Change RGB LED pins\n    Usage: setrgbpins <red> <green> <blue>\n           (use same value for all pins for single-pin LED strips)\n\n");
        TERMINAL_VIEW_ADD_TEXT("rgbmode, setrgbpins\n");
        return;
    }

    if (strcmp(category, "misc") == 0) {
        printf("\nMiscellaneous Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nMiscellaneous Commands:\n\n");
        printf("help\n    Show help menu.\n    Usage: help [category]\n\n");
        printf("chipinfo\n    Show chip info.\n    Usage: chipinfo\n\n");
        printf("timezone\n    Set timezone.\n    Usage: timezone <TZ_STRING>\n\n");
        printf("webauth\n    Enable/disable web authentication.\n    Usage: webauth <on|off>\n\n");
        printf("pineap\n    Start/stop PineAP detection.\n    Usage: pineap [-s]\n\n");
        printf("scanports\n    Scan ports.\n    Usage: scanports local|IP [option]\n\n");
        printf("tp_link_test\n    Test TP-Link smart plug.\n    Usage: tp_link_test <on|off|loop>\n\n");
        TERMINAL_VIEW_ADD_TEXT("help, chipinfo, timezone, webauth, pineap, scanports, tp_link_test\n");
        return;
    }
    if (strcmp(category, "gps") == 0) {
        printf("\nGPS Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nGPS Commands:\n\n");
        printf("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo\n\n");
        printf("startwd\n    Start GPS wardriving.\n    Usage: startwd [seconds]\n\n");
        TERMINAL_VIEW_ADD_TEXT("gpsinfo, startwd\n");
        return;
    }
    if (strcmp(category, "portal") == 0) {
        printf("\nEvil Portal Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nEvil Portal Commands:\n\n");
        printf("startportal\n    Start an Evil Portal.\n    Usage: startportal [FilePath] [AP_SSID] [PSK]\n\n");
        printf("stopportal\n    Stop Evil Portal.\n    Usage: stopportal\n\n");
        printf("listportals\n    List available Evil Portal files.\n    Usage: listportals\n\n");
        TERMINAL_VIEW_ADD_TEXT("startportal, stopportal, listportals\n");
        return;
    }

    if (strcmp(category, "printer") == 0) {
        printf("\nPrinter Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nPrinter Commands:\n\n");
        printf("powerprinter\n    Print custom text to a network printer.\n    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n\n");
        TERMINAL_VIEW_ADD_TEXT("powerprinter\n");
        TERMINAL_VIEW_ADD_TEXT("    Print custom text to a network printer.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n\n");
        return;
    }

    if (strcmp(category, "cast") == 0) {
        printf("\nYouTube Cast Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nYouTube Cast Commands:\n\n");
        printf("dialconnect\n    Cast a random YouTube video to all smart TVs on your LAN.\n    Usage: dialconnect\n\n");
        TERMINAL_VIEW_ADD_TEXT("dialconnect\n");
        TERMINAL_VIEW_ADD_TEXT("    Cast a random YouTube video to all smart TVs on your LAN.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: dialconnect\n\n");
        return;
    }

    if (strcmp(category, "capture") == 0) {
        printf("\nCapture Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nCapture Commands:\n\n");
        printf("capture\n    Start a WiFi packet capture.\n    Usage: capture [OPTION]\n");
        printf("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -stop\n\n");
        TERMINAL_VIEW_ADD_TEXT("capture\n");
        TERMINAL_VIEW_ADD_TEXT("    Start a WiFi packet capture.\n");
        TERMINAL_VIEW_ADD_TEXT("    Usage: capture [OPTION]\n");
        TERMINAL_VIEW_ADD_TEXT("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -stop\n\n");
        return;
    }

    if (strcmp(category, "beacon") == 0) {
        printf("\nBeacon Spam Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nBeacon Spam Commands:\n\n");
        printf("beaconadd\n    Add an SSID to the beacon spam list.\n    Usage: beaconadd <SSID>\n\n");
        printf("beaconremove\n    Remove an SSID from the beacon spam list.\n    Usage: beaconremove <SSID>\n\n");
        printf("beaconclear\n    Clear the beacon spam list.\n    Usage: beaconclear\n\n");
        printf("beaconshow\n    Show the current beacon spam list.\n    Usage: beaconshow\n\n");
        printf("beaconspamlist\n    Start beacon spamming using the beacon spam list.\n    Usage: beaconspamlist\n\n");
        TERMINAL_VIEW_ADD_TEXT("beaconadd, beaconremove, beaconclear, beaconshow, beaconspamlist\n");
        return;
    }

    if (strcmp(category, "attack") == 0) {
        printf("\nAttack Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nAttack Commands:\n\n");
        printf("dhcpstarve\n    DHCP starvation flood attack.\n    Usage: dhcpstarve start [threads], dhcpstarve stop, dhcpstarve display\n\n");
        printf("saeflood\n    SAE handshake flooding attack (ESP32-C5/C6 only).\n    Usage: saeflood\n\n");
        printf("stopsaeflood\n    Stop SAE flood attack.\n    Usage: stopsaeflood\n\n");
        printf("saefloodhelp\n    Show detailed SAE flood attack help.\n    Usage: saefloodhelp\n\n");
        TERMINAL_VIEW_ADD_TEXT("dhcpstarve, saeflood, stopsaeflood, saefloodhelp\n");
        return;
    }
    
    printf("\nGhost ESP Command Categories:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\nGhost ESP Command Categories:\n\n");

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


// Wardriving command
void handle_startwd(int argc, char **argv) {
    if (argc > 2) {
        printf("Usage: startwd [-s]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: startwd [-s]\n");
        return;
    }
    bool stop_flag = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
        } else {
            printf("Unknown argument: %s\nUsage: startwd [-s]\n", argv[i]);
            TERMINAL_VIEW_ADD_TEXT("Unknown argument: %s\nUsage: startwd [-s]\n", argv[i]);
            return;
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

// Port scan command
void handle_scan_ports(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage:\nscanports local [-C/-A/start_port-end_port]\nscanports [IP] [-C/-A/start_port-end_port]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage:\nscanports local [-C/-A/start_port-end_port]\nscanports [IP] [-C/-A/start_port-end_port]\n");
        return;
    }
    bool is_local = strcmp(argv[1], "local") == 0;
    const char *target_ip = NULL;
    const char *port_arg = NULL;
    if (is_local) {
        port_arg = argv[2];
        if (argc > 3) {
            printf("Too many arguments for local scan\n");
            TERMINAL_VIEW_ADD_TEXT("Too many arguments for local scan\n");
            return;
        }
        wifi_manager_scan_subnet();
        return;
    } else {
        target_ip = argv[1];
        port_arg = argv[2];
        if (argc > 3) {
            printf("Too many arguments for IP scan\n");
            TERMINAL_VIEW_ADD_TEXT("Too many arguments for IP scan\n");
            return;
        }
    }
    host_result_t result;
    if (strcmp(port_arg, "-C") == 0) {
        scan_ports_on_host(target_ip, &result);
        if (result.num_open_ports > 0) {
            printf("Open ports on %s:\n", target_ip);
            TERMINAL_VIEW_ADD_TEXT("Open ports on %s:\n", target_ip);
            for (int i = 0; i < result.num_open_ports; i++) {
                printf("Port %d\n", result.open_ports[i]);
                TERMINAL_VIEW_ADD_TEXT("Port %d\n", result.open_ports[i]);
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

// Web authentication command
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

// PineAP detection command
void handle_pineap_detection(int argc, char **argv) {
    if (argc > 2) {
        printf("Usage: pineap [-s]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: pineap [-s]\n");
        return;
    }
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping PineAP detection...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        return;
    } else if (argc > 1) {
        printf("Unknown argument: %s\nUsage: pineap [-s]\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown argument: %s\nUsage: pineap [-s]\n", argv[1]);
        return;
    }
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        printf("Warning: Failed to open PCAP file for logging\n");
        TERMINAL_VIEW_ADD_TEXT("Warning: Failed to open PCAP file for logging\n");
    }
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);
    printf("Monitoring for Pineapples\n");
    TERMINAL_VIEW_ADD_TEXT("Monitoring for Pineapples\n");
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
// TP-Link test command
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
            command = (i % 2 == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        } else {
            command = (strcmp(argv[1], "on") == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        }
        uint8_t encrypted_command[128] = {0};
        size_t command_len = strlen(command);
        if (command_len >= sizeof(encrypted_command)) {
            printf("Command too large to encrypt\n");
            TERMINAL_VIEW_ADD_TEXT("Command too large to encrypt\n");
            return;
        }
        // Assume encrypt_tp_link_command is implemented elsewhere
        encrypt_tp_link_command(command, encrypted_command, command_len);
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("Failed to create socket: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT("Failed to create socket\n");
            return;
        }
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Error occurred during sending: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT("Error occurred during sending\n");
            close(sock);
            return;
        }
        printf("Broadcast message sent: %s\n", command);
        TERMINAL_VIEW_ADD_TEXT("Broadcast message sent\n");
        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr, &addr_len);
        if (len < 0) {
            printf("No response from any device\n");
            TERMINAL_VIEW_ADD_TEXT("No response from any device\n");
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            // Assume decrypt_tp_link_response is implemented elsewhere
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            printf("Response: %s\n", decrypted_response);
            TERMINAL_VIEW_ADD_TEXT("Response received\n");
        }
        close(sock);
        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}