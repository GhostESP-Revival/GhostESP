#include "core/cli-handlers/system-cli-handler.h"
#include "managers/settings_manager.h"
#include "managers/views/terminal_screen.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

extern FSettings G_Settings;

void handle_reboot(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: reboot\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: reboot\n");
        return;
    }
    printf("Rebooting system...\n");
    TERMINAL_VIEW_ADD_TEXT("Rebooting system...\n");
    esp_restart();
}

void handle_crash(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: crash\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: crash\n");
        return;
    }
    int *ptr = NULL;
    *ptr = 42;
}

void handle_chip_info_cmd(int argc, char **argv) {
    if (argc > 1) {
        printf("Usage: chipinfo\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: chipinfo\n");
        return;
    }
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *model_name = "Unknown";
    switch(chip_info.model) {
        case CHIP_ESP32: model_name = "ESP32"; break;
        case CHIP_ESP32S2: model_name = "ESP32-S2"; break;
        case CHIP_ESP32S3: model_name = "ESP32-S3"; break;
        case CHIP_ESP32C3: model_name = "ESP32-C3"; break;
        case CHIP_ESP32C2: model_name = "ESP32-C2"; break;
        case CHIP_ESP32C6: model_name = "ESP32-C6"; break;
        case CHIP_ESP32H2: model_name = "ESP32-H2"; break;
        case CHIP_ESP32P4: model_name = "ESP32-P4"; break;
        case CHIP_ESP32C5: model_name = "ESP32-C5"; break;
        case CHIP_ESP32C61: model_name = "ESP32-C61"; break;
        default: model_name = "Unknown"; break;
    }

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;

    printf("Chip Information:\n");
    printf("  Model: %s\n", model_name);
    printf("  Revision: v%d.%d\n", major_rev, minor_rev);
    printf("  CPU Cores: %d\n", chip_info.cores);

    printf("  Features: ");
    bool first = true;
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) { printf("WiFi"); first = false; }
    if (chip_info.features & CHIP_FEATURE_BT) { if (!first) printf("/"); printf("BT"); first = false; }
    if (chip_info.features & CHIP_FEATURE_BLE) { if (!first) printf("/"); printf("BLE"); first = false; }
    if (chip_info.features & CHIP_FEATURE_IEEE802154) { if (!first) printf("/"); printf("802.15.4"); first = false; }
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH) { if (!first) printf("/"); printf("Embedded Flash"); first = false; }
    if (chip_info.features & CHIP_FEATURE_EMB_PSRAM) { if (!first) printf("/"); printf("Embedded PSRAM"); first = false; }
    if (first) { printf("None"); }
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