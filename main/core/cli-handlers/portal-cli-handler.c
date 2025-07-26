#include "core/cli-handlers/portal-cli-handler.h"
#include "managers/settings_manager.h"
#include "managers/ap_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern FSettings G_Settings;

#define MAX_PORTAL_PATH_LEN 128
#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

void handle_start_portal(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        printf("PSK is optional for an open AP.\n");
        TERMINAL_VIEW_ADD_TEXT("PSK is optional for an open AP.\n");
        return;
    }
    const char *url = argv[1];
    const char *ap_ssid = argv[2];
    const char *psk = (argc == 4) ? argv[3] : "";
    if (strlen(url) >= MAX_PORTAL_PATH_LEN) {
        printf("Error: Provided Path is too long.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Path too long.\n");
        return;
    }
    char final_url_or_path[MAX_PORTAL_PATH_LEN];
    strcpy(final_url_or_path, url);

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

void handle_stop_portal(int argc, char **argv) {
    wifi_manager_stop_evil_portal();
    printf("Stopping evil portal...\n");
    TERMINAL_VIEW_ADD_TEXT("Stopping evil portal...\n");
}

void handle_listportals(int argc, char **argv) {
    char portal_names[MAX_PORTALS][MAX_PORTAL_NAME] = {0};

    int count = get_evil_portal_list(portal_names);
    if (count > MAX_PORTALS) count = MAX_PORTALS;

    if (count <= 0) {
        printf("No portals found.\n");
        TERMINAL_VIEW_ADD_TEXT("No portals found.\n");
        return;
    }

    printf("Available Evil Portals:\n");
    TERMINAL_VIEW_ADD_TEXT("Available Evil Portals:\n");
    for (int i = 0; i < count; ++i) {
        char buf[512];
        snprintf(buf, sizeof(buf), "  %.500s\n", portal_names[i]);
        printf("%s", buf);
        TERMINAL_VIEW_ADD_TEXT("%s", buf);
    }
}