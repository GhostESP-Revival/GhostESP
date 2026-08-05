// cmd_wigle.c
// WiGLE upload and API configuration commands.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/settings_manager.h"
#include "managers/wigle_manager.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
