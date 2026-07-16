#include "managers/views/packet_monitor_screen.h"
#include "core/callbacks.h"
#include "gui/accessibility_fonts.h"
#include "gui/live_chart.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/error_popup.h"
#include "managers/views/keyboard_screen.h"
#include "managers/views/options_screen.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/wifi_channels.h"
#include "lvgl.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKET_CHART_POINTS 100
#define PACKET_UPDATE_MS 150
#define PACKET_SERIES_MAX THEME_PALETTE_SLOT_COUNT
#define PACKET_HOP_DWELL_MS 150

#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define PACKET_MONITOR_MAX_CHANNEL 165
#else
#define PACKET_MONITOR_MAX_CHANNEL 13
#endif

typedef enum {
    PACKET_START_SELECT = 0,
    PACKET_START_HOP,
    PACKET_START_CUSTOM,
} packet_start_mode_t;

static lv_obj_t *s_root;
static options_view_t *s_mode_options;
static live_chart_t *s_chart;
static lv_obj_t *s_summary;
static lv_obj_t *s_legend;
static lv_obj_t *s_legend_items[PACKET_SERIES_MAX];
static lv_obj_t *s_legend_labels[PACKET_SERIES_MAX];
static lv_timer_t *s_update_timer;
static packet_start_mode_t s_start_mode = PACKET_START_SELECT;
static uint8_t s_custom_channels[WIFI_CHANNELS_MAX];
static size_t s_custom_channel_count;
static char s_mode_label[64];
static bool s_monitor_active;
static bool s_touch_started;
static lv_point_t s_touch_start;

static volatile uint32_t s_total_packets;
static volatile uint32_t s_management_packets;
static volatile uint32_t s_control_packets;
static volatile uint32_t s_data_packets;
static volatile uint32_t s_total_bytes;
static volatile uint32_t s_channel_packets[PACKET_MONITOR_MAX_CHANNEL + 1];
static volatile uint8_t s_current_channel;
static volatile int8_t s_last_rssi;
static uint32_t s_channel_snapshots[PACKET_MONITOR_MAX_CHANNEL + 1];
static float s_channel_activity[PACKET_MONITOR_MAX_CHANNEL + 1];
static uint8_t s_series_channels[PACKET_SERIES_MAX];
static float s_target_rates[PACKET_SERIES_MAX];
static float s_smoothed_rates[PACKET_SERIES_MAX];
static uint16_t s_idle_samples[PACKET_SERIES_MAX];
static uint16_t s_hold_samples;
static int s_series_count;

static const uint32_t s_series_colors[PACKET_SERIES_MAX] = {
    0x00E5FF, 0xFF4D8D, 0xFFD166, 0x62E676, 0xA78BFA, 0xFF8A3D,
};

static void packet_monitor_observer(const wifi_promiscuous_pkt_t *packet,
                                    wifi_promiscuous_pkt_type_t type) {
    if (!s_monitor_active || !packet) return;
    s_total_packets++;
    s_total_bytes += packet->rx_ctrl.sig_len;
    s_current_channel = packet->rx_ctrl.channel;
    s_last_rssi = packet->rx_ctrl.rssi;
    if (s_current_channel > 0 && s_current_channel <= PACKET_MONITOR_MAX_CHANNEL) {
        s_channel_packets[s_current_channel]++;
    }
    if (type == WIFI_PKT_MGMT) s_management_packets++;
    else if (type == WIFI_PKT_CTRL) s_control_packets++;
    else if (type == WIFI_PKT_DATA) s_data_packets++;
}

static void packet_monitor_stop(void) {
    if (s_update_timer) {
        lv_timer_del(s_update_timer);
        s_update_timer = NULL;
    }
    if (!s_monitor_active) return;
    s_monitor_active = false;
    wifi_raw_set_observer(NULL);
    wifi_manager_stop_wireshark_channel_hop();
    wifi_manager_stop_monitor_mode();
    wifi_callbacks_set_pcap_enabled(true);
    status_display_show_status("Monitor Stop");
}

static void packet_monitor_return(void) {
    s_start_mode = PACKET_START_SELECT;
    packet_monitor_stop();
    display_manager_switch_view(&options_menu_view);
}

