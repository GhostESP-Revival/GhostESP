#ifndef STATUS_DISPLAY_MANAGER_H
#define STATUS_DISPLAY_MANAGER_H

#include <stdbool.h>

void status_display_init(void);
bool status_display_is_ready(void);
void status_display_set_lines(const char *line_one, const char *line_two);
// Like status_display_set_lines, but "holds" the panel: the idle animation /
// LoRa HUD will not paint over it, and later transient status updates are
// ignored, until status_display_clear() (or a new held screen) replaces it.
void status_display_set_lines_hold(const char *line_one, const char *line_two);
void status_display_show_attack(const char *attack_name, const char *target);
void status_display_show_status(const char *status_line);
void status_display_clear(void);
void status_display_deinit(void);

#endif // STATUS_DISPLAY_MANAGER_H


