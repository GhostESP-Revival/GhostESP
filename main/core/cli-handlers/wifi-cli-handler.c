#include "core/cli-handlers/wifi-cli-handler.h"
#include "core/callbacks.h"
#include "core/cli-handlers/ble-cli-handler.h"
#include "managers/ble_manager.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/terminal_screen.h"
#include "vendor/pcap.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

extern FSettings G_Settings;
extern bool g_listen_probes_save_to_sd;
extern int station_count; // If used in scanall
extern void wifi_manager_scanall_chart(void); // If used in scanall

void cmd_wifi_scan_start(int argc, char **argv) {
    if (argc > 2) {
        printf("Usage: scanap [seconds]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: scanap [seconds]\n");
        return;
    }
    if (argc == 2) {
        char *endptr;
        long seconds = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || seconds <= 0) {
            printf("Invalid parameter: '%s'. Usage: scanap [seconds]\n", argv[1]);
            TERMINAL_VIEW_ADD_TEXT("Invalid parameter: '%s'. Usage: scanap [seconds]\n", argv[1]);
            return;
        }
        wifi_manager_start_scan_with_time((int)seconds);
    } else {
        wifi_manager_start_scan();
    }
    wifi_manager_print_scan_results_with_oui();
}

void cmd_wifi_scan_stop(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stopscan\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stopscan\n");
        return;
    }
    wifi_manager_stop_monitor_mode();
    pcap_file_close();
    printf("WiFi scan stopped.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan stopped.\n");
}

void cmd_wifi_scan_results(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: list -a\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: list -a\n");
        return;
    }
    printf("WiFi scan results displaying with OUI matching.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan results displaying with OUI matching.\n");
    wifi_manager_print_scan_results_with_oui();
}

void handle_list(int argc, char **argv) {
    if (argc == 1) {
        printf("Usage: list -a (for Wi-Fi scan results)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: list -a (for Wi-Fi scan results)\n");
        return;
    }
    if (argc > 2) {
        printf("Too many arguments. Usage: list -a | -s | -airtags\n");
        TERMINAL_VIEW_ADD_TEXT("Too many arguments. Usage: list -a | -s | -airtags\n");
        return;
    }
    if (strcmp(argv[1], "-a") == 0) {
        cmd_wifi_scan_results(argc, argv);
    } else if (strcmp(argv[1], "-s") == 0) {
        wifi_manager_list_stations();
        printf("Listed Stations...\n");
        TERMINAL_VIEW_ADD_TEXT("Listed Stations...\n");
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    else if (strcmp(argv[1], "-airtags") == 0) {
        ble_list_airtags();
    }
#endif
    else {
        printf("Unknown flag: %s\nUsage: list -a | -s | -airtags\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown flag: %s\nUsage: list -a | -s | -airtags\n", argv[1]);
    }
}

void handle_beaconspam(int argc, char **argv) {
    if (argc == 1) {
        printf("Usage: beaconspam -r (Random) | -rr (Rickroll) | -l (AP List) | <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconspam -r | -rr | -l | <SSID>\n");
        return;
    }
    if (argc > 2) {
        printf("Too many arguments. Usage: beaconspam -r | -rr | -l | <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Too many arguments. Usage: beaconspam -r | -rr | -l | <SSID>\n");
        return;
    }
    if (strcmp(argv[1], "-r") == 0) {
        printf("Starting Random beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Random beacon spam...\n");
        wifi_manager_start_beacon(NULL);
    } else if (strcmp(argv[1], "-rr") == 0) {
        printf("Starting Rickroll beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Rickroll beacon spam...\n");
        wifi_manager_start_beacon("RICKROLL");
    } else if (strcmp(argv[1], "-l") == 0) {
        printf("Starting AP List beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting AP List beacon spam...\n");
        wifi_manager_start_beacon("APLISTMODE");
    } else if (argv[1][0] == '-') {
        printf("Unknown flag: %s\nUsage: beaconspam -r | -rr | -l | <SSID>\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown flag: %s\nUsage: beaconspam -r | -rr | -l | <SSID>\n", argv[1]);
    } else {
        wifi_manager_start_beacon(argv[1]);
    }
}

void handle_stop_spam(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stopbeaconspam\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stopbeaconspam\n");
        return;
    }
    wifi_manager_stop_beacon();
    printf("Beacon Spam Stopped...\n");
    TERMINAL_VIEW_ADD_TEXT("Beacon Spam Stopped...\n");
}

void handle_sta_scan(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stascan\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stascan\n");
        return;
    }
    wifi_manager_start_station_scan();
}

void handle_attack_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
        return;
    }
    if (strcmp(argv[1], "-d") == 0) {
        printf("Deauthentication starting...\n");
        TERMINAL_VIEW_ADD_TEXT("Deauthentication starting...\n");
        wifi_manager_deauth_station();
    } else if (strcmp(argv[1], "-e") == 0) {
        printf("EAPOL Logoff attack starting...\n");
        TERMINAL_VIEW_ADD_TEXT("EAPOL Logoff attack starting...\n");
        wifi_manager_start_eapollogoff_attack();
    } else if (strcmp(argv[1], "-s") == 0) {
        printf("SAE flood attack starting...\n");
        TERMINAL_VIEW_ADD_TEXT("SAE flood attack starting...\n");
        wifi_manager_start_sae_flood();
    } else {
        printf("Unknown flag: %s\nUsage: attack -d | -e | -s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown flag: %s\nUsage: attack -d | -e | -s\n", argv[1]);
    }
}

void handle_sae_flood_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: saeflood\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: saeflood\n");
        return;
    }
    printf("Starting SAE flood attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting SAE flood attack...\n");
    wifi_manager_start_sae_flood();
}