static bool parse_channels(const char *text, uint8_t *channels, size_t *count) {
    if (!text || !channels || !count) return false;
    size_t parsed = 0;
    const char *cursor = text;

    while (true) {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || !isdigit((unsigned char)*cursor)) return false;

        char *end = NULL;
        long channel = strtol(cursor, &end, 10);
        if (end == cursor || channel < 1 || channel > PACKET_MONITOR_MAX_CHANNEL) return false;

        bool seen = false;
        for (size_t i = 0; i < parsed; i++) {
            if (channels[i] == (uint8_t)channel) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (parsed >= WIFI_CHANNELS_MAX) return false;
            channels[parsed++] = (uint8_t)channel;
        }

        cursor = end;
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0') break;
        if (*cursor != ',') return false;
        cursor++;
    }

    *count = parsed;
    return parsed > 0;
}

static void packet_channels_submit(const char *text) {
    uint8_t channels[WIFI_CHANNELS_MAX] = {0};
    size_t count = 0;
    if (!parse_channels(text, channels, &count)) {
        error_popup_create("Enter channels like 1 or 1,6,11");
        return;
    }

    memcpy(s_custom_channels, channels, count);
    s_custom_channel_count = count;
    s_start_mode = PACKET_START_CUSTOM;
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&packet_monitor_view);
}

static void packet_mode_activate(int selected) {
    if (selected == 0) {
        s_start_mode = PACKET_START_HOP;
        display_manager_switch_view(&packet_monitor_view);
    } else if (selected == 1) {
        keyboard_view_set_return_view(&packet_monitor_view);
        keyboard_view_set_submit_callback(packet_channels_submit);
        keyboard_view_set_placeholder("Channels, e.g. 1 or 1,6,11");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
    } else {
        packet_monitor_return();
    }
}

static void packet_mode_click(lv_event_t *event) {
    packet_mode_activate((int)(intptr_t)lv_event_get_user_data(event));
}

static void packet_refresh_legend(void) {
    for (int series = 0; series < s_series_count; series++) {
        lv_obj_t *label = s_legend_labels[series];
        if (!label || !lv_obj_is_valid(label)) continue;
        if (s_series_channels[series] > 0) {
            lv_label_set_text_fmt(label, "CH %u", (unsigned)s_series_channels[series]);
        } else {
            lv_label_set_text(label, "CH --");
        }
    }
}

static bool packet_rebind_busiest_channels(void) {
    uint8_t busiest[PACKET_SERIES_MAX] = {0};
    float busiest_activity[PACKET_SERIES_MAX] = {0};

    for (int channel = 1; channel <= PACKET_MONITOR_MAX_CHANNEL; channel++) {
        float activity = s_channel_activity[channel];
        if (activity <= 0.0f) continue;
        for (int slot = 0; slot < PACKET_SERIES_MAX; slot++) {
            if (activity <= busiest_activity[slot]) continue;
            for (int move = PACKET_SERIES_MAX - 1; move > slot; move--) {
                busiest[move] = busiest[move - 1];
                busiest_activity[move] = busiest_activity[move - 1];
            }
            busiest[slot] = (uint8_t)channel;
            busiest_activity[slot] = activity;
            break;
        }
    }

    uint8_t next_channels[PACKET_SERIES_MAX] = {0};
    bool selected[PACKET_SERIES_MAX] = {0};

    /* Keep a channel's color stable while it remains among the busiest. */
    for (int series = 0; series < s_series_count; series++) {
        for (int slot = 0; slot < PACKET_SERIES_MAX; slot++) {
            if (!selected[slot] && s_series_channels[series] == busiest[slot]) {
                next_channels[series] = busiest[slot];
                selected[slot] = true;
                break;
            }
        }
    }

    for (int slot = 0; slot < PACKET_SERIES_MAX; slot++) {
        if (selected[slot]) continue;
        for (int series = 0; series < s_series_count; series++) {
            if (next_channels[series] == 0) {
                next_channels[series] = busiest[slot];
                selected[slot] = true;
                break;
            }
        }
    }

    bool changed = false;
    for (int series = 0; series < s_series_count; series++) {
        if (s_series_channels[series] == next_channels[series]) continue;
        s_series_channels[series] = next_channels[series];
        s_target_rates[series] = 0.0f;
        s_smoothed_rates[series] = 0.0f;
        s_idle_samples[series] = 0;
        changed = true;
    }
    return changed;
}

