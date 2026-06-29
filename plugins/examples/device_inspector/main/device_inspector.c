#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static const ghostesp_api_t *api;

typedef enum {
    PAGE_MENU,
    PAGE_SYSTEM,
    PAGE_WIFI,
    PAGE_BLE,
    PAGE_RGB,
    PAGE_STORAGE,
    PAGE_STORAGE_TEST,
    PAGE_GPS,
    PAGE_HARDWARE,
    PAGE_CANVAS,
    PAGE_INPUT,
    PAGE_THEME,
    PAGE_UNSAFE,
} page_id_t;

static page_id_t current_page;
static ghostesp_options_t main_menu;
static ghostesp_options_t storage_menu;
static ghostesp_options_t ble_menu;
static ghostesp_detail_t detail_view;
static ghostesp_popup_t popup;
static ghostesp_scan_t scan_status;
static ghostesp_ui_timer_t rgb_timer;
static ghostesp_ui_timer_t canvas_timer;
static ghostesp_ui_obj_t touch_bar;
static ghostesp_ui_obj_t scroll_up_btn;
static ghostesp_ui_obj_t scroll_down_btn;
static ghostesp_ui_obj_t back_btn;
static int rgb_step;
static ghostesp_ui_obj_t canvas_obj;
static int canvas_tick;
static int touch_start_x;
static int touch_start_y;
static bool touch_started;
static char storage_path[256] = "/mnt/ghostesp";
static ghostesp_storage_entry_t storage_entries[32];
static int storage_count;
static ghostesp_ble_detect_info_t ble_entries[32];
static int ble_count;
static int ble_detail_index = -1;

static void show_menu(void);
static void open_page(page_id_t page);
static void open_storage(void);
static void open_ble(void);
static void storage_go_parent(void);
static void rgb_stop(void *user);
static void canvas_stop(void *user);

#define TOUCH_SWIPE_THRESHOLD 24

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct {
    const char *label;
    page_id_t page;
} menu_page_t;

static const menu_page_t s_menu_pages[] = {
    { "System Info", PAGE_SYSTEM },
    { "WiFi Scan", PAGE_WIFI },
    { "BLE Scan", PAGE_BLE },
    { "RGB Test", PAGE_RGB },
    { "Storage Browser", PAGE_STORAGE },
    { "Storage R/W Test", PAGE_STORAGE_TEST },
    { "GPS Status", PAGE_GPS },
    { "Hardware Info", PAGE_HARDWARE },
    { "Canvas Demo", PAGE_CANVAS },
    { "Input Tester", PAGE_INPUT },
    { "Theme Colors", PAGE_THEME },
    { "Unsafe Probe", PAGE_UNSAFE },
};

static void navigate_up(void);
static void navigate_down(void);
static void navigate_back(void);

static void touch_up_clicked(void *user) {
    (void)user;
    navigate_up();
}

static void touch_down_clicked(void *user) {
    (void)user;
    navigate_down();
}

static void touch_back_clicked(void *user) {
    (void)user;
    navigate_back();
}

static void destroy_touch_controls(void) {
    if (touch_bar && api->ui_obj_delete) api->ui_obj_delete(touch_bar);
    touch_bar = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
}

static bool has_touchscreen(void) {
    return api->ui_has_touchscreen ? api->ui_has_touchscreen() : api->ui_touch_bar_create != NULL;
}

static void create_touch_controls(bool show_scroll) {
    destroy_touch_controls();
    if (!has_touchscreen()) return;
    if (!api->ui_touch_bar_create) return;

    touch_bar = api->ui_touch_bar_create(NULL);
    if (!touch_bar) return;
    if (api->ui_touch_bar_add_up) scroll_up_btn = api->ui_touch_bar_add_up(touch_bar, touch_up_clicked, NULL);
    if (api->ui_touch_bar_add_back) back_btn = api->ui_touch_bar_add_back(touch_bar, touch_back_clicked, NULL);
    if (api->ui_touch_bar_add_down) scroll_down_btn = api->ui_touch_bar_add_down(touch_bar, touch_down_clicked, NULL);
    if (back_btn && api->ui_button_set_selected) api->ui_button_set_selected(back_btn, true);
    if (api->ui_obj_set_visible) {
        if (scroll_up_btn) api->ui_obj_set_visible(scroll_up_btn, show_scroll);
        if (scroll_down_btn) api->ui_obj_set_visible(scroll_down_btn, show_scroll);
    }
}

static void detail_back(void *user) {
    (void)user;
    if (detail_view && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail_view);
        detail_view = NULL;
    }
    show_menu();
}

static void storage_detail_back(void *user) {
    (void)user;
    if (detail_view && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail_view);
        detail_view = NULL;
    }
    open_storage();
}

static void destroy_subviews(void) {
    destroy_touch_controls();
    if (detail_view && api->ui_detail_destroy) {
        api->ui_detail_destroy(detail_view);
        detail_view = NULL;
    }
    if (storage_menu && api->ui_options_destroy) {
        api->ui_options_destroy(storage_menu);
        storage_menu = NULL;
    }
    if (ble_menu && api->ui_options_destroy) {
        api->ui_options_destroy(ble_menu);
        ble_menu = NULL;
    }
}

