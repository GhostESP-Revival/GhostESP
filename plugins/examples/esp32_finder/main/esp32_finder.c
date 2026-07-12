#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"
#include <stdio.h>
#include <string.h>

#define MAX_FOUND 16
#define MAX_RESULT_LABELS 8
#define SCAN_INTERVAL_MS 6000
#define SCAN_POLL_INTERVAL_MS 250
#define SWEEP_INTERVAL_MS 60
#define TRIG_SCALE 1024

static const ghostesp_api_t *api;
static ghostesp_theme_t theme;
static ghostesp_layout_t layout;
static ghostesp_touch_state_t touch_state;

static ghostesp_ui_obj_t screen;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t info_panel;
static ghostesp_ui_obj_t lbl_status;
static ghostesp_ui_obj_t result_labels[MAX_RESULT_LABELS];
static ghostesp_ui_timer_t scan_timer;
static ghostesp_ui_timer_t sweep_timer;
static ghostesp_ui_obj_t touch_bar;

static int canvas_w, canvas_h;
static int cx, cy, radar_r;
static int line_h, visible_lines;
static int sweep_step;
static int scan_count;
static int scroll_offset;
static uint32_t last_scan_ms;
static bool scan_in_progress;

static char s_app_id[] = "esp32_finder";
static char s_app_name[] = "ESP32 Finder";

#define ESP32_FINDER_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, ui_has_touchscreen) + sizeof(((ghostesp_api_t *)0)->ui_has_touchscreen))

typedef struct {
    char label[48];
    int8_t rssi;
    uint8_t channel;
} found_device_t;

static found_device_t found[MAX_FOUND];

static const int16_t trig_cos[36] = {
    1024, 1008, 962, 887, 784, 658, 512, 350, 178, 0,
    -178, -350, -512, -658, -784, -887, -962, -1008, -1024, -1008,
    -962, -887, -784, -658, -512, -350, -178, 0, 178, 350,
    512, 658, 784, 887, 962, 1008
};

static const int16_t trig_sin[36] = {
    0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024,
    1008, 962, 887, 784, 658, 512, 350, 178, 0, -178,
    -350, -512, -658, -784, -887, -962, -1008, -1024, -1008, -962,
    -887, -784, -658, -512, -350, -178
};

static void do_scan(void *user);
static void scan_timer_cb(void *user);
static void collect_scan_results(void);
static void redraw(void);
static void update_result_labels(void);

static bool is_espressif(const uint8_t *mac) {
    return (mac[0] == 0xA4 && mac[1] == 0xCF && mac[2] == 0x12) ||
           (mac[0] == 0x24 && mac[1] == 0x6F && mac[2] == 0x28) ||
           (mac[0] == 0x30 && mac[1] == 0xAE && mac[2] == 0xA4) ||
           (mac[0] == 0xEC && mac[1] == 0xFA && mac[2] == 0xBC) ||
           (mac[0] == 0x10 && mac[1] == 0x52 && mac[2] == 0x1C) ||
           (mac[0] == 0x84 && mac[1] == 0xCC && mac[2] == 0xA8) ||
           (mac[0] == 0xE8 && mac[1] == 0xDB && mac[2] == 0x84) ||
           (mac[0] == 0x00 && mac[1] == 0x4B && mac[2] == 0x12);
}

static void touch_back_clicked(void *user) { (void)user; GH_VOID(api, app_exit); }
static void touch_scan_clicked(void *user) { (void)user; do_scan(NULL); }

static void create_touch_controls(void) {
    if (touch_bar) { /* already exists */ return; }
    if (!layout.has_touch || !api->ui_touch_bar_create) return;
    touch_bar = api->ui_touch_bar_create(NULL);
    if (!touch_bar) return;
    if (api->ui_touch_bar_add_back) api->ui_touch_bar_add_back(touch_bar, touch_back_clicked, NULL);
    if (api->ui_touch_bar_add_up) api->ui_touch_bar_add_up(touch_bar, touch_scan_clicked, NULL);
    if (api->ui_touch_bar_add_down) api->ui_touch_bar_add_down(touch_bar, touch_scan_clicked, NULL);
}