static void packet_update_cb(lv_timer_t *timer) {
    (void)timer;

    uint32_t channel_deltas[PACKET_MONITOR_MAX_CHANNEL + 1] = {0};
    for (int channel = 1; channel <= PACKET_MONITOR_MAX_CHANNEL; channel++) {
        uint32_t channel_total = s_channel_packets[channel];
        channel_deltas[channel] = channel_total - s_channel_snapshots[channel];
        s_channel_snapshots[channel] = channel_total;
        s_channel_activity[channel] *= 0.985f;
        if (channel_deltas[channel] > 0) {
            s_channel_activity[channel] +=
                (float)channel_deltas[channel] * (1000.0f / PACKET_UPDATE_MS);
        }
    }

    bool legend_changed = false;
    if (s_start_mode == PACKET_START_HOP) {
        legend_changed = packet_rebind_busiest_channels();
    }

    bool hopping = s_start_mode == PACKET_START_HOP || s_custom_channel_count > 1;
    float series_values[PACKET_SERIES_MAX] = {0};
    float displayed_pps = 0.0f;
    for (int series = 0; series < s_series_count; series++) {
        uint8_t channel = s_series_channels[series];
        uint32_t channel_delta = channel > 0 ? channel_deltas[channel] : 0;
        if (channel_delta > 0) {
            s_target_rates[series] = (float)channel_delta * (1000.0f / PACKET_UPDATE_MS);
            s_idle_samples[series] = 0;
        } else if (!hopping || ++s_idle_samples[series] > s_hold_samples) {
            s_target_rates[series] = 0.0f;
        }
        s_smoothed_rates[series] += (s_target_rates[series] - s_smoothed_rates[series]) * 0.24f;
        if (s_target_rates[series] == 0.0f && s_smoothed_rates[series] < 0.5f) {
            s_smoothed_rates[series] = 0.0f;
        }
        series_values[series] = s_smoothed_rates[series];
        displayed_pps += series_values[series];
    }
    live_chart_push_series(s_chart, series_values, s_series_count);
    if (legend_changed) packet_refresh_legend();
    uint32_t pps = (uint32_t)(displayed_pps + 0.5f);

    if (s_summary && lv_obj_is_valid(s_summary)) {
        lv_label_set_text_fmt(s_summary, "%s  %lu pkt/s",
                              s_mode_label, (unsigned long)pps);
    }
}

static void packet_monitor_create_selector(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    s_root = gui_screen_create_root(NULL, NULL,
                                    lv_color_hex(theme_palette_get_background(theme)), LV_OPA_COVER);
    packet_monitor_view.root = s_root;
    s_mode_options = options_view_create(s_root, "Packet Visualizer");
    if (!s_mode_options) return;
    options_view_add_item(s_mode_options, "Channel Hopping", packet_mode_click, (void *)(intptr_t)0);
    options_view_add_item(s_mode_options, "Choose Channel(s)...", packet_mode_click, (void *)(intptr_t)1);
    options_view_add_item(s_mode_options, LV_SYMBOL_LEFT " Back", packet_mode_click, (void *)(intptr_t)2);
    options_view_set_selected(s_mode_options, 0);
}

