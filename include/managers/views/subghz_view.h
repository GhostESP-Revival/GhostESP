#ifndef SUBGHZ_VIEW_H
#define SUBGHZ_VIEW_H

#include "managers/display_manager.h"

extern View subghz_view;

void subghz_view_create(void);
void subghz_view_destroy(void);
void subghz_view_register_stream_handler(void);
void subghz_view_update_remote_state(const char *state);

#endif