static void destroy_views(void) {
    touch_bar = NULL;
    if (scan_timer) { api->ui_timer_delete(scan_timer); scan_timer = NULL; }
    if (sweep_timer) { api->ui_timer_delete(sweep_timer); sweep_timer = NULL; }
    lbl_status = NULL;
    for (int i = 0; i < MAX_RESULT_LABELS; i++) result_labels[i] = NULL;
    info_panel = NULL; canvas = NULL; screen = NULL;
    scan_count = 0; sweep_step = 0; scroll_offset = 0;
    last_scan_ms = 0; scan_in_progress = false;
}

static void calc_layout(void) {
    int margin = layout.compact ? 2 : 4;
    line_h = layout.compact ? 13 : 15;

    if (layout.content_w > layout.content_h) {
        canvas_w = layout.content_h;
        canvas_h = layout.content_h;
        if (canvas_w > layout.content_w / 2) canvas_w = layout.content_w / 2;
        radar_r = (canvas_w < canvas_h ? canvas_w : canvas_h) / 2 - margin - 2;
        if (radar_r < 18) radar_r = 18;
        cx = canvas_w / 2; cy = canvas_h / 2;
        visible_lines = (layout.content_h - margin * 2 - line_h) / line_h;
    } else {
        canvas_w = layout.content_w;
        canvas_h = layout.content_h / 2;
        radar_r = (canvas_w < canvas_h ? canvas_w : canvas_h) / 2 - margin - 2;
        if (radar_r < 18) radar_r = 18;
        cx = canvas_w / 2; cy = canvas_h / 2;
        visible_lines = (layout.content_h - canvas_h - margin - line_h) / line_h;
    }
    if (visible_lines < 1) visible_lines = 1;
    if (visible_lines > MAX_RESULT_LABELS) visible_lines = MAX_RESULT_LABELS;
}

static void draw_radar_base(void) {
    for (int i = 1; i <= 2; i++)
        api->ui_canvas_draw_arc(canvas, cx, cy, radar_r * i / 2, 0, 360, theme.text_muted, 1);

    ghostesp_point_t h[2] = {{cx - radar_r, cy}, {cx + radar_r, cy}};
    ghostesp_point_t v[2] = {{cx, cy - radar_r}, {cx, cy + radar_r}};
    api->ui_canvas_draw_line(canvas, h, 2, theme.text_muted, 1);
    api->ui_canvas_draw_line(canvas, v, 2, theme.text_muted, 1);
    api->ui_canvas_draw_arc(canvas, cx, cy, 3, 0, 360, theme.accent, 2);
}

static void draw_sweep(void) {
    int idx = sweep_step % 36;
    int ex = cx + (radar_r * trig_cos[idx]) / TRIG_SCALE;
    int ey = cy + (radar_r * trig_sin[idx]) / TRIG_SCALE;
    ghostesp_point_t sweep[2] = {{cx, cy}, {ex, ey}};
    api->ui_canvas_draw_line(canvas, sweep, 2, theme.accent, 2);
    sweep_step = (sweep_step + 1) % 36;
}

static void draw_devices(void) {
    for (int i = 0; i < scan_count; i++) {
        int dist = 60 - found[i].rssi;
        if (dist < 10) dist = 10;
        if (dist > 90) dist = 90;
        int idx = (i * 36) / (scan_count > 0 ? scan_count : 1);
        int scaled_r = (radar_r * dist) / 100;
        int dx = cx + (scaled_r * trig_cos[idx]) / TRIG_SCALE;
        int dy = cy + (scaled_r * trig_sin[idx]) / TRIG_SCALE;
        api->ui_canvas_draw_rect(canvas, dx - 2, dy - 2, 4, 4, theme.accent);
    }
}

static void redraw(void) {
    if (!canvas) return;
    api->ui_canvas_fill(canvas, theme.bg);
    draw_radar_base();
    draw_devices();
    draw_sweep();
}

