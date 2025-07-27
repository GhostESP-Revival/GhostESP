#include "core/cli-handlers/ble-cli-handler.h"
#include "core/callbacks.h"
#include "core/cli-handlers/wifi-cli-handler.h"
#include "managers/ble_manager.h"
#include "managers/wifi_manager.h"
#include "managers/gps_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/sd_card_manager.h"
#include <stdio.h>
#include <string.h>

extern TaskHandle_t gps_info_task_handle;
extern GPSManager g_gpsManager;
int buffer_offset;

void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc == 1) {
        printf("Usage: blescan [-f|-ds|-a|-r|-s]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: blescan [-f|-ds|-a|-r|-s]\n");
        return;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-f") == 0) {
            printf("Starting Find the Flippers.\n");
            TERMINAL_VIEW_ADD_TEXT("Starting Find the Flippers.\n");
            ble_start_find_flippers();
            return;
        }
        if (strcmp(argv[1], "-ds") == 0) {
            printf("Starting BLE Spam Detector.\n");
            TERMINAL_VIEW_ADD_TEXT("Starting BLE Spam Detector.\n");
            ble_start_blespam_detector();
            return;
        }
        if (strcmp(argv[1], "-a") == 0) {
            printf("Starting AirTag Scanner.\n");
            TERMINAL_VIEW_ADD_TEXT("Starting AirTag Scanner.\n");
            ble_start_airtag_scanner();
            return;
        }
        if (strcmp(argv[1], "-r") == 0) {
            printf("Scanning for Raw Packets\n");
            TERMINAL_VIEW_ADD_TEXT("Scanning for Raw Packets\n");
            ble_start_raw_ble_packetscan();
            return;
        }
        if (strcmp(argv[1], "-s") == 0) {
            printf("Stopping BLE Scan.\n");
            TERMINAL_VIEW_ADD_TEXT("Stopping BLE Scan.\n");
            ble_stop();
            return;
        }
        printf("Unknown flag: %s\nUsage: blescan [-f|-ds|-a|-r|-s]\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown flag: %s\nUsage: blescan [-f|-ds|-a|-r|-s]\n", argv[1]);
        return;
    }
    printf("Usage: blescan [-f|-ds|-a|-r|-s]\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: blescan [-f|-ds|-a|-r|-s]\n");
}

void handle_ble_spam_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
        return;
    }
    if (strcmp(argv[1], "-apple") == 0) {
        printf("Starting Apple BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Apple BLE spam...\n");
        ble_start_ble_spam(BLE_SPAM_APPLE);
        return;
    }
    if (strcmp(argv[1], "-ms") == 0 || strcmp(argv[1], "-microsoft") == 0) {
        printf("Starting Microsoft BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Microsoft BLE spam...\n");
        ble_start_ble_spam(BLE_SPAM_MICROSOFT);
        return;
    }
    if (strcmp(argv[1], "-samsung") == 0) {
        printf("Starting Samsung BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Samsung BLE spam...\n");
        ble_start_ble_spam(BLE_SPAM_SAMSUNG);
        return;
    }
    if (strcmp(argv[1], "-google") == 0) {
        printf("Starting Google BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Google BLE spam...\n");
        ble_start_ble_spam(BLE_SPAM_GOOGLE);
        return;
    }
    if (strcmp(argv[1], "-random") == 0) {
        printf("Starting Random BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Random BLE spam...\n");
        ble_start_ble_spam(BLE_SPAM_RANDOM);
        return;
    }
    if (strcmp(argv[1], "-s") == 0) {
        printf("Stopping BLE spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping BLE spam...\n");
        ble_stop_ble_spam();
        return;
    }
    printf("Unknown flag: %s\nUsage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n", argv[1]);
    TERMINAL_VIEW_ADD_TEXT("Unknown flag: %s\nUsage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n", argv[1]);
}

void handle_list_airtags_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: listairtags\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: listairtags\n");
        return;
    }
    ble_list_airtags();
}

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

void handle_spoof_airtag(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: spoofairtag\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: spoofairtag\n");
        return;
    }
    ble_start_spoofing_selected_airtag();
}

void handle_stop_spoof(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stopspoof\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stopspoof\n");
        return;
    }
    ble_stop_spoofing();
}

void handle_ble_wardriving(int argc, char **argv) {
    bool stop_flag = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }
    if (stop_flag) {
        if (argc > 2) {
            printf("Usage: blewardriving -s\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: blewardriving -s\n");
            return;
        }
        ble_stop();
        gps_manager_deinit(&g_gpsManager);
        if (buffer_offset > 0) {
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
    } else {
        if (argc > 1) {
            printf("Usage: blewardriving\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: blewardriving\n");
            return;
        }
        if (!g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        }
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

// Flipper support (if you have these functions)
void handle_list_flippers_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: listflippers\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: listflippers\n");
        return;
    }
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

void handle_stop_flipper(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: stop\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: stop\n");
        return;
    }
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