static void stop_canvas_timer(void) {
    if (canvas_timer && api->ui_timer_delete) {
        api->ui_timer_delete(canvas_timer);
        canvas_timer = NULL;
    }
    canvas_obj = NULL;
}

static void popup_close(void *user) {
    (void)user;
    if (popup && api->ui_popup_hide) api->ui_popup_hide(popup);
    if (popup && api->ui_popup_destroy) {
        api->ui_popup_destroy(popup);
        popup = NULL;
    }
}

static void show_popup(const char *title, const char *body) {
    if (!api->ui_popup_create) return;
    popup = api->ui_popup_create(260, 180);
    if (!popup) return;
    api->ui_popup_set_title(popup, title);
    api->ui_popup_set_body(popup, body);
    api->ui_popup_add_button(popup, "OK", popup_close, NULL);
    api->ui_popup_show(popup);
}

static void detail_add_section(const char *text) {
    if (api->ui_detail_add_info) api->ui_detail_add_info(detail_view, text, "");
}

static void detail_add_summary(const char *text) {
    if (api->ui_detail_add_info) api->ui_detail_add_info(detail_view, "Summary", text);
}

static void exit_app(void *user) {
    (void)user;
    if (api->app_exit) api->app_exit();
}

static void menu_select(void *user) {
    int idx = (int)(intptr_t)user;
    if (idx >= 0 && idx < ARRAY_COUNT(s_menu_pages)) {
        if (main_menu && api->ui_options_destroy) {
            api->ui_options_destroy(main_menu);
            main_menu = NULL;
        }
        open_page(s_menu_pages[idx].page);
    } else if (idx == ARRAY_COUNT(s_menu_pages)) {
        exit_app(NULL);
    }
}

static void show_menu(void) {
    current_page = PAGE_MENU;
    if (!api->ui_options_create) return;
    destroy_subviews();
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    main_menu = api->ui_options_create("Device Inspector");
    if (!main_menu) return;

    for (int i = 0; i < ARRAY_COUNT(s_menu_pages); i++) {
        api->ui_options_add_item(main_menu, s_menu_pages[i].label, menu_select, (void *)(intptr_t)i);
    }
    api->ui_options_add_back(main_menu, exit_app, NULL);
    create_touch_controls(true);
}

