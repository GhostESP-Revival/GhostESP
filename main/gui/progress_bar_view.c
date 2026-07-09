#include "gui/progress_bar_view.h"

#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);

struct progress_bar_view_t {
    lv_obj_t *container;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *percent;
    lv_obj_t *subtext;
    bool active;
};

static const lv_font_t *progress_title_font(void) {
    return LV_VER_RES <= 135 ? accessibility_get_font_body() : accessibility_get_font_title();
}

static const lv_font_t *progress_body_font(void) {
    return LV_VER_RES <= 135 ? accessibility_get_font_small() : accessibility_get_font_body();
}

progress_bar_view_t *progress_bar_view_create(const char *title) {
    progress_bar_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    view->container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(view->container, LV_PCT(100), LV_VER_RES - GUI_STATUS_BAR_H);
    lv_obj_set_pos(view->container, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(view->container, bg, 0);
    lv_obj_set_style_bg_opa(view->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->container, 0, 0);
    lv_obj_set_style_pad_all(view->container, 0, 0);
    lv_obj_clear_flag(view->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->container, LV_OBJ_FLAG_CLICKABLE);

    display_manager_add_status_bar("Progress");

    // Fill the screen edge-to-edge like the shared confirm/NFC popups instead
    // of a floating centered card.
    int card_w = LV_HOR_RES;
    int card_h = LV_VER_RES - GUI_STATUS_BAR_H;

    view->card = lv_obj_create(view->container);
    lv_obj_set_size(view->card, card_w, card_h);
    lv_obj_align(view->card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(view->card, surface, 0);
    lv_obj_set_style_bg_opa(view->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->card, 0, 0);
    lv_obj_set_style_radius(view->card, 0, 0);
    lv_obj_set_style_pad_all(view->card, GUI_GRID * 3, 0);
    lv_obj_set_style_pad_row(view->card, GUI_GRID * 2, 0);
    lv_obj_set_flex_flow(view->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->card, LV_OBJ_FLAG_SCROLLABLE);

    view->title = lv_label_create(view->card);
    lv_obj_set_width(view->title, card_w - GUI_GRID * 6);
    lv_obj_set_style_text_font(view->title, progress_title_font(), 0);
    lv_obj_set_style_text_color(view->title, text, 0);
    lv_obj_set_style_text_align(view->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view->title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(view->title, title ? title : "Working");

    view->track = lv_obj_create(view->card);
    lv_obj_set_size(view->track, card_w - GUI_GRID * 8, GUI_GRID * 2);
    lv_obj_set_style_bg_color(view->track, bg, 0);
    lv_obj_set_style_bg_opa(view->track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->track, 0, 0);
    lv_obj_set_style_radius(view->track, 999, 0);
    lv_obj_set_style_pad_all(view->track, 0, 0);
    lv_obj_clear_flag(view->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(view->track, LV_ALIGN_CENTER);

    view->fill = lv_obj_create(view->track);
    lv_obj_set_height(view->fill, LV_PCT(100));
    lv_obj_set_width(view->fill, LV_PCT(0));
    lv_obj_set_pos(view->fill, 0, 0);
    lv_obj_set_align(view->fill, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_bg_color(view->fill, accent, 0);
    lv_obj_set_style_bg_opa(view->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->fill, 0, 0);
    lv_obj_set_style_radius(view->fill, 999, 0);
    lv_obj_set_style_pad_all(view->fill, 0, 0);
    lv_obj_clear_flag(view->fill, LV_OBJ_FLAG_SCROLLABLE);

    view->percent = lv_label_create(view->card);
    lv_obj_set_width(view->percent, card_w - GUI_GRID * 6);
    lv_obj_set_style_text_font(view->percent, progress_body_font(), 0);
    lv_obj_set_style_text_color(view->percent, text, 0);
    lv_obj_set_style_text_align(view->percent, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(view->percent, "0%");

    view->subtext = lv_label_create(view->card);
    lv_obj_set_width(view->subtext, card_w - GUI_GRID * 6);
    lv_obj_set_style_text_font(view->subtext, progress_body_font(), 0);
    lv_obj_set_style_text_color(view->subtext, text, 0);
    lv_obj_set_style_text_align(view->subtext, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view->subtext, LV_LABEL_LONG_WRAP);
    lv_label_set_text(view->subtext, "");
    lv_obj_add_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);

    view->active = true;
    return view;
}

void progress_bar_view_update(progress_bar_view_t *view, const char *title) {
    if (!view || !view->active || !view->title || !title) return;
    lv_label_set_text(view->title, title);
}

void progress_bar_view_set_subtext(progress_bar_view_t *view, const char *subtext) {
    if (!view || !view->active || !view->subtext) return;
    if (!subtext || subtext[0] == '\0') {
        lv_label_set_text(view->subtext, "");
        lv_obj_add_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(view->subtext, subtext);
    lv_obj_clear_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);
}

void progress_bar_view_set_progress(progress_bar_view_t *view, size_t current, size_t total) {
    if (!view || !view->active) return;
    int pct = 0;
    if (total > 0) {
        if (current > total) current = total;
        pct = (int)((current * 100u) / total);
    }
    if (view->fill) lv_obj_set_width(view->fill, LV_PCT(pct));
    if (view->percent) {
        char buf[48];
        if (total > 0) {
            snprintf(buf, sizeof(buf), "%d%%  %u / %u KB", pct, (unsigned)(current / 1024), (unsigned)(total / 1024));
        } else {
            snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        lv_label_set_text(view->percent, buf);
    }
}

void progress_bar_view_close(progress_bar_view_t *view) {
    if (!view) return;
    view->active = false;
    if (view->container && lv_obj_is_valid(view->container)) {
        lv_obj_del(view->container);
    }
    free(view);
}

bool progress_bar_view_is_active(const progress_bar_view_t *view) {
    return view ? view->active : false;
}
