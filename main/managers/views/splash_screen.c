#include "managers/views/splash_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/setup_wizard_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/lockscreen.h"
#include "managers/settings_manager.h"
#include "core/ghostesp_version.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include <stdio.h>
#include <string.h>

extern const lv_img_dsc_t ghostesplogo;

#define SPLASH_MIN_HOLD_MS_REDUCED  50
#define SPLASH_MIN_HOLD_MS_NORMAL   1000
#define SPLASH_TIMEOUT_MS           8000
#define SPLASH_PROGRESS_BAR_HEIGHT  8
#define SPLASH_LABEL_MAX_LEN        32
#define SPLASH_PROGRESS_BAR_INDETERMINATE_PCT (-1.0f)
#define SPLASH_INDETERMINATE_PERIOD_MS 1200
#define SPLASH_PROGRESS_BAR_WIDTH_PCT  60
#define SPLASH_PROGRESS_BAR_MIN_WIDTH  80

static lv_coord_t splash_progress_bar_width(void) {
    lv_coord_t w = (lv_coord_t)((lv_disp_get_hor_res(NULL) * SPLASH_PROGRESS_BAR_WIDTH_PCT) / 100);
    if (w < SPLASH_PROGRESS_BAR_MIN_WIDTH) w = SPLASH_PROGRESS_BAR_MIN_WIDTH;
    return w;
}

lv_obj_t *splash_screen;
lv_obj_t *img;

static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_progress_fill = NULL;
static lv_coord_t s_progress_bar_width = 0;
static lv_timer_t *s_fade_timer = NULL;
static lv_anim_t s_progress_indet_anim;
static bool s_progress_indet_running = false;
static uint32_t s_splash_start_ms = 0;
static bool s_splash_done = false;

typedef struct {
    float pct;
    char label[SPLASH_LABEL_MAX_LEN];
} splash_progress_msg_t;

static void fade_out_cb(void *var);
static void splash_hold_expired_cb(lv_timer_t *t);
static void splash_progress_apply(void *arg);
static void splash_completion_apply(void *arg);
static void splash_progress_indet_anim_cb(void *var, int32_t v);
static void start_indeterminate_anim(void);
static void stop_indeterminate_anim(void);
static uint32_t min_hold_ms(void);
static uint32_t elapsed_ms(void);
static void schedule_fade_check(void);

static uint32_t min_hold_ms(void) {
    return settings_get_reduced_motion(&G_Settings) ? SPLASH_MIN_HOLD_MS_REDUCED : SPLASH_MIN_HOLD_MS_NORMAL;
}

static uint32_t elapsed_ms(void) {
    return lv_tick_elaps(s_splash_start_ms);
}

