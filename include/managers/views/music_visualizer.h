#ifndef MUSIC_VISUALIZER_H
#define MUSIC_VISUALIZER_H

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#include "managers/display_manager.h"

#define NUM_BARS 15

typedef struct {
    lv_obj_t *track_label;
    lv_obj_t *artist_label;
    lv_obj_t *status_label;
    lv_obj_t *endpoint_label;
    lv_obj_t *bars[NUM_BARS];
    lv_obj_t *peaks[NUM_BARS];
    lv_obj_t *reflections[NUM_BARS];
} MusicVisualizerView;

void music_visualizer_view_create(void);

void music_visualizer_view_update(const uint8_t *amplitudes,
                                  const char *track_name,
                                  const char *artist_name);

void music_visualizer_destroy(void);

extern View music_visualizer_view;

#endif
