#ifndef SD_BROWSER_SCREEN_H
#define SD_BROWSER_SCREEN_H

#include "managers/display_manager.h"

void sd_browser_create(void);
void sd_browser_destroy(void);
void sd_browser_get_callback(void **callback);

extern View sd_browser_view;

#endif /* SD_BROWSER_SCREEN_H */