static void open_system(void) {
    current_page = PAGE_SYSTEM;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("System Info");
    if (!detail_view) return;

    char buf[64];
    const char *ver = api->system_firmware_version ? api->system_firmware_version() : "unknown";
    api->ui_detail_add_info(detail_view, "Firmware", ver ? ver : "unknown");
    api->ui_detail_add_info(detail_view, "Target", api->target ? api->target : "unknown");

    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)api->system_uptime_ms());
    api->ui_detail_add_info(detail_view, "Uptime", buf);

    snprintf(buf, sizeof(buf), "%lu bytes", (unsigned long)api->system_free_heap());
    api->ui_detail_add_info(detail_view, "Free Heap", buf);

    snprintf(buf, sizeof(buf), "%lu bytes", (unsigned long)api->system_free_internal_heap());
    api->ui_detail_add_info(detail_view, "Free Internal", buf);

    size_t used = api->app_memory_used ? api->app_memory_used() : 0;
    size_t limit = api->app_memory_limit ? api->app_memory_limit() : 0;
    snprintf(buf, sizeof(buf), "%lu / %lu", (unsigned long)used, (unsigned long)limit);
    api->ui_detail_add_info(detail_view, "App Memory", buf);

    const char *aid = api->app_id ? api->app_id() : "unknown";
    api->ui_detail_add_info(detail_view, "App ID", aid);

    const char *datapath = api->app_data_path ? api->app_data_path() : "unknown";
    api->ui_detail_add_info(detail_view, "Data Path", datapath);

    snprintf(buf, sizeof(buf), "%ld x %ld",
             (long)(api->ui_screen_get_width ? api->ui_screen_get_width() : 0),
             (long)(api->ui_screen_get_height ? api->ui_screen_get_height() : 0));
    api->ui_detail_add_info(detail_view, "Screen", buf);

    uint32_t flags = api->flags;
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)flags);
    api->ui_detail_add_info(detail_view, "Flags", buf);

    const char *devname = api->settings_get_device_name ? api->settings_get_device_name() : NULL;
    if (devname) api->ui_detail_add_info(detail_view, "Device Name", devname);

    uint8_t theme = api->settings_get_theme ? api->settings_get_theme() : 0;
    snprintf(buf, sizeof(buf), "%u", (unsigned)theme);
    api->ui_detail_add_info(detail_view, "Theme", buf);

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void wifi_scan_done(void *user) {
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("WiFi Scan Results");
    if (!detail_view) return;

    char buf[96];
    uint16_t count = api->wifi_ap_count ? api->wifi_ap_count() : 0;
    snprintf(buf, sizeof(buf), "%u APs found", (unsigned)count);
    detail_add_summary(buf);

    int show = count > 20 ? 20 : (int)count;
    for (int i = 0; i < show; i++) {
        ghostesp_wifi_ap_info_t ap;
        if (!api->wifi_scan_get_ap(i, &ap)) continue;
        snprintf(buf, sizeof(buf), "%s ch%d %ddBm", ap.ssid[0] ? ap.ssid : "(hidden)", ap.channel, ap.rssi);
        api->ui_detail_add_info(detail_view, "", buf);
    }
    if ((int)count > show) {
        snprintf(buf, sizeof(buf), "... and %u more", (unsigned)(count - show));
        api->ui_detail_add_info(detail_view, "", buf);
    }

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void open_wifi(void) {
    current_page = PAGE_WIFI;
    if (api->ui_scan_status_create) {
        scan_status = api->ui_scan_status_create("Scanning WiFi...");
        if (scan_status) api->ui_scan_status_set_progress(scan_status, 0, 1);
    }
    if (api->wifi_start_scan) api->wifi_start_scan();
    if (api->delay_ms) api->delay_ms(3000);
    wifi_scan_done(NULL);
}

static void ble_track_selected(void *user) {
    if (ble_detail_index >= 0 && api->ble_detect_start_tracking) {
        bool ok = api->ble_detect_start_tracking(ble_detail_index);
        if (!ok) show_popup("BLE Detect", "Unable to start tracking");
    }
}

static void ble_spoof_selected(void *user) {
    if (ble_detail_index >= 0 && api->ble_detect_start_airtag_spoof) {
        bool ok = api->ble_detect_start_airtag_spoof(ble_detail_index);
        if (!ok) show_popup("BLE Detect", "Unable to start AirTag spoof");
    }
}

static void ble_select(void *user) {
    int idx = (int)(intptr_t)user;
    if (idx < 0 || idx >= ble_count) return;
    if (ble_menu && api->ui_options_destroy) {
        api->ui_options_destroy(ble_menu);
        ble_menu = NULL;
    }
    if (!api->ui_detail_create) return;

    ble_detail_index = idx;
    ghostesp_ble_detect_info_t *dev = &ble_entries[idx];
    const char *type = api->ble_detect_type_name ? api->ble_detect_type_name(dev->type) : "BLE Device";
    detail_view = api->ui_detail_create(type);
    if (!detail_view) return;

    char buf[96];
    char mac[24];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);

    api->ui_detail_add_info(detail_view, "Type", type);
    api->ui_detail_add_info(detail_view, "MAC", mac);
    api->ui_detail_add_info(detail_view, "Name", dev->name[0] ? dev->name : "Unknown");
    if (dev->subtype[0]) api->ui_detail_add_info(detail_view, "Variant", dev->subtype);
    snprintf(buf, sizeof(buf), "%d dBm", dev->rssi);
    api->ui_detail_add_info(detail_view, "RSSI", buf);
    api->ui_detail_add_info(detail_view, "Tracking", dev->tracking ? "YES" : "NO");

    api->ui_detail_add_action(detail_view, "Track Signal", ble_track_selected, NULL);
    if (dev->type == 1) api->ui_detail_add_action(detail_view, "Spoof AirTag", ble_spoof_selected, NULL);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void ble_back(void *user) {
    (void)user;
    show_menu();
}

static void ble_activate_selected(void) {
    if (!ble_menu || !api->ui_options_get_selected) return;
    int selected = api->ui_options_get_selected(ble_menu);
    if (selected >= 0 && selected < ble_count) {
        ble_select((void *)(intptr_t)selected);
    } else {
        show_menu();
    }
}

static void open_ble(void) {
    current_page = PAGE_BLE;
    destroy_subviews();
    if (!api->ui_options_create) return;

    if (api->ui_scan_status_create) {
        scan_status = api->ui_scan_status_create("Detecting BLE Devices...");
        if (scan_status) api->ui_scan_status_set_progress(scan_status, 0, 1);
    }
    if (api->ble_detect_start) api->ble_detect_start();
    else if (api->ble_start_scan) api->ble_start_scan();
    if (api->delay_ms) api->delay_ms(4000);
    if (api->ble_detect_stop) api->ble_detect_stop();
    else if (api->ble_stop_scan) api->ble_stop_scan();
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }

    ble_menu = api->ui_options_create("BLE Detect Devices");
    if (!ble_menu) return;

    ble_count = 0;
    int count = api->ble_detect_count ? api->ble_detect_count() : 0;
    int show = count > ARRAY_COUNT(ble_entries) ? ARRAY_COUNT(ble_entries) : count;
    for (int i = 0; i < show; i++) {
        if (!api->ble_detect_get_device || !api->ble_detect_get_device(i, &ble_entries[ble_count])) continue;
        ghostesp_ble_detect_info_t *dev = &ble_entries[ble_count];
        const char *type = api->ble_detect_type_name ? api->ble_detect_type_name(dev->type) : "BLE";
        char label[128];
        snprintf(label, sizeof(label), "%s: %s %ddBm", type, dev->name[0] ? dev->name : "Unknown", dev->rssi);
        api->ui_options_add_item(ble_menu, label, ble_select, (void *)(intptr_t)ble_count);
        ble_count++;
    }
    if (ble_count == 0) api->ui_options_add_item(ble_menu, "No Flipper/AirTag/Skimmer devices", NULL, NULL);
    api->ui_options_add_back(ble_menu, ble_back, NULL);
    create_touch_controls(true);
}

