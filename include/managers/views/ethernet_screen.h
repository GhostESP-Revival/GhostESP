#pragma once

#include "managers/display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern View ethernet_screen_view;

void ethernet_screen_create(void);
void ethernet_screen_destroy(void);
void ethernet_screen_set_return_view(View *view);

#ifdef __cplusplus
}
#endif