void splash_create(void) {

  display_manager_fill_screen(lv_color_black());

  splash_screen = gui_screen_create_root_no_bg(NULL, NULL, lv_color_black(), LV_OPA_COVER);
  splash_view.root = splash_screen;

  img = lv_img_create(splash_screen);

  if (LV_VER_RES < 140 || LV_HOR_RES > 300) { // small screen gets small ghostie
    lv_img_set_src(img, &ghost); // using ghost sprite as placeholder till logo gets scaled
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_zoom(img, 384); //256 is 1x zoom - 384 is 1.5x
  }
  else {
    lv_img_set_src(img, &ghostesplogo);
  }

  lv_obj_align(img, LV_ALIGN_CENTER, 0, -20);


  lv_obj_t *label1 = lv_label_create(splash_screen);
  lv_label_set_text(label1, GHOSTESP_VERSION);
  lv_obj_set_style_text_color(label1, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align_to(label1, img, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  lv_obj_t *label2 = lv_label_create(splash_screen);
  const char *build_name = CONFIG_BUILD_CONFIG_TEMPLATE;
  if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
    build_name = "The Banshee";
  }
  lv_label_set_text_fmt(label2, "%s", build_name);
  lv_obj_set_style_text_color(label2, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align_to(label2, label1, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

  s_status_label = lv_label_create(splash_screen);
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xCCCCCC), 0);
  lv_label_set_text(s_status_label, "Initializing...");
  lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -28);

  s_progress_bar_width = splash_progress_bar_width();
  s_progress_bar = lv_obj_create(splash_screen);
  lv_obj_set_size(s_progress_bar, s_progress_bar_width, SPLASH_PROGRESS_BAR_HEIGHT);
  lv_obj_clear_flag(s_progress_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_progress_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_progress_bar, 0, LV_PART_MAIN);
  lv_obj_align(s_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -16);

  s_progress_fill = lv_obj_create(s_progress_bar);
  lv_obj_set_size(s_progress_fill, 0, SPLASH_PROGRESS_BAR_HEIGHT);
  lv_obj_set_pos(s_progress_fill, 0, 0);
  lv_obj_clear_flag(s_progress_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_progress_fill, lv_color_hex(0x00AAFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_progress_fill, 4, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_progress_fill, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_progress_fill, 0, LV_PART_MAIN);

  s_splash_start_ms = lv_tick_get();
  s_splash_done = false;
  s_progress_indet_running = false;
  schedule_fade_check();
}

static void splash_progress_indet_anim_cb(void *var, int32_t v) {
    (void)var;
    if (!s_progress_fill) return;
    int32_t bar_w = (int32_t)s_progress_bar_width;
    int32_t pill_w = bar_w / 3;
    if (pill_w < 8) pill_w = 8;
    int32_t max_x = bar_w - pill_w;
    if (max_x < 0) max_x = 0;
    int32_t x = (v * max_x) / 100;
    lv_obj_set_size(s_progress_fill, (lv_coord_t)pill_w, SPLASH_PROGRESS_BAR_HEIGHT);
    lv_obj_set_x(s_progress_fill, (lv_coord_t)x);
}

static void start_indeterminate_anim(void) {
    if (!s_progress_fill || s_progress_indet_running) return;
    if (settings_get_reduced_motion(&G_Settings)) return;
    s_progress_indet_running = true;
    lv_anim_init(&s_progress_indet_anim);
    lv_anim_set_var(&s_progress_indet_anim, s_progress_fill);
    lv_anim_set_values(&s_progress_indet_anim, 0, 100);
    lv_anim_set_time(&s_progress_indet_anim, SPLASH_INDETERMINATE_PERIOD_MS);
    lv_anim_set_playback_time(&s_progress_indet_anim, 0);
    lv_anim_set_repeat_count(&s_progress_indet_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&s_progress_indet_anim, splash_progress_indet_anim_cb);
    lv_anim_start(&s_progress_indet_anim);
}

static void stop_indeterminate_anim(void) {
    if (!s_progress_indet_running) return;
    lv_anim_del(s_progress_fill, splash_progress_indet_anim_cb);
    s_progress_indet_running = false;
}

static void splash_hold_expired_cb(lv_timer_t *t) {
    (void)t;
    s_fade_timer = NULL;
    fade_out_cb(NULL);
}

static void fade_out_cb(void *var) {
  (void)var;
  stop_indeterminate_anim();
  if (s_fade_timer) {
      lv_timer_del(s_fade_timer);
      s_fade_timer = NULL;
  }
  if (!settings_get_setup_complete(&G_Settings)) {
    display_manager_switch_view(&setup_wizard_view);
  } else if (settings_get_lockscreen_enabled(&G_Settings)) {
    lockscreen_reset_input();
    display_manager_switch_view(&lockscreen_view);
  } else {
    display_manager_switch_view(&main_menu_view);
  }
}

static void schedule_fade_check(void) {
    if (s_fade_timer) {
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
    }
    uint32_t elapsed = elapsed_ms();
    uint32_t min_hold = min_hold_ms();
    uint32_t delay;
    if (s_splash_done) {
        if (elapsed >= min_hold) {
            delay = 0;
        } else {
            delay = min_hold - elapsed;
        }
    } else {
        if (elapsed >= SPLASH_TIMEOUT_MS) {
            delay = 0;
        } else {
            delay = SPLASH_TIMEOUT_MS - elapsed;
        }
    }
    s_fade_timer = lv_timer_create(splash_hold_expired_cb, delay, NULL);
    lv_timer_set_repeat_count(s_fade_timer, 1);
}

void splash_set_progress(float pct, const char *label) {
    splash_progress_msg_t *msg = malloc(sizeof(*msg));
    if (!msg) return;
    if (pct < 0.0f) {
        msg->pct = SPLASH_PROGRESS_BAR_INDETERMINATE_PCT;
    } else {
        if (pct > 100.0f) pct = 100.0f;
        msg->pct = pct;
    }
    if (label) {
        strncpy(msg->label, label, SPLASH_LABEL_MAX_LEN - 1);
        msg->label[SPLASH_LABEL_MAX_LEN - 1] = '\0';
    } else {
        msg->label[0] = '\0';
    }
    display_manager_run_on_lvgl(splash_progress_apply, msg);
}

static void splash_progress_apply(void *arg) {
    splash_progress_msg_t *msg = (splash_progress_msg_t *)arg;
    if (!msg) return;
    if (s_status_label && msg->label[0]) {
        lv_label_set_text(s_status_label, msg->label);
    }
    if (s_progress_fill && s_progress_bar) {
        if (msg->pct < 0.0f) {
            if (!s_progress_indet_running) start_indeterminate_anim();
        } else {
            if (s_progress_indet_running) stop_indeterminate_anim();
            int32_t w = (int32_t)(((float)s_progress_bar_width * msg->pct) / 100.0f + 0.5f);
            if (w < 0) w = 0;
            if (w > s_progress_bar_width) w = s_progress_bar_width;
            lv_obj_set_size(s_progress_fill, (lv_coord_t)w, SPLASH_PROGRESS_BAR_HEIGHT);
            lv_obj_set_x(s_progress_fill, 0);
        }
    }
    free(msg);
}

void splash_signal_completion(void) {
    display_manager_run_on_lvgl(splash_completion_apply, NULL);
}

static void splash_completion_apply(void *arg) {
    (void)arg;
    s_splash_done = true;
    stop_indeterminate_anim();
    if (s_progress_fill && s_progress_bar) {
        lv_obj_set_size(s_progress_fill, s_progress_bar_width, SPLASH_PROGRESS_BAR_HEIGHT);
        lv_obj_set_x(s_progress_fill, 0);
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Ready");
    }
    schedule_fade_check();
}

void splash_destroy(void) {
    stop_indeterminate_anim();
    if (s_fade_timer) {
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
    }
    s_status_label = NULL;
    s_progress_bar = NULL;
    s_progress_fill = NULL;
    lvgl_obj_del_safe(&splash_screen);
}

View splash_view = {.root = NULL,
                    .create = splash_create,
                    .destroy = splash_destroy,
                    .input_callback = NULL,
                    .name = "Splash Screen"};