static void update_result_labels(void) {
    if (!lbl_status || !api->ui_label_set_text) return;

    int total_lines = scan_count > 0 ? scan_count : 1;
    if (scroll_offset > total_lines - visible_lines) scroll_offset = total_lines - visible_lines;
    if (scroll_offset < 0) scroll_offset = 0;

    char buf[96];
    if (scan_in_progress) api->ui_label_set_text(lbl_status, "Scanning...");
    else if (scan_count == 0) api->ui_label_set_text(lbl_status, "Scanning for GhostNet / ESP32");
    else { snprintf(buf, sizeof(buf), "%d hit%s", scan_count, scan_count == 1 ? "" : "s"); api->ui_label_set_text(lbl_status, buf); }

    for (int i = 0; i < MAX_RESULT_LABELS; i++) {
        if (!result_labels[i]) continue;
        if (i >= visible_lines) { api->ui_label_set_text(result_labels[i], ""); continue; }
        int idx = scroll_offset + i;
        if (scan_count == 0 || idx >= scan_count) { api->ui_label_set_text(result_labels[i], ""); continue; }
        snprintf(buf, sizeof(buf), "%s  ch%u  %ddBm", found[idx].label, found[idx].channel, found[idx].rssi);
        api->ui_label_set_text(result_labels[i], buf);
    }
}

static void collect_scan_results(void) {
    scan_count = 0;
    int total = api->wifi_ap_count ? api->wifi_ap_count() : 0;

    for (int i = 0; i < total && scan_count < MAX_FOUND; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap || !api->wifi_scan_get_ap((uint16_t)i, &ap)) continue;
        bool match = false;
        if (ap.ssid[0] && strcmp(ap.ssid, "GhostNet") == 0) match = true;
        if (!match && is_espressif(ap.bssid)) match = true;
        if (!match) continue;

        found_device_t *d = &found[scan_count];
        memset(d, 0, sizeof(*d));
        if (ap.ssid[0]) {
            size_t len = strlen(ap.ssid);
            if (len >= sizeof(d->label)) len = sizeof(d->label) - 1;
            memcpy(d->label, ap.ssid, len); d->label[len] = '\0';
        } else {
            snprintf(d->label, sizeof(d->label), "%02X:%02X:%02X", ap.bssid[0], ap.bssid[1], ap.bssid[2]);
        }
        d->channel = ap.channel; d->rssi = ap.rssi;
        scan_count++;
    }

    if (api->rgb_set_all) api->rgb_set_all(scan_count ? 0 : 0, scan_count ? 32 : 0, 0);
    update_result_labels();
    redraw();
}

static void do_scan(void *user) {
    (void)user;
    if (scan_in_progress) return;
    if (api->wifi_start_scan_async && api->wifi_scan_check_done && api->wifi_finish_scan) {
        scan_in_progress = api->wifi_start_scan_async();
        if (scan_in_progress) { update_result_labels(); return; }
    }
    if (api->wifi_start_scan) api->wifi_start_scan();
    collect_scan_results();
    last_scan_ms = api->system_uptime_ms ? api->system_uptime_ms() : last_scan_ms + SCAN_INTERVAL_MS;
}

static void scan_timer_cb(void *user) {
    (void)user;
    if (scan_in_progress) {
        if (api->wifi_scan_check_done && !api->wifi_scan_check_done()) return;
        if (api->wifi_finish_scan) api->wifi_finish_scan();
        scan_in_progress = false;
        collect_scan_results();
        last_scan_ms = api->system_uptime_ms ? api->system_uptime_ms() : last_scan_ms + SCAN_INTERVAL_MS;
        return;
    }
    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : last_scan_ms + SCAN_INTERVAL_MS;
    if (last_scan_ms == 0 || now - last_scan_ms >= SCAN_INTERVAL_MS) do_scan(NULL);
}

static void sweep_timer_cb(void *user) { (void)user; redraw(); }