static void packet_monitor_create_visualizer(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    s_root = gui_screen_create_root_no_bg(NULL, "Packet Visualizer", surface, LV_OPA_COVER);
    packet_monitor_view.root = s_root;

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_H);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_coord_t row_gap = LV_VER_RES <= 80 ? 1 : GUI_GRID;
    lv_obj_set_style_pad_row(content, row_gap, 0);

    memset(s_series_channels, 0, sizeof(s_series_channels));
    memset(s_target_rates, 0, sizeof(s_target_rates));
    memset(s_smoothed_rates, 0, sizeof(s_smoothed_rates));
    memset(s_idle_samples, 0, sizeof(s_idle_samples));
    memset(s_legend_items, 0, sizeof(s_legend_items));
    memset(s_legend_labels, 0, sizeof(s_legend_labels));
    s_series_count = s_start_mode == PACKET_START_HOP ? PACKET_SERIES_MAX :
                     LV_MIN((int)s_custom_channel_count, PACKET_SERIES_MAX);
    if (s_start_mode == PACKET_START_CUSTOM) {
        memcpy(s_series_channels, s_custom_channels, (size_t)s_series_count);
    }
    size_t sweep_channels = s_custom_channel_count;
    if (s_start_mode == PACKET_START_HOP) {
        uint8_t channels[WIFI_CHANNELS_MAX];
        sweep_channels = wifi_channels_build_country_list(channels, WIFI_CHANNELS_MAX);
    }
    s_hold_samples = (uint16_t)((sweep_channels * PACKET_HOP_DWELL_MS + PACKET_UPDATE_MS - 1) /
                                PACKET_UPDATE_MS);
    if (s_hold_samples < 1) s_hold_samples = 1;

    const lv_font_t *font = LV_VER_RES <= 80 ? &lv_font_montserrat_8 : accessibility_get_font_small();
    s_summary = lv_label_create(content);
    lv_obj_set_width(s_summary, LV_PCT(100));
    lv_obj_set_style_text_font(s_summary, font, 0);
    lv_obj_set_style_text_color(s_summary, lv_color_hex(theme_palette_get_accent(theme)), 0);
    lv_obj_set_style_text_align(s_summary, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_DOT);

    lv_coord_t line_height = lv_font_get_line_height(font);
    int legend_columns = LV_MAX(1, LV_HOR_RES / 50);
    int legend_rows = (s_series_count + legend_columns - 1) / legend_columns;
    lv_coord_t legend_height = line_height * legend_rows;
    lv_coord_t max_height = LV_VER_RES - GUI_STATUS_BAR_H - line_height - legend_height - row_gap * 2;
    if (max_height > 160) max_height = 160;
    if (max_height < 20) max_height = 20;
    live_chart_config_t config = {
        .type = LIVE_CHART_AREA,
        .data_points = PACKET_CHART_POINTS,
        .y_min = 0.0f,
        .y_max = 0.0f,
        .x_label_left = "15s ago",
        .x_label_right = "now",
        .y_label_fmt = "%d",
        .grid_lines = 3,
        .max_height = max_height,
        .flat = true,
        .show_peaks = false,
        .auto_scroll = true,
    };
    s_chart = live_chart_create(content, NULL, &config);
    if (!s_chart) return;

    if (!live_chart_set_series_count(s_chart, s_series_count)) {
        error_popup_create("Unable to allocate packet graph");
        return;
    }
    for (int series = 0; series < s_series_count; series++) {
        live_chart_set_series_color(s_chart, series, lv_color_hex(s_series_colors[series]));
    }

    s_legend = lv_obj_create(content);
    lv_obj_set_size(s_legend, LV_PCT(100), legend_height);
    lv_obj_set_style_bg_opa(s_legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_legend, 0, 0);
    lv_obj_set_style_pad_all(s_legend, 0, 0);
    lv_obj_set_style_pad_gap(s_legend, 0, 0);
    lv_obj_set_flex_flow(s_legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_legend, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int series = 0; series < s_series_count; series++) {
        lv_color_t color = lv_color_hex(s_series_colors[series]);
        lv_obj_t *item = lv_obj_create(s_legend);
        s_legend_items[series] = item;
        lv_obj_set_size(item, 50, line_height);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_style_pad_column(item, 2, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *swatch = lv_obj_create(item);
        lv_obj_set_size(swatch, 8, 3);
        lv_obj_set_style_bg_color(swatch, color, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *label = lv_label_create(item);
        s_legend_labels[series] = label;
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, color, 0);
    }
    packet_refresh_legend();

    s_total_packets = 0;
    s_management_packets = 0;
    s_control_packets = 0;
    s_data_packets = 0;
    s_total_bytes = 0;
    memset((void *)s_channel_packets, 0, sizeof(s_channel_packets));
    memset(s_channel_snapshots, 0, sizeof(s_channel_snapshots));
    memset(s_channel_activity, 0, sizeof(s_channel_activity));
    s_current_channel = 0;
    s_last_rssi = 0;
    s_monitor_active = true;
    wifi_callbacks_set_pcap_enabled(false);
    wifi_raw_set_observer(packet_monitor_observer);
    wifi_manager_start_monitor_mode(wifi_raw_scan_callback);

    if (s_start_mode == PACKET_START_CUSTOM) {
        esp_err_t err = wifi_manager_start_wireshark_channel_list(s_custom_channels, s_custom_channel_count);
        if (err != ESP_OK) {
            packet_monitor_stop();
            error_popup_create("Unable to use those Wi-Fi channels");
            return;
        }
        if (s_custom_channel_count == 1) {
            snprintf(s_mode_label, sizeof(s_mode_label), "Locked");
            s_current_channel = s_custom_channels[0];
        } else {
            snprintf(s_mode_label, sizeof(s_mode_label), "Custom hop");
        }
    } else {
        snprintf(s_mode_label, sizeof(s_mode_label), "Hopping");
        wifi_manager_start_wireshark_channel_hop();
    }

    lv_label_set_text(s_summary, s_mode_label);
    s_update_timer = lv_timer_create(packet_update_cb, PACKET_UPDATE_MS, NULL);
    status_display_show_status("Packet Visualizer");
}

