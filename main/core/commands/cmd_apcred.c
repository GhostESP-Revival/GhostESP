// cmd_apcred.c
// Access Point credentials management command.

#include "core/commands.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "managers/ap_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    if (strlen(new_ssid) > 32) {
        glog("Error: SSID must be 32 characters or less\n");
        status_display_show_status("SSID Too Long");
        return;
    }

    if (strlen(new_password) < 8) {
        glog("Error: Password must be at least 8 characters\n");
        status_display_show_status("Password Weak");
        return;
    }

    if (strlen(new_password) > 63) {
        glog("Error: Password must be 63 characters or less\n");
        status_display_show_status("Password Too Long");
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
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", new_ssid);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", new_password);
    
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
