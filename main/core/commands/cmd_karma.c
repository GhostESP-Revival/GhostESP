// cmd_karma.c
// Karma (evil twin / SSID lure) attack commands.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
