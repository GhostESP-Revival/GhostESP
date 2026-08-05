// cmd_gps.c
// GPS commands: pin, baud rate, and info display.

#include "core/commands.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "managers/gps_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "vendor/GPS/gps_logger.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Owned by commandline.c, shared with handle_gps_info and the stop handler.
extern TaskHandle_t gps_info_task_handle;
extern StackType_t *gps_task_stack;
extern StaticTask_t *gps_task_tcb;

void handle_gps_pin(int argc, char **argv) {
    if (argc < 2) {
        uint8_t current_pin = settings_get_gps_rx_pin(&G_Settings);
        if (current_pin > 0) {
            glog("GPS RX pin: IO%d\n", current_pin);
        } else {
            glog("GPS RX pin: not set (using default)\n");
        }
        glog("Usage: gpspin <pin>\n");
        return;
    }

    int pin = atoi(argv[1]);
    if (pin < 0 || pin > 48) {
        glog("Invalid pin. Must be 0-48.\n");
        return;
    }

    settings_set_gps_rx_pin(&G_Settings, (uint8_t)pin);
    settings_save(&G_Settings);
    glog("GPS RX pin set to IO%d. Restart GPS to apply.\n", pin);
    TERMINAL_VIEW_ADD_TEXT("GPS pin set to IO%d\n", pin);
}

void handle_gps_baud(int argc, char **argv) {
    if (argc < 2) {
        uint32_t current_baud = settings_get_gps_baud_rate(&G_Settings);
        if (current_baud == GPS_BAUD_AUTO) {
            glog("GPS baud rate: auto-detect\n");
        } else if (current_baud > 0) {
            glog("GPS baud rate: %lu\n", (unsigned long)current_baud);
        } else {
#ifdef CONFIG_GPS_UART_BAUD_RATE
            glog("GPS baud rate: not set (using default %d)\n", CONFIG_GPS_UART_BAUD_RATE);
#else
            glog("GPS baud rate: not set\n");
#endif
        }
        glog("Usage: gpsbaud <rate|auto>\n");
        glog("Common rates: auto, 9600, 19200, 38400, 57600, 115200 (0 = reset to default)\n");
        return;
    }

    long baud = 0;
    bool auto_baud = (strcmp(argv[1], "auto") == 0 || strcmp(argv[1], "AUTO") == 0 ||
                      strcmp(argv[1], "Auto") == 0);
    if (auto_baud) {
        baud = GPS_BAUD_AUTO;
    } else {
        baud = atol(argv[1]);
    }
    if (baud < 0) {
        glog("Invalid baud rate.\n");
        return;
    }
    if (!auto_baud && baud == GPS_BAUD_AUTO) {
        glog("Invalid baud rate. Use 'gpsbaud auto' for auto-detect.\n");
        return;
    }

    settings_set_gps_baud_rate(&G_Settings, (uint32_t)baud);
    settings_save(&G_Settings);
    if (auto_baud) {
        glog("GPS baud rate set to auto-detect. Restart GPS to apply.\n");
    } else if (baud == 0) {
        glog("GPS baud rate reset to default. Restart GPS to apply.\n");
    } else {
        glog("GPS baud rate set to %ld. Restart GPS to apply.\n", baud);
    }
    TERMINAL_VIEW_ADD_TEXT("GPS baud: %ld\n", baud);
}

void handle_gps_info(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        if (gps_info_task_handle != NULL) {
            vTaskDelete(gps_info_task_handle);
            gps_info_task_handle = NULL;

            // Free the manually allocated stack and TCB
            if (gps_task_stack) {
                heap_caps_free(gps_task_stack);
                gps_task_stack = NULL;
            }
            if (gps_task_tcb) {
                heap_caps_free(gps_task_tcb);
                gps_task_tcb = NULL;
            }

            gps_manager_deinit(&g_gpsManager);
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_clear_peer_fix();
            printf("GPS info display stopped.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display stopped.\n");
            status_display_show_status("GPS Info Off");
        }
    } else {
        if (gps_info_task_handle == NULL) {
            bool peer_connected = esp_comm_manager_is_connected();
            gps_manager_set_peer_gps_preferred(peer_connected);
            if (!peer_connected) {
                gps_manager_clear_peer_fix();
            }
            if (!peer_connected) {
                gps_manager_init(&g_gpsManager);
            } else if (g_gpsManager.isinitilized) {
                gps_manager_deinit(&g_gpsManager);
            }

            // Wait a moment for GPS initialization
            vTaskDelay(pdMS_TO_TICKS(100));

            // Start info display task with PSRAM preference
            gps_info_task_handle = NULL;

            // Allocate stack in PSRAM if available, fallback to internal RAM
            const size_t stack_bytes_target = 8192;
            const size_t stack_words = (stack_bytes_target + sizeof(StackType_t) - 1) / sizeof(StackType_t);
            const size_t stack_size = stack_words * sizeof(StackType_t);
            gps_task_stack = NULL;

#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
            gps_task_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
            if (!gps_task_stack) {
                gps_task_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }

            if (!gps_task_stack) {
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to allocate stack.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to allocate stack.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }

            gps_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!gps_task_tcb) {
                heap_caps_free(gps_task_stack);
                gps_task_stack = NULL;
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to allocate TCB.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to allocate TCB.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }

            TaskHandle_t created_task = xTaskCreateStatic(gps_info_display_task, "gps_info", stack_words, NULL, 1, gps_task_stack, gps_task_tcb);
            if (created_task == NULL) {
                heap_caps_free(gps_task_stack);
                heap_caps_free(gps_task_tcb);
                gps_task_stack = NULL;
                gps_task_tcb = NULL;
                gps_manager_deinit(&g_gpsManager);
                printf("GPS info failed to start.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS info failed to start.\n");
                status_display_show_status("GPS Info Fail");
                return;
            }
            gps_info_task_handle = created_task;
            printf("GPS info started.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info started.\n");
            if (peer_connected) {
                printf("GPS source: peer stream preferred.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS source: peer stream preferred.\n");
            } else {
                printf("GPS source: local parser.\n");
                TERMINAL_VIEW_ADD_TEXT("GPS source: local parser.\n");
            }
            status_display_show_status("GPS Info On");
        }
    }
}