static void rgb_timer_cb(void *user) {
    if (current_page != PAGE_RGB || !api->rgb_set_all) return;
    rgb_step++;
    uint8_t r = 0, g = 0, b = 0;
    switch (rgb_step % 8) {
        case 0: r = 255; break;
        case 1: g = 255; break;
        case 2: b = 255; break;
        case 3: r = 255; g = 255; break;
        case 4: g = 255; b = 255; break;
        case 5: r = 255; b = 255; break;
        case 6: r = 255; g = 255; b = 255; break;
        case 7: r = 0; g = 0; b = 0; break;
    }
    api->rgb_set_all(r, g, b);

    char msg[64];
    snprintf(msg, sizeof(msg), "Step %d: RGB(%d,%d,%d)", rgb_step % 8, r, g, b);
    if (api->ui_set_status) api->ui_set_status(msg);
}

static void rgb_stop(void *user) {
    if (rgb_timer && api->ui_timer_delete) {
        api->ui_timer_delete(rgb_timer);
        rgb_timer = NULL;
    }
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
    detail_back(NULL);
}

static void open_rgb(void) {
    current_page = PAGE_RGB;
    rgb_step = 0;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("RGB Test");
    if (!detail_view) return;

    detail_add_summary("Cycles through 8 colors automatically");

    char hex[16];
    uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFFFFF, 0x000000};
    const char *names[] = {"Red", "Green", "Blue", "Yellow", "Cyan", "Magenta", "White", "Off"};
    for (int i = 0; i < 8; i++) {
        snprintf(hex, sizeof(hex), "%s (#%06lX)", names[i], (unsigned long)colors[i]);
        api->ui_detail_add_info(detail_view, "", hex);
    }

    api->ui_detail_add_action(detail_view, "Stop Test", rgb_stop, NULL);
    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);

    if (api->ui_timer_create) {
        rgb_timer = api->ui_timer_create(rgb_timer_cb, 500, NULL);
    }
}

static bool storage_is_root(void) {
    return strcmp(storage_path, "/mnt/ghostesp") == 0;
}

static void storage_go_parent(void) {
    if (storage_is_root()) {
        show_menu();
        return;
    }
    char *slash = strrchr(storage_path, '/');
    if (!slash || slash <= storage_path + strlen("/mnt/ghostesp")) {
        strcpy(storage_path, "/mnt/ghostesp");
    } else {
        *slash = '\0';
    }
    open_storage();
}

static void storage_back(void *user) {
    (void)user;
    storage_go_parent();
}

static void storage_open_file(int idx) {
    if (idx < 0 || idx >= storage_count) return;
    if (storage_menu && api->ui_options_destroy) {
        api->ui_options_destroy(storage_menu);
        storage_menu = NULL;
    }
    if (!api->ui_detail_create) return;

    detail_view = api->ui_detail_create(storage_entries[idx].name);
    if (!detail_view) return;

    char full_path[320];
    snprintf(full_path, sizeof(full_path), "%s/%s", storage_path, storage_entries[idx].name);
    api->ui_detail_add_info(detail_view, "Type", storage_entries[idx].is_directory ? "Directory" : "File");
    api->ui_detail_add_info(detail_view, "Path", full_path);
    if (!storage_entries[idx].is_directory && api->storage_read) {
        char preview[129] = {0};
        int n = api->storage_read(full_path, preview, sizeof(preview) - 1);
        if (n >= 0) {
            for (int i = 0; i < n; i++) {
                if ((unsigned char)preview[i] < 32 && preview[i] != '\n' && preview[i] != '\r' && preview[i] != '\t') preview[i] = '.';
            }
            api->ui_detail_add_info(detail_view, "Preview", n > 0 ? preview : "Empty file");
        }
    }

    api->ui_detail_add_back(detail_view, storage_detail_back, NULL);
    create_touch_controls(true);
}

static void storage_select(void *user) {
    int encoded = (int)(intptr_t)user;
    if (encoded == 0) {
        storage_go_parent();
        return;
    }
    int idx = encoded - 1;
    if (idx < 0 || idx >= storage_count) return;
    if (storage_entries[idx].is_directory) {
        size_t len = strlen(storage_path);
        size_t name_len = strlen(storage_entries[idx].name);
        if (len + 1 + name_len < sizeof(storage_path)) {
            storage_path[len] = '/';
            memcpy(storage_path + len + 1, storage_entries[idx].name, name_len + 1);
        }
        open_storage();
    } else {
        storage_open_file(idx);
    }
}

static void storage_activate_selected(void) {
    if (!storage_menu || !api->ui_options_get_selected) return;
    int selected = api->ui_options_get_selected(storage_menu);
    int entry_start = storage_is_root() ? 0 : 1;
    if (!storage_is_root() && selected == 0) {
        storage_go_parent();
    } else if (selected >= entry_start && selected < entry_start + storage_count) {
        storage_select((void *)(intptr_t)(selected - entry_start + 1));
    } else {
        storage_go_parent();
    }
}

