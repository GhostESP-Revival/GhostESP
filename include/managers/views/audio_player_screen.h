#ifndef AUDIO_PLAYER_SCREEN_H
#define AUDIO_PLAYER_SCREEN_H

#include "managers/display_manager.h"

extern View audio_player_view;

void audio_player_create(void);
void audio_player_destroy(void);
void audio_player_set_return_view(View *view);

#endif // AUDIO_PLAYER_SCREEN_H
