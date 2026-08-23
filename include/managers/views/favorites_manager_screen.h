#ifndef FAVORITES_MANAGER_SCREEN_H
#define FAVORITES_MANAGER_SCREEN_H

#include "managers/display_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern View favorites_manager_view;

void favorites_manager_create(void);
void favorites_manager_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