static void open_storage(void) {
    current_page = PAGE_STORAGE;
    destroy_subviews();
    if (!api->ui_options_create) return;

    char title[80];
    snprintf(title, sizeof(title), "Storage: %s", storage_is_root() ? "/" : strrchr(storage_path, '/') + 1);
    storage_menu = api->ui_options_create(title);
    if (!storage_menu) return;

    if (!storage_is_root()) api->ui_options_add_item(storage_menu, "..", storage_select, (void *)(intptr_t)0);

    storage_count = api->storage_list ? api->storage_list(storage_path, storage_entries, ARRAY_COUNT(storage_entries)) : -1;
    if (storage_count < 0) {
        storage_count = 0;
        api->ui_options_add_item(storage_menu, "Unable to list SD card", NULL, NULL);
    } else if (storage_count == 0) {
        api->ui_options_add_item(storage_menu, "Empty directory", NULL, NULL);
    } else {
        for (int i = 0; i < storage_count; i++) {
            char label[96];
            snprintf(label, sizeof(label), "%s %.88s", storage_entries[i].is_directory ? "[D]" : "[F]", storage_entries[i].name);
            api->ui_options_add_item(storage_menu, label, storage_select, (void *)(intptr_t)(i + 1));
        }
    }
    api->ui_options_add_back(storage_menu, storage_back, NULL);
    create_touch_controls(true);
}

