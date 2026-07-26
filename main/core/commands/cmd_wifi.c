// cmd_wifi.c
// WiFi scanning, selection, and basic WiFi attack commands.

#include "core/commands.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ap_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include "vendor/pcap.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    (void)argc;
    (void)argv;

    // Properly stop any ongoing WiFi scan
    wifi_manager_stop_scan();

    // Stop monitor mode
    wifi_manager_stop_monitor_mode();

    // Close pcap file
    pcap_file_close();

    // Reset WiFi to a good state
    esp_err_t stop_err = esp_wifi_stop();
    esp_err_t start_err = esp_wifi_start();

    // Live AP scan and timed scan both call ap_manager_stop_services() on
    // entry, so this function is the only thing that brings the AP back for
    // those flows. Always restore AP services before returning, regardless
    // of whether the Wi-Fi driver restart succeeded.
    ap_manager_start_services();

    if (stop_err != ESP_OK || start_err != ESP_OK) {
        glog("WiFi scan stop completed with recovery errors (stop=%s, start=%s).\n",
             esp_err_to_name(stop_err), esp_err_to_name(start_err));
        status_display_show_status("Scan Stop Warn");
        return;
    }

    glog("WiFi scan stopped.\n");
    status_display_show_status("Scan Stopped");
}

void cmd_wifi_scan_results(int argc, char **argv) {
    (void)argc;
    (void)argv;
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
    (void)argc;
    (void)argv;
    wifi_manager_stop_beacon();
    glog("Beacon Spam Stopped...\n");
    status_display_show_status("Beacon Stopped");
}

void handle_sta_scan(int argc, char **argv) {
    (void)argc;
    (void)argv;
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
        } else if (strcmp(argv[1], "-hsd") == 0) {
            glog("Handshake+Deauth starting...\n");
            wifi_manager_start_handshake_deauth();
            status_display_show_status("HS+Deauth Start");
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
        } else if (strcmp(argv[1], "-g") == 0) {
            if (argc < 4) {
                glog("Usage: attack -g <ssid> <password>\n");
                status_display_show_status("GTK Usage");
                return;
            }
            glog("GTK Abuse test starting...\n");
            wifi_manager_start_gtk_abuse(argv[2], argv[3]);
            status_display_show_status("GTK Start");
            return;
        }
    }
    glog("Usage: attack -d (deauth) | attack -hsd (handshake+deauth) | attack -c (channel switch) | attack -e (EAPOL logoff) | attack -s <password> (SAE flood) | attack -g <ssid> <password> (GTK abuse)\n");
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
    (void)argc;
    (void)argv;
    glog("Stopping SAE flood attack...\n");
    wifi_manager_stop_sae_flood();
    status_display_show_status("SAE Flood Off");
}

void handle_sae_flood_help_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_sae_flood_help();
    status_display_show_status("SAE Help");
}

void handle_stop_deauth(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_stop_deauth();
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_handshake_deauth();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
    wifi_manager_stop_channel_switch_attack();
    wifi_manager_stop_gtk_abuse();
    glog("All WiFi attacks stopped...\n");
    status_display_show_status("Attacks Off");
}

void handle_select_cmd(int argc, char **argv) {
    if (argc != 3) {
        glog("Usage: select -a <number[,number,...]> or select -s <number>\n");
        return;
    }

    if (strcmp(argv[1], "-a") == 0) {
        const char *input = argv[2];
        const char *comma = strchr(input, ',');

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
            char buf[128];
            size_t in_len = strlen(input);
            if (in_len >= sizeof(buf)) {
                glog("Error: index list too long.\n");
                return;
            }
            memcpy(buf, input, in_len + 1);

            char *saveptr = NULL;
            char *token = strtok_r(buf, ",", &saveptr);

            while (token != NULL && count < 32) {
                char *endptr;
                int num = (int)strtol(token, &endptr, 10);
                if (*endptr == '\0') {
                    indices[count++] = num;
                } else {
                    glog("Error: '%s' is not a valid number.\n", token);
                    return;
                }
                token = strtok_r(NULL, ",", &saveptr);
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
    (void)argc;
    (void)argv;
    wifi_manager_clear_beacon_list();
    status_display_show_status("Beacon Clear");
}

void handle_beaconshow(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_show_beacon_list();
    status_display_show_status("Beacon Show");
}

void handle_beaconspamlist(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_start_beacon_list();
    status_display_show_status("Beacon List On");
}

void handle_track_ap_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_track_ap();
}

void handle_track_sta_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_manager_track_sta();
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
        settings_save_sta_credentials(&G_Settings);
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

void handle_wifi_disconnect(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_manager_set_manual_disconnect(true);
        esp_err_t err = esp_wifi_disconnect();
        if (err == ESP_OK) {
            glog("WiFi disconnect command sent successfully\n");
        } else {
            glog("Failed to send disconnect command: %s\n", esp_err_to_name(err));
        }
    } else {
        glog("WiFi is not connected\n");
    }

    // kill any lingering visualizer task started on connect
    if (VisualizerHandle != NULL) {
        wifi_manager_stop_visualizer();
    }
}

void handle_wifi_status(int argc, char **argv) {
    (void)argc;
    (void)argv;
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

void handle_ip_lookup(int argc, char **argv) {
    (void)argc;
    (void)argv;
    glog("Starting IP lookup...\n");
    wifi_manager_start_ip_lookup();
    status_display_show_status("IP Lookup");
}

void handle_wifi_autoreconnect_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("WiFi auto-reconnect: %s\n",
             settings_get_wifi_auto_reconnect(&G_Settings) ? "on" : "off");
        glog("Usage: autoreconnect <on|off>\n");
        return;
    }

    bool enable;
    if (strcmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        glog("Invalid argument. Use 'on' or 'off'\n");
        return;
    }

    settings_set_wifi_auto_reconnect(&G_Settings, enable);
    settings_persist_setting(SETTING_WIFI_AUTO_RECONNECT);

    if (!enable) {
        wifi_manager_stop_reconnect();
    }

    glog("WiFi auto-reconnect %s\n", enable ? "enabled" : "disabled");
}
