#pragma once

#include "managers/display_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ghostscript_runner_set_script(const char *path);
bool ghostscript_runner_stop_script(void);
bool ghostscript_runner_is_script_active(void);
extern View ghostscript_runner_view;

#ifdef __cplusplus
}
#endif
