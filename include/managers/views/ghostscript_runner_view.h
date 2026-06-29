#pragma once

#include "managers/display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void ghostscript_runner_set_script(const char *path);
extern View ghostscript_runner_view;

#ifdef __cplusplus
}
#endif
