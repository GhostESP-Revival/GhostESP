#include "managers/views/channel_congestion_screen.h"
#include "gui/accessibility_fonts.h"
#include "gui/live_chart.h"
#include "gui/scan_status.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/options_screen.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/wifi_channels.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_IDF_TARGET_ESP32C5
#define CONGESTION_SCAN_ESTIMATE_SECONDS 6
#else
#define CONGESTION_SCAN_ESTIMATE_SECONDS 5
#endif
#define CONGESTION_SCAN_TIMEOUT_SECONDS (CONGESTION_SCAN_ESTIMATE_SECONDS + 10)

static lv_obj_t *s_root;
static live_chart_t *s_chart;
static lv_obj_t *s_summary_label;
static scan_status_t *s_scan_status;
static lv_timer_t *s_poll_timer;
static int64_t s_scan_started_us;
static bool s_scan_running;
static bool s_leaving;

static void congestion_set_summary(const char *message, bool error) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (s_chart) live_chart_clear_alert(s_chart);
    if (s_summary_label && lv_obj_is_valid(s_summary_label)) {
        lv_label_set_text(s_summary_label, message ? message : "");
        lv_obj_set_style_text_color(
            s_summary_label,
            lv_color_hex(error ? theme_palette_get_text_muted(theme) : theme_palette_get_accent(theme)), 0);
        lv_obj_clear_flag(s_summary_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        display_manager_add_status_bar(error ? "Scan Error" : "Channel Congestion");
    }
}

static void congestion_show_error(const char *message) {
    display_manager_add_status_bar("Channel Congestion");
    congestion_set_summary(message, true);
}

static void congestion_stop_scan(void) {
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_scan_running && ap_scan_is_running()) ap_scan_cancel_async();
    s_scan_running = false;
    if (s_scan_status) {
        scan_status_close(s_scan_status);
        s_scan_status = NULL;
    }
}

static void congestion_return_to_options(void) {
    if (s_leaving) return;
    s_leaving = true;
    display_manager_go_back();
}

static void sort_channels(uint8_t *channels, int count) {
    for (int i = 1; i < count; i++) {
        uint8_t channel = channels[i];
        int j = i - 1;
        while (j >= 0 && channels[j] > channel) {
            channels[j + 1] = channels[j];
            j--;
        }
        channels[j + 1] = channel;
    }
}

static int find_channel(const uint8_t *channels, int count, uint8_t channel) {
    for (int i = 0; i < count; i++) {
        if (channels[i] == channel) return i;
    }
    return -1;
}

static void congestion_show_results(void) {
    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    uint8_t channels[WIFI_CHANNELS_MAX] = {0};
    int channel_count = 0;
    for (uint16_t i = 0; aps && i < ap_count && channel_count < WIFI_CHANNELS_MAX; i++) {
        uint8_t channel = aps[i].primary;
        if (channel > 0 && find_channel(channels, channel_count, channel) < 0) {
            channels[channel_count++] = channel;
        }
    }
    sort_channels(channels, channel_count);

    float counts[WIFI_CHANNELS_MAX] = {0};
    int peak_index = -1;
    int peak_count = 0;
    for (uint16_t i = 0; aps && i < ap_count; i++) {
        int index = find_channel(channels, channel_count, aps[i].primary);
        if (index < 0) continue;
        counts[index] += 1.0f;
        if ((int)counts[index] > peak_count) {
            peak_count = (int)counts[index];
            peak_index = index;
        }
    }

    display_manager_add_status_bar(ap_count == 0 ? "No APs" : "Channel Congestion");
    if (channel_count <= 0) {
        live_chart_clear(s_chart);
        congestion_set_summary("No access points found", true);
        status_display_show_status("No AP Found");
        return;
    }
    if (!live_chart_set_data(s_chart, counts, channel_count)) {
        congestion_set_summary("Unable to build channel chart", true);
        return;
    }

    int channel_labels[WIFI_CHANNELS_MAX];
    for (int i = 0; i < channel_count; i++) channel_labels[i] = channels[i];
    live_chart_set_x_labels(s_chart, "", "");
    (void)live_chart_set_x_values(s_chart, channel_labels, channel_count);
    live_chart_set_cursor(s_chart, peak_index);

    char summary[80];
    if (ap_count == 0 || peak_index < 0) {
        snprintf(summary, sizeof(summary), "No access points found");
    } else {
        snprintf(summary, sizeof(summary), ap_scan_results_truncated() ?
                     "%u+ APs detected  Busiest: CH %u (%d)" :
                     "%u APs detected  Busiest: CH %u (%d)",
                 (unsigned)ap_count, (unsigned)channels[peak_index], peak_count);
    }
    congestion_set_summary(summary, false);
    status_display_show_status(ap_count > 0 ? "Congest Done" : "No AP Found");
}