static void esp32_finder_start(void) {
    if (api->log) api->log("ESP32 Finder started");
    gh_theme_init(api, &theme);
    gh_layout_init(api, &layout);
    gh_touch_reset(&touch_state);

    sweep_step = 0; scan_count = 0; scroll_offset = 0;
    last_scan_ms = 0; scan_in_progress = false;

    calc_layout();
    int margin = layout.compact ? 2 : 4;

    screen = api->ui_screen_create(s_app_name);
    if (!screen) return;
    GH_VOID(api, ui_obj_set_pad, screen, 0, 0, 0, 0);
    GH_VOID(api, ui_obj_set_pad_column, screen, 2);
    GH_VOID(api, ui_obj_set_pad_row, screen, 2);
    GH_VOID(api, ui_obj_set_flex_flow, screen, layout.content_w > layout.content_h ? GHOSTESP_FLEX_FLOW_ROW : GHOSTESP_FLEX_FLOW_COLUMN);
    GH_VOID(api, ui_obj_set_scrollable, screen, false);

    canvas = api->ui_canvas_create(screen, canvas_w, canvas_h);
    if (!canvas) return;
    GH_VOID(api, ui_obj_set_size, canvas, canvas_w, canvas_h);

    info_panel = api->ui_card_create ? api->ui_card_create(screen) : screen;
    if (!info_panel) return;
    GH_VOID(api, ui_obj_set_bg_color, info_panel, theme.bg);
    GH_VOID(api, ui_obj_set_border_width, info_panel, 0);
    GH_VOID(api, ui_obj_set_radius, info_panel, 0);
    GH_VOID(api, ui_obj_set_pad, info_panel, 2, 2, 2, 2);
    GH_VOID(api, ui_obj_set_size, info_panel,
        layout.content_w > layout.content_h ? layout.content_w - canvas_w - 2 : layout.content_w,
        layout.content_w > layout.content_h ? layout.content_h : layout.content_h - canvas_h - 2);
    GH_VOID(api, ui_obj_set_flex_flow, info_panel, GHOSTESP_FLEX_FLOW_COLUMN);
    GH_VOID(api, ui_obj_set_pad_row, info_panel, 0);
    GH_VOID(api, ui_obj_set_scrollable, info_panel, false);

    int info_w = layout.content_w > layout.content_h ? layout.content_w - canvas_w - 8 : layout.content_w - 8;
    lbl_status = gh_label(api, info_panel, "Scanning for GhostNet / ESP32", theme.text, GHOSTESP_FONT_MICRO);
    if (lbl_status) GH_VOID(api, ui_obj_set_size, lbl_status, info_w, line_h);

    for (int i = 0; i < MAX_RESULT_LABELS; i++) {
        result_labels[i] = gh_label(api, info_panel, "", theme.text_muted, GHOSTESP_FONT_MICRO);
        if (result_labels[i]) GH_VOID(api, ui_obj_set_size, result_labels[i], info_w, line_h);
    }

    create_touch_controls();
    sweep_timer = api->ui_timer_create(sweep_timer_cb, SWEEP_INTERVAL_MS, NULL);
    scan_timer = api->ui_timer_create(scan_timer_cb, SCAN_POLL_INTERVAL_MS, NULL);
    redraw();
    do_scan(NULL);
}

static void esp32_finder_stop(void) {
    destroy_views();
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
    if (api->log) api->log("ESP32 Finder stopped");
}

static void scroll_results(int delta) {
    scroll_offset += delta;
    update_result_labels();
}

static void esp32_finder_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_RIGHT) { GH_VOID(api, app_exit); }
        else if (swipe == GHOSTESP_INPUT_DOWN || swipe == GHOSTESP_INPUT_UP) { do_scan(NULL); }
        return;
    }

    if (event->type == GHOSTESP_INPUT_BACK) { GH_VOID(api, app_exit); }
    else if (event->type == GHOSTESP_INPUT_SELECT) { do_scan(NULL); }
    else if (event->type == GHOSTESP_INPUT_UP) { scroll_results(-1); }
    else if (event->type == GHOSTESP_INPUT_DOWN) { scroll_results(1); }
    else if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == 27 || v == 8 || v == 127 || v == 'q' || v == 'Q') { GH_VOID(api, app_exit); }
        else if (v == 10 || v == 'r' || v == 'R' || v == ' ') { do_scan(NULL); }
        else if (v == 'w' || v == 'W') { scroll_results(-1); }
        else if (v == 's' || v == 'S') { scroll_results(1); }
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    s_app_id, s_app_name,
    esp32_finder_start, esp32_finder_stop, esp32_finder_input, NULL
);

GHOSTESP_APP_INIT_WITH_API(app, api, "esp32_finder", ESP32_FINDER_REQUIRED_API_SIZE)