static void open_storage_test(void) {
    current_page = PAGE_STORAGE_TEST;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Storage R/W Test");
    if (!detail_view) return;

    char buf[96];
    char readback[128] = {0};
    const char *path = "test_rw.txt";
    const char *payload = "GhostESP storage test OK";

    detail_add_summary("Write / Read / Delete cycle");

    bool ok = api->app_storage_write && api->app_storage_write(path, payload, strlen(payload));
    snprintf(buf, sizeof(buf), "Write: %s", ok ? "OK" : "FAIL");
    api->ui_detail_add_info(detail_view, "1. Write", ok ? "OK" : "FAIL");

    int n = api->app_storage_read ? api->app_storage_read(path, readback, sizeof(readback) - 1) : -1;
    if (n > 0) readback[n] = '\0';
    api->ui_detail_add_info(detail_view, "2. Read", n > 0 ? "OK" : "FAIL");
    if (n > 0) {
        api->ui_detail_add_info(detail_view, "   Content", readback);
    }

    bool match = (n > 0 && strncmp(readback, payload, strlen(payload)) == 0);
    api->ui_detail_add_info(detail_view, "3. Verify", match ? "MATCH" : "MISMATCH");

    bool appended = api->app_storage_append && api->app_storage_append(path, " + appended", 10);
    api->ui_detail_add_info(detail_view, "4. Append", appended ? "OK" : "FAIL");

    bool exists = api->app_storage_exists && api->app_storage_exists(path);
    api->ui_detail_add_info(detail_view, "5. Exists", exists ? "YES" : "NO");

    bool deleted = api->app_storage_delete && api->app_storage_delete(path);
    api->ui_detail_add_info(detail_view, "6. Delete", deleted ? "OK" : "FAIL");

    bool gone = api->app_storage_exists && !api->app_storage_exists(path);
    api->ui_detail_add_info(detail_view, "7. Gone", gone ? "YES" : "NO - still there!");

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void open_gps(void) {
    current_page = PAGE_GPS;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("GPS Status");
    if (!detail_view) return;

    bool avail = api->gps_is_available && api->gps_is_available();
    api->ui_detail_add_info(detail_view, "Available", avail ? "YES" : "NO");

    if (avail) {
        bool fix = api->gps_has_fix && api->gps_has_fix();
        api->ui_detail_add_info(detail_view, "Fix", fix ? "YES" : "NO");

        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", api->gps_get_latitude ? api->gps_get_latitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Latitude", buf);

        snprintf(buf, sizeof(buf), "%.6f", api->gps_get_longitude ? api->gps_get_longitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Longitude", buf);

        snprintf(buf, sizeof(buf), "%.1f m", api->gps_get_altitude ? api->gps_get_altitude() : 0.0);
        api->ui_detail_add_info(detail_view, "Altitude", buf);

        snprintf(buf, sizeof(buf), "%d", api->gps_get_satellites ? api->gps_get_satellites() : 0);
        api->ui_detail_add_info(detail_view, "Satellites", buf);

        snprintf(buf, sizeof(buf), "%.1f km/h", api->gps_get_speed ? api->gps_get_speed() : 0.0f);
        api->ui_detail_add_info(detail_view, "Speed", buf);

        snprintf(buf, sizeof(buf), "%.1f deg", api->gps_get_heading ? api->gps_get_heading() : 0.0f);
        api->ui_detail_add_info(detail_view, "Heading", buf);
    } else {
        api->ui_detail_add_info(detail_view, "", "No GPS hardware detected");
    }

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void open_hardware(void) {
    current_page = PAGE_HARDWARE;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Hardware Info");
    if (!detail_view) return;

    char buf[64];

    detail_add_section("Radio Hardware");
    api->ui_detail_add_info(detail_view, "WiFi", api->wifi_start_scan ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "BLE", api->ble_start_scan ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "NFC", api->nfc_is_available && api->nfc_is_available() ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "SubGHz", api->subghz_is_available && api->subghz_is_available() ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "IR", api->ir_send_file ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "BadUSB", api->badusb_run_script ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "RGB LED", api->rgb_set_all ? "Present" : "N/A");
    api->ui_detail_add_info(detail_view, "GPS", api->gps_is_available && api->gps_is_available() ? "Present" : "N/A");

    detail_add_section("Display");
    int32_t w = api->ui_screen_get_width ? api->ui_screen_get_width() : 0;
    int32_t h = api->ui_screen_get_height ? api->ui_screen_get_height() : 0;
    snprintf(buf, sizeof(buf), "%ld x %ld px", (long)w, (long)h);
    api->ui_detail_add_info(detail_view, "Resolution", buf);

    if (api->ui_theme_is_bright) {
        api->ui_detail_add_info(detail_view, "Theme Mode", api->ui_theme_is_bright() ? "Bright" : "Dark");
    }

    detail_add_section("Memory");
    snprintf(buf, sizeof(buf), "%lu bytes free", (unsigned long)(api->system_free_heap ? api->system_free_heap() : 0));
    api->ui_detail_add_info(detail_view, "Heap", buf);
    snprintf(buf, sizeof(buf), "%lu bytes free", (unsigned long)(api->system_free_internal_heap ? api->system_free_internal_heap() : 0));
    api->ui_detail_add_info(detail_view, "Internal", buf);

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void canvas_tick_cb(void *user) {
    (void)user;
    if (current_page != PAGE_CANVAS || !canvas_obj) return;
    if (!api->ui_canvas_fill || !api->ui_canvas_draw_rect) return;

    canvas_tick++;
    int32_t w = api->ui_obj_get_width ? api->ui_obj_get_width(canvas_obj) : 200;
    int32_t h = api->ui_obj_get_height ? api->ui_obj_get_height(canvas_obj) : 100;
    if (w < 40) w = 40;
    if (h < 40) h = 40;

    api->ui_canvas_fill(canvas_obj, api->ui_theme_get_background ? api->ui_theme_get_background() : 0x000000);

    uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x1976D2;
    uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xFFFFFF;
    uint32_t surface = api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0x1A1A1A;

    int cx = w / 2;
    int cy = h / 2;
    int max_r = (w < h ? w : h) / 2 - 10;
    if (max_r < 10) max_r = 10;
    int radius = max_r * ((canvas_tick % 60) + 1) / 60;

    if (api->ui_canvas_draw_arc) {
        api->ui_canvas_draw_arc(canvas_obj, cx, cy, radius > 5 ? radius : 5, 0, 360, accent, 2);
    }

    int bars = 8;
    int bar_w = (w - 20) / bars;
    for (int i = 0; i < bars; i++) {
        int bh = ((canvas_tick * 3 + i * 37) % (h - 20)) + 5;
        uint32_t c = (i % 2 == 0) ? accent : surface;
        api->ui_canvas_draw_rect(canvas_obj, 10 + i * bar_w, h - bh - 5, bar_w - 2, bh, c);
    }

    int dots = 6;
    static const int8_t orbit_x[6] = {100, 50, -50, -100, -50, 50};
    static const int8_t orbit_y[6] = {0, 87, 87, 0, -87, -87};
    int orbit = max_r * 7 / 10;
    int phase = (canvas_tick / 4) % dots;
    for (int i = 0; i < dots; i++) {
        int p = (i + phase) % dots;
        int px = cx + orbit * orbit_x[p] / 100;
        int py = cy + orbit * orbit_y[p] / 100;
        api->ui_canvas_draw_rect(canvas_obj, px - 2, py - 2, 5, 5, text);
    }
}

static void canvas_stop(void *user) {
    (void)user;
    stop_canvas_timer();
    detail_back(NULL);
}

static void open_canvas(void) {
    stop_canvas_timer();
    current_page = PAGE_CANVAS;
    canvas_tick = 0;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Canvas Demo");
    if (!detail_view) return;

    detail_add_summary("Animated drawing on canvas widget");

    if (!api->ui_canvas_create || !api->ui_canvas_fill || !api->ui_canvas_draw_rect) {
        api->ui_detail_add_info(detail_view, "Canvas", "API unavailable");
    } else {
        int32_t sw = api->ui_screen_get_width ? api->ui_screen_get_width() : 240;
        if (sw < 40) sw = 240;
        int32_t ch = 100;
        canvas_obj = api->ui_canvas_create(NULL, sw - 20, ch);
        if (!canvas_obj) {
            api->ui_detail_add_info(detail_view, "Canvas", "Create failed");
        }
    }

    api->ui_detail_add_action(detail_view, "Stop", canvas_stop, NULL);
    api->ui_detail_add_back(detail_view, canvas_stop, NULL);
    create_touch_controls(true);

    if (canvas_obj && api->ui_timer_create) {
        canvas_timer = api->ui_timer_create(canvas_tick_cb, 50, NULL);
    }
}

static void open_input(void) {
    current_page = PAGE_INPUT;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Input Tester");
    if (!detail_view) return;

    detail_add_summary("Press any button to see events");
    api->ui_detail_add_info(detail_view, "", "Waiting for input...");
    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void open_theme(void) {
    current_page = PAGE_THEME;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Theme Colors");
    if (!detail_view) return;

    char buf[16];
    uint32_t colors[] = {
        api->ui_theme_get_background ? api->ui_theme_get_background() : 0,
        api->ui_theme_get_surface ? api->ui_theme_get_surface() : 0,
        api->ui_theme_get_surface_alt ? api->ui_theme_get_surface_alt() : 0,
        api->ui_theme_get_text ? api->ui_theme_get_text() : 0,
        api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0,
        api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0,
    };
    const char *names[] = {"Background", "Surface", "Surface Alt", "Text", "Text Muted", "Accent"};

    detail_add_section("Current theme palette");
    for (int i = 0; i < 6; i++) {
        snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)colors[i]);
        api->ui_detail_add_info(detail_view, names[i], buf);
    }

    if (api->ui_theme_is_bright) {
        api->ui_detail_add_info(detail_view, "Mode", api->ui_theme_is_bright() ? "Bright" : "Dark");
    }

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void open_unsafe(void) {
    current_page = PAGE_UNSAFE;
    if (!api->ui_detail_create) return;
    detail_view = api->ui_detail_create("Native Probe");
    if (!detail_view) return;

    char buf[64];
    detail_add_section("Raw LVGL pointers");

    void *scr = api->lv_scr_act ? api->lv_scr_act() : NULL;
    snprintf(buf, sizeof(buf), "%p", scr);
    api->ui_detail_add_info(detail_view, "lv_scr_act", buf);

    void *view = api->display_get_current_view ? api->display_get_current_view() : NULL;
    snprintf(buf, sizeof(buf), "%p", view);
    api->ui_detail_add_info(detail_view, "Current View", buf);

    void *sym = api->raw_symbol ? api->raw_symbol("lv_scr_act") : NULL;
    snprintf(buf, sizeof(buf), "%p", sym);
    api->ui_detail_add_info(detail_view, "raw_symbol test", buf);

    api->ui_detail_add_back(detail_view, detail_back, NULL);
    create_touch_controls(true);
}

static void navigate_up(void) {
    if (current_page == PAGE_MENU && main_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, -1);
        return;
    }
    if (current_page == PAGE_STORAGE && storage_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(storage_menu, -1);
        return;
    }
    if (current_page == PAGE_BLE && ble_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(ble_menu, -1);
        return;
    }
    if (detail_view) {
        if (api->ui_detail_step_up) api->ui_detail_step_up(detail_view);
        else if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, -1);
    }
}

static void navigate_down(void) {
    if (current_page == PAGE_MENU && main_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, 1);
        return;
    }
    if (current_page == PAGE_STORAGE && storage_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(storage_menu, 1);
        return;
    }
    if (current_page == PAGE_BLE && ble_menu) {
        if (api->ui_options_move_selection) api->ui_options_move_selection(ble_menu, 1);
        return;
    }
    if (detail_view) {
        if (api->ui_detail_step_down) api->ui_detail_step_down(detail_view);
        else if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, 1);
    }
}