static void congestion_poll_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_scan_running || s_leaving) return;

    int elapsed_seconds = (int)((esp_timer_get_time() - s_scan_started_us) / 1000000);
    if (elapsed_seconds >= CONGESTION_SCAN_TIMEOUT_SECONDS) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
        ap_scan_cancel_async();
        s_scan_running = false;
        if (s_scan_status) {
            scan_status_close(s_scan_status);
            s_scan_status = NULL;
        }
        congestion_show_error("Wi-Fi scan timed out");
        return;
    }
    int remaining = CONGESTION_SCAN_ESTIMATE_SECONDS - elapsed_seconds;
    if (remaining < 1) remaining = 1;
    if (s_scan_status) {
        char progress[48];
        snprintf(progress, sizeof(progress), "Checking all channels | %ds", remaining);
        scan_status_set_subtext(s_scan_status, progress);
    }

    if (!ap_scan_check_done()) return;

    lv_timer_del(s_poll_timer);
    s_poll_timer = NULL;
    ap_scan_finish_async();
    s_scan_running = false;
    if (s_scan_status) {
        scan_status_close(s_scan_status);
        s_scan_status = NULL;
    }
    congestion_show_results();
}

static void congestion_input_callback(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) congestion_return_to_options();
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 0 || button == 1) congestion_return_to_options();
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        /* Enter opens this view from keyboard-driven menus, so never treat a
         * queued or repeated activation key as a request to close it. */
        if (key == LV_KEY_ESC || key == 29 || key == '`' || key == 'q' || key == 'Q') {
            congestion_return_to_options();
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) congestion_return_to_options();
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        congestion_return_to_options();
    }
}

static void channel_congestion_create(void) {
    if (s_root) return;

    s_leaving = false;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    s_root = gui_screen_create_root_no_bg(NULL, "Channel Congestion", surface, LV_OPA_COVER);
    channel_congestion_view.root = s_root;

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_H);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, LV_VER_RES <= 80 ? 1 : GUI_GRID * 2, 0);

    lv_coord_t chart_max_height = 160;
    if (LV_VER_RES > 80) {
        const lv_font_t *summary_font = accessibility_get_font_small();
        s_summary_label = lv_label_create(content);
        lv_label_set_text(s_summary_label, "");
        lv_obj_set_width(s_summary_label, LV_PCT(100));
        lv_obj_set_style_text_font(s_summary_label, summary_font, 0);
        lv_obj_set_style_text_align(s_summary_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_summary_label, LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_summary_label, LV_OBJ_FLAG_HIDDEN);

        lv_coord_t available = LV_VER_RES - GUI_STATUS_BAR_H -
                               lv_font_get_line_height(summary_font) - GUI_GRID * 2;
        if (chart_max_height > available) chart_max_height = available;
    }
    if (chart_max_height < 32) chart_max_height = 32;

    live_chart_config_t config = {
        .type = LIVE_CHART_BAR,
        .data_points = WIFI_CHANNELS_MAX,
        .y_min = 0.0f,
        .y_max = 0.0f,
        .x_label_left = "",
        .x_label_right = "",
        .y_label_fmt = "%d",
        .grid_lines = 4,
        .max_height = chart_max_height,
        .flat = true,
        .show_peaks = false,
        .auto_scroll = false,
    };
    s_chart = live_chart_create(content, NULL, &config);
    if (!s_chart) return;

    s_scan_status = scan_status_create("Scanning Wi-Fi channels");
    if (s_scan_status) {
        scan_status_set_subtext(s_scan_status, LV_VER_RES <= 80 ? "Please wait" : "Checking all channels");
        scan_status_set_cancel_cb(s_scan_status, congestion_return_to_options);
    }

    if (ap_scan_start_async() != ESP_OK) {
        if (s_scan_status) {
            scan_status_close(s_scan_status);
            s_scan_status = NULL;
        }
        congestion_show_error("Wi-Fi scan could not start");
        return;
    }

    s_scan_running = true;
    s_scan_started_us = esp_timer_get_time();
    s_poll_timer = lv_timer_create(congestion_poll_cb, 100, NULL);
}

static void channel_congestion_destroy(void) {
    congestion_stop_scan();
    if (s_chart) {
        live_chart_destroy(s_chart);
        s_chart = NULL;
    }
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = NULL;
    s_summary_label = NULL;
    channel_congestion_view.root = NULL;
    s_leaving = false;
}

static void get_channel_congestion_callback(void **callback) {
    if (callback) *callback = channel_congestion_view.input_callback;
}

View channel_congestion_view = {
    .root = NULL,
    .create = channel_congestion_create,
    .destroy = channel_congestion_destroy,
    .name = "Channel Congestion",
    .get_hardwareinput_callback = get_channel_congestion_callback,
    .input_callback = congestion_input_callback,
};
