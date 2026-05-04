#ifndef GUI_ANIM_H
#define GUI_ANIM_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    GUI_ANIM_DIR_LEFT  = -1,
    GUI_ANIM_DIR_RIGHT =  1,
} gui_anim_dir_t;

void gui_anim_slide_in(lv_obj_t *obj, gui_anim_dir_t dir, uint32_t duration);
void gui_anim_slide_out(lv_obj_t *obj, gui_anim_dir_t dir, uint32_t duration, lv_anim_ready_cb_t ready_cb, void *user_data);

typedef struct {
    lv_obj_t **items;
    int count;
    int *revealed;
    lv_coord_t *item_ys;
} gui_anim_wipe_ctx_t;

void gui_anim_list_wipe(lv_obj_t *parent, lv_obj_t *items[], int count, uint32_t duration);

void gui_anim_selection_travel(lv_obj_t *indicator, lv_coord_t from_y, lv_coord_t to_y, lv_coord_t item_h, uint32_t duration);

void gui_anim_press_pulse(lv_obj_t *obj);
void gui_anim_breathe_start(lv_obj_t *obj);
void gui_anim_breathe_stop(lv_obj_t *obj);
void gui_anim_pop_in(lv_obj_t *obj);

int32_t gui_anim_path_spring(const lv_anim_t *a);
int32_t gui_anim_path_decel(const lv_anim_t *a);

#endif
