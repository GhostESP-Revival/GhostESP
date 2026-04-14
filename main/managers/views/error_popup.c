#include "managers/views/error_popup.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "gui/lvgl_safe.h"
#include "lvgl.h"

static const char *TAG = "error_popup";

static lv_obj_t *error_popup_root = NULL;
static lv_obj_t *error_popup_label = NULL;
static lv_timer_t *error_popup_timer = NULL;
static SemaphoreHandle_t popup_mutex = NULL;

#define DISPLAY_DURATION_MS 2000
#define ANIMATION_TIME_MS 150

static void fade_anim_cb(void *obj, int32_t value) {
    lv_obj_set_style_opa(obj, value, 0);
}

static void error_popup_auto_destroy_cb(lv_timer_t *timer) {
    error_popup_timer = NULL;
    if (!error_popup_root) return;

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, error_popup_root);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_0);
    lv_anim_set_time(&fade_out, ANIMATION_TIME_MS);
    lv_anim_set_exec_cb(&fade_out, fade_anim_cb);
    lv_anim_set_ready_cb(&fade_out, (lv_anim_ready_cb_t)lv_obj_del);
    lv_anim_start(&fade_out);

    error_popup_label = NULL;
    error_popup_root = NULL;
}

void error_popup_destroy(void) {
    if (error_popup_root == NULL) {
        return;
    }
    
    if (xSemaphoreTake(popup_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (error_popup_timer) {
            lv_timer_del(error_popup_timer);
            error_popup_timer = NULL;
        }
        if (error_popup_root) {
            lvgl_obj_del_safe(&error_popup_root);
            error_popup_label = NULL;
        }
        xSemaphoreGive(popup_mutex);
    }
}

void error_popup_create(const char *message) {
    if (!message) return;

    ESP_LOGE(TAG, "Error popup called with message: %s", message);

    // Initialize mutex if not already done
    if (!popup_mutex) {
        popup_mutex = xSemaphoreCreateMutex();
        if (!popup_mutex) {
            return;
        }
    }

    // If a popup already exists, destroy it first
    if (error_popup_root != NULL) {
        error_popup_destroy();
    }

    if (xSemaphoreTake(popup_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // Create popup as an overlay
    error_popup_root = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(error_popup_root, LV_OBJ_FLAG_SCROLLABLE);

    int popup_width = LV_HOR_RES * 0.8;
    int padding = (LV_HOR_RES <= 128) ? 5 : 10;
    const lv_font_t *font = (LV_HOR_RES <= 128) ? &lv_font_montserrat_8 : &lv_font_montserrat_12;
    lv_obj_set_style_pad_all(error_popup_root, padding, 0);
    lv_obj_set_width(error_popup_root, popup_width);
    // container height and alignment will be set after label size is calculated

    // Improved styling
    lv_obj_set_style_bg_color(error_popup_root, lv_color_hex(0x323232), 0);  // Darker gray
    lv_obj_set_style_radius(error_popup_root, 8, 0);                          // Softer corners
    lv_obj_set_style_border_width(error_popup_root, 1, 0);
    lv_obj_set_style_border_color(error_popup_root, lv_color_hex(0x555555), 0);
    lv_obj_set_style_shadow_color(error_popup_root, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(error_popup_root, 5, 0);
    lv_obj_set_style_shadow_opa(error_popup_root, LV_OPA_60, 0);
    lv_obj_set_style_opa(error_popup_root, LV_OPA_0, 0);  // Start transparent

    // Create label with better alignment
    error_popup_label = lv_label_create(error_popup_root);
    lv_label_set_long_mode(error_popup_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(error_popup_label, popup_width - 2 * padding);
    lv_obj_set_style_text_color(error_popup_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(error_popup_label, font, 0);
    lv_obj_set_style_text_align(error_popup_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(error_popup_label, message);
    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, message, font,
                    lv_obj_get_style_text_letter_space(error_popup_label, 0),
                    lv_obj_get_style_text_line_space(error_popup_label, 0),
                    popup_width - 2 * padding,
                    LV_TEXT_FLAG_NONE);
    lv_obj_set_size(error_popup_label, popup_width - 2 * padding, txt_size.y);
    lv_obj_set_size(error_popup_root, popup_width, txt_size.y + 2 * padding);
    lv_obj_align(error_popup_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(error_popup_label, LV_ALIGN_CENTER, 0, 0);

    // Fade in animation
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, error_popup_root);
    lv_anim_set_values(&fade_in, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIMATION_TIME_MS);
    lv_anim_set_exec_cb(&fade_in, fade_anim_cb);
    lv_anim_start(&fade_in);

    if (error_popup_timer) {
        lv_timer_del(error_popup_timer);
    }
    error_popup_timer = lv_timer_create(error_popup_auto_destroy_cb, DISPLAY_DURATION_MS, NULL);
    lv_timer_set_repeat_count(error_popup_timer, 1);

    xSemaphoreGive(popup_mutex);
}

bool is_error_popup_rendered(void) {
    return error_popup_root != NULL;
}

// Special version that doesn't auto-destroy (for shutdown warnings)
void error_popup_create_persistent(const char *message) {
    ESP_LOGE(TAG, "Persistent error popup called with message: %s", message);

    // Initialize mutex if not already done
    if (!popup_mutex) {
        popup_mutex = xSemaphoreCreateMutex();
        if (!popup_mutex) {
            return;
        }
    }

    // If a popup already exists, destroy it first
    if (error_popup_root != NULL) {
        error_popup_destroy();
    }

    if (xSemaphoreTake(popup_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // Create popup as an overlay
    error_popup_root = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(error_popup_root, LV_OBJ_FLAG_SCROLLABLE);

    int popup_width = LV_HOR_RES * 0.8;
    int padding = (LV_HOR_RES <= 128) ? 5 : 10;
    const lv_font_t *font = (LV_HOR_RES <= 128) ? &lv_font_montserrat_8 : &lv_font_montserrat_12;
    lv_obj_set_style_pad_all(error_popup_root, padding, 0);
    lv_obj_set_width(error_popup_root, popup_width);

    // Improved styling with red background for warning
    lv_obj_set_style_bg_color(error_popup_root, lv_color_hex(0xD32F2F), 0);  // Red background for warning
    lv_obj_set_style_radius(error_popup_root, 8, 0);                          // Softer corners
    lv_obj_set_style_border_width(error_popup_root, 1, 0);
    lv_obj_set_style_border_color(error_popup_root, lv_color_hex(0xFF5555), 0);
    lv_obj_set_style_shadow_color(error_popup_root, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(error_popup_root, 5, 0);
    lv_obj_set_style_shadow_opa(error_popup_root, LV_OPA_60, 0);
    lv_obj_set_style_opa(error_popup_root, LV_OPA_0, 0);  // Start transparent

    // Create label with better alignment
    error_popup_label = lv_label_create(error_popup_root);
    lv_label_set_long_mode(error_popup_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(error_popup_label, popup_width - 2 * padding);
    lv_obj_set_style_text_color(error_popup_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(error_popup_label, font, 0);
    lv_obj_set_style_text_align(error_popup_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(error_popup_label, message);
    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, message, font,
                    lv_obj_get_style_text_letter_space(error_popup_label, 0),
                    lv_obj_get_style_text_line_space(error_popup_label, 0),
                    popup_width - 2 * padding,
                    LV_TEXT_FLAG_NONE);
    lv_obj_set_size(error_popup_label, popup_width - 2 * padding, txt_size.y);
    lv_obj_set_size(error_popup_root, popup_width, txt_size.y + 2 * padding);
    lv_obj_align(error_popup_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(error_popup_label, LV_ALIGN_CENTER, 0, 0);

    // Fade in animation
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, error_popup_root);
    lv_anim_set_values(&fade_in, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIMATION_TIME_MS);
    lv_anim_set_exec_cb(&fade_in, fade_anim_cb);
    lv_anim_start(&fade_in);

    // No auto-destroy task is created for persistent popups

    xSemaphoreGive(popup_mutex);
}
