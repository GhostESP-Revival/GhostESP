#ifndef TDONGLE_STATUS_SCREEN_H
#define TDONGLE_STATUS_SCREEN_H

#include "managers/display_manager.h"
#include <stdbool.h>

extern View tdongle_status_view;

bool tdongle_status_is_ready(void);
void tdongle_status_set_lines(const char *line_one, const char *line_two);
void tdongle_status_show_status(const char *status_line);
void tdongle_status_show_attack(const char *attack_name, const char *target);
void tdongle_status_clear(void);

#endif /* TDONGLE_STATUS_SCREEN_H */
