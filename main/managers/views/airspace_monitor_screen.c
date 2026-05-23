#include "managers/views/airspace_monitor_screen.h"
#include "core/callbacks.h"
#include "core/ouis.h"
#include "esp_timer.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/airspace_monitor.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *root_container = NULL;
static lv_timer_t *update_timer = NULL;
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *lbl_reason = NULL;
static lv_obj_t *lbl_channel = NULL;
static lv_obj_t *lbl_pps = NULL;
static lv_obj_t *lbl_deauth = NULL;
static lv_obj_t *lbl_unique = NULL;
static lv_obj_t *lbl_insight = NULL;
static lv_obj_t *lbl_advice = NULL;
static lv_obj_t *suspect_cards[AIRSPACE_MAX_SUSPECTS] = {0};
static lv_obj_t *lbl_suspect[AIRSPACE_MAX_SUSPECTS] = {0};
static lv_obj_t *lbl_suspect_vendor[AIRSPACE_MAX_SUSPECTS] = {0};
static lv_obj_t *lbl_counts_1 = NULL;
static lv_obj_t *lbl_counts_2 = NULL;
static lv_obj_t *counts_card = NULL;
static lv_obj_t *content_scroller = NULL;

static uint32_t accent_color = 0x00FFFF;
static uint32_t bg_color = 0x0A0A0A;
static uint32_t card_color = 0x1A1A1A;
static uint32_t text_color = 0xFFFFFF;
static uint32_t dim_color = 0x888888;
static uint32_t warn_color = 0xFFAA00;
static uint32_t error_color = 0xFF4444;
static uint32_t good_color = 0x00FF00;

static uint32_t last_insight_ms = 0;
static uint32_t prev_pps = 0;
static uint32_t prev_kick = 0;
static uint8_t prev_tx = 0;
static airspace_threat_level_t insight_level = AIRSPACE_THREAT_QUIET;
static char insight_text[96] = "Learning normal activity";
static char advice_text[96] = "Monitor for 30 seconds to build context";
static bool touch_active = false;
static bool touch_moved = false;
static int16_t touch_last_y = 0;

static bool compact_layout(void) {
    return LV_VER_RES <= 320;
}

static bool tiny_layout(void) {
    return LV_VER_RES <= 160;
}

static const lv_font_t *body_font(void) {
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (tiny_layout()) return &lv_font_montserrat_8;
    if (compact_layout()) return &lv_font_montserrat_10;
    if (LV_VER_RES <= 160) return fs == 0 ? &lv_font_montserrat_8 : &lv_font_montserrat_10;
    if (LV_VER_RES <= 240) return fs == 0 ? &lv_font_montserrat_10 : &lv_font_montserrat_12;
    return fs == 0 ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
}

static const lv_font_t *title_font(void) {
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (tiny_layout()) return &lv_font_montserrat_12;
    if (compact_layout()) return &lv_font_montserrat_14;
    if (LV_VER_RES <= 160) return fs == 0 ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    if (LV_VER_RES <= 240) return fs == 0 ? &lv_font_montserrat_14 : &lv_font_montserrat_18;
    return fs == 0 ? &lv_font_montserrat_18 : &lv_font_montserrat_24;
}

static lv_obj_t *create_card(lv_obj_t *parent, int width_pct) {
    lv_obj_t *card = lv_obj_create(parent);
    int padding = tiny_layout() ? 2 : (compact_layout() ? 4 : (LV_VER_RES <= 100 ? 3 : (LV_VER_RES <= 160 ? 5 : 8)));
    lv_obj_set_size(card, LV_PCT(width_pct), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, padding, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(text_color), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, tiny_layout() ? 0 : (compact_layout() ? 1 : 2), 0);
    return card;
}

static lv_obj_t *create_card_title(lv_obj_t *card, const char *text) {
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, body_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(dim_color), 0);
    return label;
}

