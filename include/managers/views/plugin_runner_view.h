#ifndef PLUGIN_RUNNER_VIEW_H
#define PLUGIN_RUNNER_VIEW_H

#include "managers/display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void plugin_runner_set_app(const char *app_id);
void plugin_runner_view_create(void);
void plugin_runner_view_destroy(void);
void plugin_runner_stop_tick(void);
void plugin_runner_get_callback(void **callback);

extern View plugin_runner_view;

#ifdef __cplusplus
}
#endif

#endif
