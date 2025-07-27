#include "core/cli-handlers/gps-cli-handler.h"
#include "managers/gps_manager.h"
#include "managers/views/terminal_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

extern TaskHandle_t gps_info_task_handle;
extern GPSManager g_gpsManager;

void handle_gps_info(int argc, char **argv) {
    bool stop_flag = false;

    // Validate arguments: only allow 0 or 1 argument, and if present, it must be "-s"
    if (argc > 2) {
        printf("Usage: gpsinfo [-s]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: gpsinfo [-s]\n");
        return;
    }
    if (argc == 2 && strcmp(argv[1], "-s") != 0) {
        printf("Unknown parameter: %s\nUsage: gpsinfo [-s]\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Unknown parameter: %s\nUsage: gpsinfo [-s]\n", argv[1]);
        return;
    }

    if (argc == 2 && strcmp(argv[1], "-s") == 0) {
        stop_flag = true;
    }

    if (stop_flag) {
        if (gps_info_task_handle != NULL) {
            vTaskDelete(gps_info_task_handle);
            gps_info_task_handle = NULL;
            gps_manager_deinit(&g_gpsManager);
            printf("GPS info display stopped.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display stopped.\n");
        } else {
            printf("GPS info display is not running.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display is not running.\n");
        }
    } else {
        if (gps_info_task_handle == NULL) {
            gps_manager_init(&g_gpsManager);

            // Wait a brief moment for GPS initialization
            vTaskDelay(pdMS_TO_TICKS(100));

            // Start the info display task
            xTaskCreate(gps_info_display_task, "gps_info", 4096, NULL, 1, &gps_info_task_handle);
            printf("GPS info started.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info started.\n");
        } else {
            printf("GPS info display is already running.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display is already running.\n");
        }
    }
}