static void navigate_back(void) {
    if (popup) {
        popup_close(NULL);
        return;
    }
    if (current_page == PAGE_MENU) {
        exit_app(NULL);
        return;
    }
    if (current_page == PAGE_RGB && detail_view) {
        rgb_stop(NULL);
        return;
    }
    if (current_page == PAGE_CANVAS && detail_view) {
        canvas_stop(NULL);
        return;
    }
    if (current_page == PAGE_STORAGE) {
        if (detail_view) storage_detail_back(NULL);
        else storage_go_parent();
        return;
    }
    if (current_page == PAGE_BLE) {
        if (detail_view) detail_back(NULL);
        else show_menu();
        return;
    }
    if (detail_view) {
        detail_back(NULL);
        return;
    }
    show_menu();
}

static bool handle_touch_navigation(const ghostesp_input_event_t *event) {
    if (!event || event->type != GHOSTESP_INPUT_TOUCH) return false;

    if (event->pressed) {
        touch_started = true;
        touch_start_x = event->x;
        touch_start_y = event->y;
        return true;
    }
    if (!touch_started) return true;

    touch_started = false;
    int dx = event->x - touch_start_x;
    int dy = event->y - touch_start_y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;

    if (abs_dy >= TOUCH_SWIPE_THRESHOLD && abs_dy >= abs_dx) {
        if (dy < 0) navigate_down();
        else navigate_up();
        return true;
    }
    if (abs_dx >= TOUCH_SWIPE_THRESHOLD && abs_dx > abs_dy && dx > 0) {
        navigate_back();
        return true;
    }
    return true;
}