void handle_stop_sae_flood_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stopsaeflood\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stopsaeflood\n");
        return;
    }
    printf("Stopping SAE flood attack...\n");
    TERMINAL_VIEW_ADD_TEXT("Stopping SAE flood attack...\n");
    wifi_manager_stop_sae_flood();
}

void handle_sae_flood_help_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: saefloodhelp\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: saefloodhelp\n");
        return;
    }
    wifi_manager_sae_flood_help();
}

void handle_stop_deauth(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stopdeauth\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stopdeauth\n");
        return;
    }
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

void handle_ip_lookup(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: scanlocal\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: scanlocal\n");
        return;
    }
    printf("Starting local network scan...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting local network scan...\n");
    wifi_manager_start_ip_lookup();
}

void handle_capture_scan(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: capture [-probe|-beacon|-deauth|-raw|-eapol|-pwn|-wps|-stop|-ble|-skimmer]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: capture [-probe|-beacon|-deauth|-raw|-eapol|-pwn|-wps|-stop|-ble|-skimmer]\n");
        return;
    }

    char *capturetype = argv[1];

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
    } else if (strcmp(capturetype, "-deauth") == 0) {
        int err = pcap_file_open("deauthscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_deauth_scan_callback);
    } else if (strcmp(capturetype, "-beacon") == 0) {
        printf("Starting beacon\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting beacon\npacket capture...\n");
        int err = pcap_file_open("beaconscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_beacon_scan_callback);
    } else if (strcmp(capturetype, "-raw") == 0) {
        printf("Starting raw\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting raw\npacket capture...\n");
        int err = pcap_file_open("rawscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
    } else if (strcmp(capturetype, "-eapol") == 0) {
        printf("Starting EAPOL\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting EAPOL\npacket capture...\n");
        int err = pcap_file_open("eapolscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
    } else if (strcmp(capturetype, "-pwn") == 0) {
        printf("Starting PWN\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting PWN\npacket capture...\n");
        int err = pcap_file_open("pwnscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_pwn_scan_callback);
    } else if (strcmp(capturetype, "-wps") == 0) {
        printf("Starting WPS\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting WPS\npacket capture...\n");
        int err = pcap_file_open("wpsscan", PCAP_CAPTURE_WIFI);

        // should_store_wps is assumed to be declared elsewhere
        extern int should_store_wps;
        should_store_wps = 0;

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_wps_detection_callback);
    } else if (strcmp(capturetype, "-stop") == 0) {
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
    else if (strcmp(capturetype, "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
    } else if (strcmp(capturetype, "-skimmer") == 0) {
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
    else {
        printf("Unknown capture type: %s\n", capturetype);
        TERMINAL_VIEW_ADD_TEXT("Unknown capture type: %s\n", capturetype);
        printf("Usage: capture [-probe|-beacon|-deauth|-raw|-eapol|-pwn|-wps|-stop|-ble|-skimmer]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: capture [-probe|-beacon|-deauth|-raw|-eapol|-pwn|-wps|-stop|-ble|-skimmer]\n");
    }
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

    // 2. Perform Station Scan
    printf("--- Starting Station Scan (%d seconds) ---\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("--- Starting STA Scan (%ds) ---\n", sta_scan_seconds);
    station_count = 0; // Reset station list before new scan
    wifi_manager_start_station_scan(); // Starts monitor mode + channel hopping
    printf("Station scan running for %d seconds...\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("Station scan running for %ds...\n", sta_scan_seconds);
    vTaskDelay(pdMS_TO_TICKS(sta_scan_seconds * 1000));
    wifi_manager_stop_monitor_mode(); // Stops monitor mode + channel hopping

    printf("--- Scan Complete ---\n");
    TERMINAL_VIEW_ADD_TEXT("--- Scan Complete ---\n");

    // 3. Print Combined Results
    wifi_manager_scanall_chart();

    // Ensure AP mode is restored if it was stopped
    ap_manager_start_services(); // Restore AP for WebUI
}

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
    if (argc > 1) {
        printf("Usage: beaconclear\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconclear\n");
        return;
    }
    wifi_manager_clear_beacon_list();
}

void handle_beaconshow(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: beaconshow\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconshow\n");
        return;
    }
    wifi_manager_show_beacon_list();
}

void handle_beaconspamlist(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: beaconspamlist\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconspamlist\n");
        return;
    }
    wifi_manager_start_beacon_list();
}

void handle_dhcpstarve_cmd(int argc, char **argv) {
    if (argc < 2) {
        wifi_manager_dhcpstarve_help();
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        int thr = 1;
        if (argc == 3) {
            char *endptr;
            thr = (int)strtol(argv[2], &endptr, 10);
            if (*endptr != '\0' || thr < 1) {
                printf("Invalid thread count: %s\n", argv[2]);
                TERMINAL_VIEW_ADD_TEXT("Invalid thread count: %s\n", argv[2]);
                wifi_manager_dhcpstarve_help();
                return;
            }
        } else if (argc > 3) {
            wifi_manager_dhcpstarve_help();
            return;
        }
        wifi_manager_start_dhcpstarve(thr);
    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc > 2) {
            wifi_manager_dhcpstarve_help();
            return;
        }
        wifi_manager_stop_dhcpstarve();
    } else if (strcmp(argv[1], "display") == 0) {
        if (argc > 2) {
            wifi_manager_dhcpstarve_help();
            return;
        }
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

void handle_wifi_disconnect(int argc, char **argv)
{
    if (argc > 1) {
        printf("Usage: disconnect\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: disconnect\n");
        return;
    }
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