static void format_mac(const uint8_t mac[6], char *out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static uint32_t color_for_level(airspace_threat_level_t level) {
    switch (level) {
        case AIRSPACE_THREAT_ATTACK_LIKELY: return error_color;
        case AIRSPACE_THREAT_SUSPICIOUS: return warn_color;
        case AIRSPACE_THREAT_BUSY: return accent_color;
        case AIRSPACE_THREAT_QUIET:
        default: return good_color;
    }
}

static void reset_insight_state(void) {
    last_insight_ms = 0;
    prev_pps = 0;
    prev_kick = 0;
    prev_tx = 0;
    insight_level = AIRSPACE_THREAT_QUIET;
    snprintf(insight_text, sizeof(insight_text), "Learning normal activity");
    snprintf(advice_text, sizeof(advice_text), "Monitor for 30 seconds to build context");
}

static void update_insight(const airspace_monitor_snapshot_t *snap) {
    if (!snap) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t kick = snap->deauth_per_sec + snap->disassoc_per_sec;
    if (last_insight_ms != 0 && (uint32_t)(now_ms - last_insight_ms) < 1800U) {
        return;
    }

    if (last_insight_ms == 0) {
        snprintf(insight_text, sizeof(insight_text), "Watching for sudden changes");
        snprintf(advice_text, sizeof(advice_text), "Let it run briefly for a baseline");
        insight_level = AIRSPACE_THREAT_QUIET;
    } else if (kick >= prev_kick + 3 && kick >= 3) {
        snprintf(insight_text, sizeof(insight_text), "Kick spike: clients may be forced off");
        snprintf(advice_text, sizeof(advice_text), "Watch suspect MAC and channel now");
        insight_level = AIRSPACE_THREAT_ATTACK_LIKELY;
    } else if (prev_kick >= 3 && kick == 0) {
        snprintf(insight_text, sizeof(insight_text), "Kick traffic stopped");
        snprintf(advice_text, sizeof(advice_text), "Keep monitoring; attack may resume");
        insight_level = AIRSPACE_THREAT_BUSY;
    } else if (snap->packets_per_sec >= (prev_pps * 2U + 20U) && snap->packets_per_sec >= 30U) {
        snprintf(insight_text, sizeof(insight_text), "Traffic surge: many frames appeared");
        snprintf(advice_text, sizeof(advice_text), "Look for a rising suspect device");
        insight_level = AIRSPACE_THREAT_SUSPICIOUS;
    } else if (prev_pps >= 30U && (snap->packets_per_sec * 2U + 10U) < prev_pps) {
        snprintf(insight_text, sizeof(insight_text), "Traffic dropped: air is quieter");
        snprintf(advice_text, sizeof(advice_text), "Likely normal if no kick frames");
        insight_level = AIRSPACE_THREAT_BUSY;
    } else if (snap->unique_devices >= prev_tx + 5 && snap->unique_devices >= 8) {
        snprintf(insight_text, sizeof(insight_text), "New devices appeared quickly");
        snprintf(advice_text, sizeof(advice_text), "Busy area or clients reconnecting");
        insight_level = AIRSPACE_THREAT_BUSY;
    } else if (prev_tx >= snap->unique_devices + 5 && prev_tx >= 8) {
        snprintf(insight_text, sizeof(insight_text), "Several devices went quiet");
        snprintf(advice_text, sizeof(advice_text), "Could be channel hop or clients left");
        insight_level = AIRSPACE_THREAT_BUSY;
    } else if (snap->packets_per_sec < 3 && kick == 0) {
        snprintf(insight_text, sizeof(insight_text), "Very quiet airspace");
        snprintf(advice_text, sizeof(advice_text), "No action needed");
        insight_level = AIRSPACE_THREAT_QUIET;
    } else if (kick == 0) {
        snprintf(insight_text, sizeof(insight_text), "Normal chatter, no kick pattern");
        snprintf(advice_text, sizeof(advice_text), "Healthy unless suspect appears");
        insight_level = AIRSPACE_THREAT_QUIET;
    } else {
        snprintf(insight_text, sizeof(insight_text), "Some kick frames seen; watching trend");
        snprintf(advice_text, sizeof(advice_text), "A few kick frames can be normal");
        insight_level = AIRSPACE_THREAT_SUSPICIOUS;
    }

    prev_pps = snap->packets_per_sec;
    prev_kick = kick;
    prev_tx = snap->unique_devices;
    last_insight_ms = now_ms;
}

static void scroll_airspace(int dir) {
    if (!content_scroller) return;
    lv_coord_t step = lv_obj_get_height(content_scroller) / 2;
    if (step < 24) step = 24;
    lv_obj_scroll_by_bounded(content_scroller, 0, dir > 0 ? -step : step, LV_ANIM_OFF);
}

static void update_display_cb(lv_timer_t *timer) {
    (void)timer;

    airspace_monitor_snapshot_t snap = {0};
    airspace_monitor_get_snapshot(&snap);
    update_insight(&snap);

    if (lbl_status) {
        lv_label_set_text(lbl_status, airspace_monitor_threat_label(snap.threat_level));
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(color_for_level(snap.threat_level)), 0);
    }
    if (lbl_reason) lv_label_set_text(lbl_reason, snap.reason);
    const char *band = snap.current_channel > 14 ? "5G" : "2G";
    if (lbl_channel) {
        lv_label_set_text_fmt(lbl_channel, "Ch %u %s  plan %u  hop %lu/%lu",
                              (unsigned)snap.current_channel,
                              band,
                              (unsigned)snap.channel_count,
                              (unsigned long)snap.hop_success,
                              (unsigned long)snap.hop_fail);
    }
    if (lbl_pps) lv_label_set_text_fmt(lbl_pps, "Pkt %lu/s  Tx %u",
                                       (unsigned long)snap.packets_per_sec,
                                       (unsigned)snap.unique_devices);
    if (lbl_deauth) {
        lv_label_set_text_fmt(lbl_deauth, "Kick %lu/s",
                              (unsigned long)(snap.deauth_per_sec + snap.disassoc_per_sec));
        lv_obj_set_style_text_color(lbl_deauth,
                                    lv_color_hex((snap.deauth_per_sec + snap.disassoc_per_sec) >= 5 ? warn_color : text_color),
                                    0);
    }
    if (lbl_unique) lv_label_set_text_fmt(lbl_unique, "Total %lu  Up %lum%02lu",
                                          (unsigned long)snap.total_packets,
                                          (unsigned long)(snap.uptime_s / 60),
                                          (unsigned long)(snap.uptime_s % 60));
    if (lbl_insight) {
        lv_label_set_text(lbl_insight, insight_text);
        lv_obj_set_style_text_color(lbl_insight, lv_color_hex(color_for_level(insight_level)), 0);
    }
    if (lbl_advice) lv_label_set_text(lbl_advice, advice_text);

    for (uint8_t i = 0; i < AIRSPACE_MAX_SUSPECTS; i++) {
        if (!suspect_cards[i]) continue;

        if (i >= snap.suspect_count) {
            if (i == 0 && snap.suspect_count == 0) {
                lv_obj_clear_flag(suspect_cards[i], LV_OBJ_FLAG_HIDDEN);
                if (lbl_suspect[i]) lv_label_set_text(lbl_suspect[i], "No suspected device");
                if (lbl_suspect_vendor[i]) lv_label_set_text(lbl_suspect_vendor[i], "Passive channel hopping");
            } else {
                lv_obj_add_flag(suspect_cards[i], LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }

        const airspace_suspect_t *suspect = &snap.suspects[i];
        uint32_t kick_rate = suspect->deauth_rate + suspect->disassoc_rate;
        uint32_t kick_total = suspect->deauth_total + suspect->disassoc_total;
        char mac[18] = {0};
        char vendor[64] = "Unknown";
        format_mac(suspect->mac, mac, sizeof(mac));
        (void)ouis_lookup_vendor(mac, vendor, sizeof(vendor));

        lv_obj_clear_flag(suspect_cards[i], LV_OBJ_FLAG_HIDDEN);
        if (lbl_suspect[i]) {
            lv_label_set_text_fmt(lbl_suspect[i], "%u. %s  ch%u  %ddBm",
                                  (unsigned)(i + 1),
                                  mac,
                                  (unsigned)suspect->channel,
                                  (int)suspect->rssi);
        }
        if (lbl_suspect_vendor[i]) {
            lv_label_set_text_fmt(lbl_suspect_vendor[i], "%s | kick %lu/s | %lu kick frames",
                                  vendor,
                                  (unsigned long)kick_rate,
                                  (unsigned long)kick_total);
        }
    }

    if (lbl_counts_1) {
        lv_label_set_text_fmt(lbl_counts_1, compact_layout() ? "Mgmt %lu  Data %lu  Ctrl %lu" : "Mgmt:%lu Data:%lu Ctrl:%lu",
                              (unsigned long)snap.mgmt_packets,
                              (unsigned long)snap.data_packets,
                              (unsigned long)snap.ctrl_packets);
    }
    if (lbl_counts_2) {
        lv_label_set_text_fmt(lbl_counts_2, compact_layout() ? "Bcn %lu  Probe %lu  Auth %lu  Kick %lu" : "Bcn:%lu Probe:%lu Auth:%lu Deauth:%lu",
                              (unsigned long)snap.beacon_packets,
                              (unsigned long)snap.probe_packets,
                              (unsigned long)snap.auth_packets,
                              (unsigned long)(snap.deauth_packets + snap.disassoc_packets));
    }
}

static void airspace_input_callback(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        int16_t y = event->data.touch_data.point.y;
        if (event->data.touch_data.state == LV_INDEV_STATE_PR) {
            if (!touch_active) {
                touch_active = true;
                touch_moved = false;
                touch_last_y = y;
                return;
            }

            int16_t dy = y - touch_last_y;
            if (dy > 3 || dy < -3) {
                if (content_scroller) {
                    lv_obj_scroll_by_bounded(content_scroller, 0, dy, LV_ANIM_OFF);
                }
                touch_last_y = y;
                touch_moved = true;
            }
        } else if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            touch_active = false;
            if (!touch_moved) {
                display_manager_switch_view(&main_menu_view);
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_KEYBOARD) {
        if (event->type == INPUT_TYPE_JOYSTICK) {
            int button = event->data.joystick_index;
            if (button == 2) scroll_airspace(1);
            else if (button == 4) scroll_airspace(-1);
            else if (button == 0 || button == 1 || button == 3) display_manager_switch_view(&main_menu_view);
            return;
        }

        int key = event->data.key_value;
        if (key == LV_KEY_UP || key == 'k' || key == ';') {
            scroll_airspace(1);
        } else if (key == LV_KEY_DOWN || key == 'j' || key == '.') {
            scroll_airspace(-1);
        } else if (key == LV_KEY_ESC || key == 27 || key == 29 || key == '`' || key == 'q' || key == 'Q') {
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            display_manager_switch_view(&main_menu_view);
        } else if (event->data.encoder.direction > 0) {
            scroll_airspace(1);
        } else if (event->data.encoder.direction < 0) {
            scroll_airspace(-1);
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
    }
}

void airspace_monitor_view_create(void) {
    if (airspace_monitor_view.root != NULL) {
        return;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    accent_color = theme_palette_get_accent(theme);
    bg_color = theme_palette_get_background(theme);
    card_color = theme_palette_get_surface_alt(theme);
    text_color = theme_palette_get_text(theme);
    dim_color = theme_palette_get_text_muted(theme);
    reset_insight_state();

    airspace_monitor_reset();
    wifi_manager_start_monitor_mode(wifi_airspace_monitor_callback);
    (void)airspace_monitor_start();

    display_manager_fill_screen(lv_color_hex(bg_color));
    root_container = gui_screen_create_root(NULL, "Airspace", lv_color_hex(bg_color), LV_OPA_COVER);
    airspace_monitor_view.root = root_container;

    lv_obj_t *content = gui_screen_create_content(root_container, GUI_STATUS_BAR_HEIGHT);
    content_scroller = content;
    lv_obj_set_style_text_color(content, lv_color_hex(text_color), 0);
    lv_obj_set_style_pad_all(content, tiny_layout() ? 1 : (compact_layout() ? 2 : 4), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, tiny_layout() ? 1 : (compact_layout() ? 2 : 4), 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *status_card = create_card(content, 100);
    lbl_status = lv_label_create(status_card);
    lv_label_set_text(lbl_status, "Starting");
    lv_obj_set_style_text_font(lbl_status, title_font(), 0);
    lbl_reason = lv_label_create(status_card);
    lv_label_set_text(lbl_reason, "Passive monitor starting");
    lv_obj_set_style_text_font(lbl_reason, body_font(), 0);
    lv_obj_set_width(lbl_reason, LV_PCT(100));
    lv_label_set_long_mode(lbl_reason, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_t *rates_card = create_card(content, 100);
    if (!compact_layout()) {
        create_card_title(rates_card, "Live Rates");
    }
    lbl_channel = lv_label_create(rates_card);
    lv_obj_set_style_text_font(lbl_channel, body_font(), 0);
    lv_obj_set_style_text_color(lbl_channel, lv_color_hex(accent_color), 0);
    lbl_pps = lv_label_create(rates_card);
    lv_obj_set_style_text_font(lbl_pps, body_font(), 0);
    lbl_deauth = lv_label_create(rates_card);
    lv_obj_set_style_text_font(lbl_deauth, body_font(), 0);
    lbl_unique = lv_label_create(rates_card);
    lv_obj_set_style_text_font(lbl_unique, body_font(), 0);
    lv_obj_set_style_text_color(lbl_unique, lv_color_hex(dim_color), 0);

    if (compact_layout()) {
        lbl_counts_1 = lv_label_create(rates_card);
        lv_obj_set_style_text_font(lbl_counts_1, body_font(), 0);
        lv_obj_set_style_text_color(lbl_counts_1, lv_color_hex(dim_color), 0);
        lbl_counts_2 = lv_label_create(rates_card);
        lv_obj_set_style_text_font(lbl_counts_2, body_font(), 0);
        lv_obj_set_style_text_color(lbl_counts_2, lv_color_hex(dim_color), 0);
        lv_obj_set_width(lbl_counts_2, LV_PCT(100));
        lv_label_set_long_mode(lbl_counts_2, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    lv_obj_t *insight_card = create_card(content, 100);
    if (!compact_layout()) {
        create_card_title(insight_card, "Insight");
    }
    lbl_insight = lv_label_create(insight_card);
    lv_label_set_text(lbl_insight, insight_text);
    lv_obj_set_style_text_font(lbl_insight, body_font(), 0);
    lv_obj_set_width(lbl_insight, LV_PCT(100));
    lv_label_set_long_mode(lbl_insight, LV_LABEL_LONG_SCROLL_CIRCULAR);

    if (!tiny_layout()) {
        lv_obj_t *advice_card = create_card(content, 100);
        if (!compact_layout()) {
            create_card_title(advice_card, "Advice");
        }
        lbl_advice = lv_label_create(advice_card);
        lv_label_set_text(lbl_advice, advice_text);
        lv_obj_set_style_text_font(lbl_advice, body_font(), 0);
        lv_obj_set_style_text_color(lbl_advice, lv_color_hex(dim_color), 0);
        lv_obj_set_width(lbl_advice, LV_PCT(100));
        lv_label_set_long_mode(lbl_advice, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    for (uint8_t i = 0; i < AIRSPACE_MAX_SUSPECTS; i++) {
        suspect_cards[i] = create_card(content, 100);
        if (!compact_layout()) {
            char title[16] = {0};
            snprintf(title, sizeof(title), "Suspect %u", (unsigned)(i + 1));
            create_card_title(suspect_cards[i], title);
        }
        lbl_suspect[i] = lv_label_create(suspect_cards[i]);
        lv_obj_set_style_text_font(lbl_suspect[i], body_font(), 0);
        lv_obj_set_width(lbl_suspect[i], LV_PCT(100));
        lv_label_set_long_mode(lbl_suspect[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lbl_suspect_vendor[i] = lv_label_create(suspect_cards[i]);
        lv_obj_set_style_text_font(lbl_suspect_vendor[i], body_font(), 0);
        lv_obj_set_style_text_color(lbl_suspect_vendor[i], lv_color_hex(dim_color), 0);
        lv_obj_set_width(lbl_suspect_vendor[i], LV_PCT(100));
        lv_label_set_long_mode(lbl_suspect_vendor[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        if (i > 0) {
            lv_obj_add_flag(suspect_cards[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!compact_layout()) {
        counts_card = create_card(content, 100);
        create_card_title(counts_card, "Frame Mix");
        lbl_counts_1 = lv_label_create(counts_card);
        lv_obj_set_style_text_font(lbl_counts_1, body_font(), 0);
        lbl_counts_2 = lv_label_create(counts_card);
        lv_obj_set_style_text_font(lbl_counts_2, body_font(), 0);
        lv_obj_set_width(lbl_counts_2, LV_PCT(100));
        lv_label_set_long_mode(lbl_counts_2, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    display_manager_add_status_bar("Airspace Monitor");
    update_timer = lv_timer_create(update_display_cb, 250, NULL);
    update_display_cb(NULL);
}

void airspace_monitor_view_destroy(void) {
    if (update_timer) {
        lv_timer_del(update_timer);
        update_timer = NULL;
    }

    wifi_manager_stop_monitor_mode();
    airspace_monitor_stop();

    if (root_container) {
        lv_obj_del(root_container);
        root_container = NULL;
        airspace_monitor_view.root = NULL;
    }

    lbl_status = NULL;
    lbl_reason = NULL;
    lbl_channel = NULL;
    lbl_pps = NULL;
    lbl_deauth = NULL;
    lbl_unique = NULL;
    lbl_insight = NULL;
    lbl_advice = NULL;
    for (uint8_t i = 0; i < AIRSPACE_MAX_SUSPECTS; i++) {
        suspect_cards[i] = NULL;
        lbl_suspect[i] = NULL;
        lbl_suspect_vendor[i] = NULL;
    }
    lbl_counts_1 = NULL;
    lbl_counts_2 = NULL;
    counts_card = NULL;
    content_scroller = NULL;
    touch_active = false;
    touch_moved = false;
}

static void get_airspace_callback(void **callback) {
    if (callback) {
        *callback = (void *)airspace_input_callback;
    }
}

View airspace_monitor_view = {
    .root = NULL,
    .create = airspace_monitor_view_create,
    .destroy = airspace_monitor_view_destroy,
    .input_callback = airspace_input_callback,
    .name = "AirspaceMonitorView",
    .get_hardwareinput_callback = get_airspace_callback
};
