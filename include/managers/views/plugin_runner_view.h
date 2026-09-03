#ifndef PLUGIN_RUNNER_VIEW_H
#define PLUGIN_RUNNER_VIEW_H

#include "managers/display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void plugin_runner_set_app(const char *app_id);
void plugin_runner_request_exit(void);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
bool plugin_runner_request_home_exit(void);
bool plugin_runner_home_exit_pending(void);
#endif
void plugin_runner_view_create(void);
void plugin_runner_view_destroy(void);
void plugin_runner_stop_tick(void);
void plugin_runner_preserve_for_keyboard_input(void);
void plugin_runner_get_callback(void **callback);

extern View plugin_runner_view;

#ifdef __cplusplus
}
#endif

#endif