static bool point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 &&
           point->y >= area.y1 && point->y <= area.y2;
}

static void packet_monitor_input(InputEvent *event) {
    if (!event) return;

    if (s_monitor_active) {
        if (event->type == INPUT_TYPE_TOUCH) {
            if (event->data.touch_data.state == LV_INDEV_STATE_PR) {
                s_touch_started = true;
            } else if (event->data.touch_data.state == LV_INDEV_STATE_REL && s_touch_started) {
                s_touch_started = false;
                packet_monitor_return();
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed &&
                   (event->data.joystick_index == 0 || event->data.joystick_index == 1)) {
            packet_monitor_return();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int key = event->data.key_value;
            if (key == LV_KEY_ESC || key == 29 || key == '`' || key == 'q' || key == 'Q') {
                packet_monitor_return();
            }
        } else if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
            packet_monitor_return();
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            packet_monitor_return();
        }
        return;
    }

    if (!s_mode_options) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        const lv_indev_data_t *touch = &event->data.touch_data;
        if (touch->state == LV_INDEV_STATE_PR) {
            s_touch_started = true;
            s_touch_start = touch->point;
        } else if (touch->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            lv_obj_t *list = options_view_get_list(s_mode_options);
            for (int i = 0; i < options_view_get_item_count(s_mode_options); i++) {
                lv_obj_t *row = lv_obj_get_child(list, i);
                if (point_in_obj(row, &s_touch_start) && point_in_obj(row, &touch->point)) {
                    packet_mode_activate(i);
                    break;
                }
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        int button = event->data.joystick_index;
        if (button == 0) packet_monitor_return();
        else if (button == 2) options_view_move_selection(s_mode_options, -1);
        else if (button == 4) options_view_move_selection(s_mode_options, 1);
        else if (button == 1) packet_mode_activate(options_view_get_selected(s_mode_options));
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_UP || key == 'k' || key == ';') options_view_move_selection(s_mode_options, -1);
        else if (key == LV_KEY_DOWN || key == 'j' || key == '.') options_view_move_selection(s_mode_options, 1);
        else if (key == LV_KEY_ENTER || key == 13) packet_mode_activate(options_view_get_selected(s_mode_options));
        else if (key == LV_KEY_ESC || key == 29 || key == '`') packet_monitor_return();
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) packet_mode_activate(options_view_get_selected(s_mode_options));
        else if (event->data.encoder.direction != 0) {
            options_view_move_selection(s_mode_options, event->data.encoder.direction > 0 ? 1 : -1);
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        packet_monitor_return();
    }
}

static void packet_monitor_create(void) {
    s_touch_started = false;
    if (s_start_mode == PACKET_START_SELECT) packet_monitor_create_selector();
    else packet_monitor_create_visualizer();
}

static void packet_monitor_destroy(void) {
    packet_monitor_stop();
    if (s_mode_options) {
        options_view_destroy(s_mode_options);
        s_mode_options = NULL;
    }
    if (s_chart) {
        live_chart_destroy(s_chart);
        s_chart = NULL;
    }
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = NULL;
    s_summary = NULL;
    s_legend = NULL;
    packet_monitor_view.root = NULL;
}

static void get_packet_monitor_callback(void **callback) {
    if (callback) *callback = packet_monitor_view.input_callback;
}

View packet_monitor_view = {
    .root = NULL,
    .create = packet_monitor_create,
    .destroy = packet_monitor_destroy,
    .name = "Packet Visualizer",
    .get_hardwareinput_callback = get_packet_monitor_callback,
    .input_callback = packet_monitor_input,
};
