#include "managers/views/splash_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/setup_wizard_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/settings_manager.h"
#include "core/ghostesp_version.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include <stdio.h>

lv_obj_t *splash_screen;
lv_obj_t *img;

static void fade_anim_cb(void *var, int32_t opacity);
static void fade_out_cb(void *var);

#define SPLASH_HOLD_MS 900

void splash_create(void) {

  display_manager_fill_screen(lv_color_black());

  splash_screen = gui_screen_create_root(NULL, NULL, lv_color_black(), LV_OPA_COVER);
  splash_view.root = splash_screen;

  img = lv_img_create(splash_screen);

  if (LV_VER_RES < 140 || LV_HOR_RES > 300) { // small screen gets small ghostie
    lv_img_set_src(img, &ghost); // using ghost sprite as placeholder till logo gets scaled
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_zoom(img, 384); //256 is 1x zoom - 384 is 1.5x
  }
  else {
    lv_img_set_src(img, &Ghost_ESP);
  }
  
  lv_obj_align(img, LV_ALIGN_CENTER, 0, -20);


  lv_obj_t *label1 = lv_label_create(splash_screen);
  lv_label_set_text(label1, "GhostESP: Revival");
  lv_obj_set_style_text_color(label1, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align_to(label1, img, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  
  lv_obj_t *label2 = lv_label_create(splash_screen);
  lv_label_set_text(label2, GHOSTESP_VERSION);
  lv_obj_set_style_text_color(label2, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align_to(label2, label1, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  
  lv_obj_t *label3 = lv_label_create(splash_screen);
  // Special naming for somethingsomething template
  const char *build_name = CONFIG_BUILD_CONFIG_TEMPLATE;
  if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
    build_name = "The Banshee";
  }
  lv_label_set_text_fmt(label3, "%s", build_name);
  lv_obj_set_style_text_color(label3, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align_to(label3, label2, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

  lv_anim_t fade_anim;
  lv_anim_init(&fade_anim);

  lv_anim_set_var(&fade_anim, img);
  lv_anim_set_values(&fade_anim, LV_OPA_100, LV_OPA_100);
  lv_anim_set_time(&fade_anim, SPLASH_HOLD_MS);
  lv_anim_set_exec_cb(&fade_anim, fade_anim_cb);
  lv_anim_set_ready_cb(&fade_anim, fade_out_cb);
  lv_anim_start(&fade_anim);
  

}

static void fade_anim_cb(void *var, int32_t opacity) {
  lv_obj_set_style_img_opa((lv_obj_t *)var, opacity, LV_PART_MAIN);
}

static void fade_out_cb(void *var) {
  if (!settings_get_setup_complete(&G_Settings)) {
    display_manager_switch_view(&setup_wizard_view);
  } else {
    display_manager_switch_view(&main_menu_view);
  }
}

void splash_destroy(void) {
  lvgl_obj_del_safe(&splash_screen);
}

View splash_view = {.root = NULL,
                    .create = splash_create,
                    .destroy = splash_destroy,
                    .input_callback = NULL,
                    .name = "Splash Screen"};