static void open_page(page_id_t page) {
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    destroy_touch_controls();
    switch (page) {
        case PAGE_SYSTEM: open_system(); break;
        case PAGE_WIFI: open_wifi(); break;
        case PAGE_BLE: open_ble(); break;
        case PAGE_RGB: open_rgb(); break;
        case PAGE_STORAGE: open_storage(); break;
        case PAGE_STORAGE_TEST: open_storage_test(); break;
        case PAGE_GPS: open_gps(); break;
        case PAGE_HARDWARE: open_hardware(); break;
        case PAGE_CANVAS: open_canvas(); break;
        case PAGE_INPUT: open_input(); break;
        case PAGE_THEME: open_theme(); break;
        case PAGE_UNSAFE: open_unsafe(); break;
        default: show_menu(); break;
    }
}

static void inspector_start(void) {
    api->log("device_inspector started");
    touch_started = false;
    show_menu();
}

static void inspector_stop(void) {
    if (rgb_timer && api->ui_timer_delete) {
        api->ui_timer_delete(rgb_timer);
        rgb_timer = NULL;
    }
    stop_canvas_timer();
    if (api->rgb_set_all) api->rgb_set_all(0, 0, 0);
    if (api->ble_detect_stop) api->ble_detect_stop();
    else if (api->ble_stop_scan) api->ble_stop_scan();
    destroy_subviews();
    if (main_menu && api->ui_options_destroy) {
        api->ui_options_destroy(main_menu);
        main_menu = NULL;
    }
    if (popup && api->ui_popup_destroy) {
        api->ui_popup_destroy(popup);
        popup = NULL;
    }
    if (scan_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(scan_status);
        scan_status = NULL;
    }
    touch_started = false;
    api->log("device_inspector stopped");
}

static void inspector_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    if (current_page == PAGE_INPUT && detail_view && api->ui_detail_clear) {
        const char *type_name = "UNKNOWN";
        switch (event->type) {
            case GHOSTESP_INPUT_LEFT: type_name = "LEFT"; break;
            case GHOSTESP_INPUT_RIGHT: type_name = "RIGHT"; break;
            case GHOSTESP_INPUT_UP: type_name = "UP"; break;
            case GHOSTESP_INPUT_DOWN: type_name = "DOWN"; break;
            case GHOSTESP_INPUT_SELECT: type_name = "SELECT"; break;
            case GHOSTESP_INPUT_BACK: type_name = "BACK"; break;
            case GHOSTESP_INPUT_KEY: type_name = "KEY"; break;
            case GHOSTESP_INPUT_TOUCH: type_name = "TOUCH"; break;
            default: break;
        }

        api->ui_detail_clear(detail_view);
        detail_add_summary("Input Event Received");

        char buf[64];
        api->ui_detail_add_info(detail_view, "Type", type_name);

        snprintf(buf, sizeof(buf), "%ld", (long)event->value);
        api->ui_detail_add_info(detail_view, "Value", buf);

        snprintf(buf, sizeof(buf), "%ld, %ld", (long)event->x, (long)event->y);
        api->ui_detail_add_info(detail_view, "Position", buf);

        api->ui_detail_add_info(detail_view, "Pressed", event->pressed ? "YES" : "NO");

        api->ui_detail_add_back(detail_view, detail_back, NULL);

        if (event->type == GHOSTESP_INPUT_BACK) {
            detail_back(NULL);
            return;
        }
        return;
    }

    if (handle_touch_navigation(event)) return;

    if (current_page == PAGE_MENU && main_menu) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(main_menu, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            int idx = api->ui_options_get_selected ? api->ui_options_get_selected(main_menu) : -1;
            menu_select((void *)(intptr_t)idx);
            return;
        }
    }

    if (current_page == PAGE_STORAGE && storage_menu) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(storage_menu, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(storage_menu, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            storage_activate_selected();
            return;
        }
    }

    if (current_page == PAGE_BLE && ble_menu) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(ble_menu, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_options_move_selection) api->ui_options_move_selection(ble_menu, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            ble_activate_selected();
            return;
        }
    }

    if (detail_view) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            if (api->ui_detail_step_up) api->ui_detail_step_up(detail_view);
            else if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            if (api->ui_detail_step_down) api->ui_detail_step_down(detail_view);
            else if (api->ui_detail_move_selection) api->ui_detail_move_selection(detail_view, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            if (api->ui_detail_activate_selected) {
                api->ui_detail_activate_selected(detail_view);
            } else {
                int selected = api->ui_detail_get_selected ? api->ui_detail_get_selected(detail_view) : -1;
                int count = api->ui_detail_get_count ? api->ui_detail_get_count(detail_view) : 0;
                if (current_page == PAGE_RGB && selected == count - 2) {
                    rgb_stop(NULL);
                } else if (current_page == PAGE_CANVAS && selected == count - 2) {
                    canvas_stop(NULL);
                } else if (current_page == PAGE_STORAGE) {
                    storage_detail_back(NULL);
                } else if (current_page == PAGE_BLE) {
                    detail_back(NULL);
                } else {
                    detail_back(NULL);
                }
            }
            return;
        }
    }

    if (event->type == GHOSTESP_INPUT_BACK) {
        navigate_back();
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_APP_STRUCT_SIZE_V1,
    .id = "device_inspector",
    .name = "Device Inspector",
    .on_start = inspector_start,
    .on_stop = inspector_stop,
    .on_input = inspector_input,
};

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < GHOSTESP_API_STRUCT_SIZE_V1) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {}
