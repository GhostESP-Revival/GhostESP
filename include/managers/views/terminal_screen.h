#ifndef TERMINAL_VIEW_H
#define TERMINAL_VIEW_H

#include "lvgl.h"
#include "managers/display_manager.h"
#include "managers/ap_manager.h"
#include "core/esp_comm_manager.h"

extern View terminal_view;
extern lv_timer_t *terminal_update_timer;

void terminal_view_add_text(const char *text);

void terminal_view_create(void);

void terminal_view_destroy(void);

void terminal_set_return_view(View *view);

void terminal_set_dualcomm_filter(bool enable);

#ifdef CONFIG_WITH_SCREEN
#define TERMINAL_VIEW_ADD_TEXT(fmt, ...)                                           \
    do {                                                                           \
        char buffer[512];                                                          \
        snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__);                      \
        if (esp_comm_manager_is_remote_command()) {                                \
            esp_comm_manager_send_response((const uint8_t*)buffer, strlen(buffer));\
        }                                                                          \
        terminal_view_add_text(buffer);                                            \
        ap_manager_add_log(buffer);                                                \
    } while (0)
#else
#define TERMINAL_VIEW_ADD_TEXT(fmt, ...)                                           \
    do {                                                                           \
        char buffer[512];                                                          \
        snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__);                      \
        if (esp_comm_manager_is_remote_command()) {                                \
            esp_comm_manager_send_response((const uint8_t*)buffer, strlen(buffer)); \
        }                                                                          \
        ap_manager_add_log(buffer);                                                \
    } while (0)
#endif

void terminal_screen_create(lv_obj_t* parent);

#endif // TERMINAL_VIEW_H