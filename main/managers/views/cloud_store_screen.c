#include "managers/views/cloud_store_screen.h"

#include "gui/asset_pack.h"
#include "gui/lvgl_safe.h"
#include "gui/options_view.h"
#include "gui/popup.h"
#include "gui/progress_bar_view.h"
#include "gui/scan_status.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/toast.h"
#include "managers/cloud_store_manager.h"
#include "managers/display_manager.h"
#include "managers/plugin_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t theme_palette_get_accent(uint8_t theme);

typedef enum {
    CLOUD_ITEM_NEW = 0,
    CLOUD_ITEM_INSTALLED,
    CLOUD_ITEM_UPDATE,
} cloud_item_install_status_t;

#define CLOUD_STORE_MAX_ROWS 24

typedef struct {
    int original_index;
    char name[CLOUD_STORE_NAME_MAX];
} sorted_cloud_item_t;

// Per-row descriptor so navigation and activation work off the actually-rendered
// rows instead of fragile index-range arithmetic (which desynced from the list
// whenever section counts and rendered rows disagreed, making some rows — e.g.
// asset packs — impossible to highlight or select).
typedef enum {
    ROW_KIND_NONE = 0, // unused slot
    ROW_KIND_REFRESH,
    ROW_KIND_FOLDER, // opens a category submenu (Apps / Asset Packs)
    ROW_KIND_ITEM,
    ROW_KIND_EMPTY,
    ROW_KIND_BACK,
} row_kind_t;

typedef struct {
    row_kind_t kind;
    cloud_store_item_type_t item_type;
    int original_index; // index into the manager's items of item_type
} row_meta_t;

// -1 = top-level menu (Refresh, folders, Back); otherwise a
// cloud_store_item_type_t selecting which category submenu is open.
#define CLOUD_SECTION_TOP (-1)

static lv_obj_t *s_root;
static options_view_t *s_options;
static lv_obj_t *s_rows[CLOUD_STORE_MAX_ROWS];
static row_meta_t s_row_meta[CLOUD_STORE_MAX_ROWS];
static lv_timer_t *s_status_timer;
static progress_bar_view_t *s_progress;
static scan_status_t *s_loading; // full-screen loading overlay while fetching
static popup_confirm_t *s_confirm_popup;
static bool s_apps_available;
static int s_selected_row;
static int s_section = CLOUD_SECTION_TOP; // which submenu is open, or top level
static uint32_t s_last_refresh_ms;
static cloud_store_state_t s_last_status_state;
static char s_pending_id[CLOUD_STORE_ID_MAX];
static cloud_store_item_type_t s_pending_type;
static sorted_cloud_item_t s_sorted_apps[CLOUD_STORE_MAX_ITEMS];
static int s_sorted_apps_count;
static sorted_cloud_item_t s_sorted_packs[CLOUD_STORE_MAX_ITEMS];
static int s_sorted_packs_count;

static int row_count(void) {
    return options_view_get_item_count(s_options);
}

static void update_selection(void) {
    int count = row_count();
    if (count <= 0) return;
    if (s_selected_row < 0) s_selected_row = 0;
    if (s_selected_row >= count) s_selected_row = count - 1;
    options_view_set_selected(s_options, s_selected_row);
}

// Move one row in the travel direction, wrapping at the ends. The two-level
// layout has no non-selectable separator rows, so every row is a valid stop.
static void move_selection(int direction) {
    int count = row_count();
    if (count <= 0) return;
    int row = (s_selected_row + direction) % count;
    if (row < 0) row += count;
    s_selected_row = row;
    options_view_set_selected(s_options, s_selected_row);
}

static void render_store(void);

static void row_event_cb(lv_event_t *e) {
    intptr_t row = (intptr_t)lv_event_get_user_data(e);
    s_selected_row = (int)row;
    update_selection();
    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK };
    ev.data.joystick_index = 1;
    if (cloud_store_view.input_callback) cloud_store_view.input_callback(&ev);
}

static int sort_items_compare(const void *a, const void *b) {
    const sorted_cloud_item_t *sa = (const sorted_cloud_item_t *)a;
    const sorted_cloud_item_t *sb = (const sorted_cloud_item_t *)b;
    return strcmp(sa->name, sb->name);
}

static void load_sorted_items(cloud_store_item_type_t type, sorted_cloud_item_t *out, int *out_count) {
    int item_count = cloud_store_get_count(type);
    int n = 0;
    for (int i = 0; i < item_count && n < CLOUD_STORE_MAX_ITEMS; ++i) {
        cloud_store_item_t item;
        if (!cloud_store_get_item(type, i, &item)) continue;
        out[n].original_index = i;
        strncpy(out[n].name, item.name, CLOUD_STORE_NAME_MAX - 1);
        out[n].name[CLOUD_STORE_NAME_MAX - 1] = '\0';
        n++;
    }
    qsort(out, n, sizeof(sorted_cloud_item_t), sort_items_compare);
    *out_count = n;
}

// Best-effort "is this already on the device?" check so the list can flag
// installed items and offered updates. Apps live in the plugin manager (with a
// version we can diff); asset packs are bare <id>.gtheme files with no on-disk
// version, so they can only be "installed" or not.
static cloud_item_install_status_t item_install_status(const cloud_store_item_t *item) {
    if (item->type == CLOUD_STORE_TYPE_APP) {
        const plugin_app_manifest_t *inst = plugin_manager_find(item->id);
        if (!inst) return CLOUD_ITEM_NEW;
        if (item->version[0] && inst->version[0] && strcmp(item->version, inst->version) != 0) {
            return CLOUD_ITEM_UPDATE;
        }
        return CLOUD_ITEM_INSTALLED;
    }
    int n = asset_pack_get_installed_count();
    for (int i = 0; i < n; ++i) {
        const char *name = asset_pack_get_installed_name(i);
        if (name && strcmp(name, item->id) == 0) return CLOUD_ITEM_INSTALLED;
    }
    return CLOUD_ITEM_NEW;
}

// Recolor-markup prefix (options_view labels have recolor enabled) that turns
// into a green check for installed items and an amber refresh glyph for updates.
static const char *item_status_prefix(cloud_item_install_status_t status) {
    switch (status) {
        case CLOUD_ITEM_INSTALLED: return "#3ddc84 " LV_SYMBOL_OK "#  ";
        case CLOUD_ITEM_UPDATE:    return "#ffb300 " LV_SYMBOL_REFRESH "#  ";
        default:                   return "    ";
    }
}

// Full-screen loading overlay while the manifest is fetching — same backgrounded
// spinner used by the AP-scan flow, rather than a bare arc floating over the menu.
static void update_spinner(cloud_store_state_t state) {
    bool want = (state == CLOUD_STORE_STATE_FETCHING);
    if (want && !s_loading) {
        s_loading = scan_status_create("Loading cloud store...");
    } else if (!want && s_loading) {
        scan_status_close(s_loading);
        s_loading = NULL;
    }
}

static void add_folder_row(int *rendered, cloud_store_item_type_t type, const char *label) {
    int count = cloud_store_get_count(type);
    char line[64];
    snprintf(line, sizeof(line), LV_SYMBOL_DIRECTORY "  %s (%d)", label, count);
    s_rows[*rendered] = options_view_add_item(s_options, line, row_event_cb, (void *)(intptr_t)*rendered);
    s_row_meta[*rendered].kind = ROW_KIND_FOLDER;
    s_row_meta[*rendered].item_type = type;
    (*rendered)++;
}

static void add_item_row(int *rendered, cloud_store_item_type_t type, int original_index) {
    cloud_store_item_t item;
    if (!cloud_store_get_item(type, original_index, &item)) return;

    const char *prefix = item_status_prefix(item_install_status(&item));
    char line[CLOUD_STORE_NAME_MAX + 48];
    if (item.version[0]) {
        snprintf(line, sizeof(line), "%s%s  v%s", prefix, item.name, item.version);
    } else {
        snprintf(line, sizeof(line), "%s%s", prefix, item.name);
    }
    s_rows[*rendered] = options_view_add_item(s_options, line, row_event_cb, (void *)(intptr_t)*rendered);
    s_row_meta[*rendered].kind = ROW_KIND_ITEM;
    s_row_meta[*rendered].item_type = type;
    s_row_meta[*rendered].original_index = original_index;
    (*rendered)++;
}

// Top-level menu: Refresh, a folder per available category, and Back (exits).
static void render_top_level(int *rendered, cloud_store_status_t status) {
    options_view_set_title(s_options, "Cloud Store");

    char refresh_buf[CLOUD_STORE_ERROR_MAX + 40];
    const char *refresh_label = LV_SYMBOL_REFRESH "  Refresh cloud list";
    if (status.state == CLOUD_STORE_STATE_FETCHING) {
        refresh_label = LV_SYMBOL_REFRESH "  Loading cloud manifest...";
    } else if (status.state == CLOUD_STORE_STATE_FAILED &&
               cloud_store_get_count(CLOUD_STORE_TYPE_APP) == 0 &&
               cloud_store_get_count(CLOUD_STORE_TYPE_ASSET_PACK) == 0) {
        // Make it obvious this row is a tappable retry, not just an error label.
        snprintf(refresh_buf, sizeof(refresh_buf), "#ff5555 " LV_SYMBOL_WARNING "#  %s  (tap to retry)",
                 status.error[0] ? status.error : "Cloud manifest unavailable");
        refresh_label = refresh_buf;
    }
    s_rows[*rendered] = options_view_add_item(s_options, refresh_label, row_event_cb, (void *)(intptr_t)*rendered);
    s_row_meta[*rendered].kind = ROW_KIND_REFRESH;
    (*rendered)++;

    if (s_apps_available) {
        add_folder_row(rendered, CLOUD_STORE_TYPE_APP, "Apps");
    }
    add_folder_row(rendered, CLOUD_STORE_TYPE_ASSET_PACK, "Asset Packs");

    s_rows[*rendered] = options_view_add_back_row(s_options, row_event_cb, (void *)(intptr_t)*rendered);
    s_row_meta[*rendered].kind = ROW_KIND_BACK;
    (*rendered)++;
}

// Category submenu: Back (to top level) followed by the sorted item rows.
static void render_section(int *rendered, cloud_store_status_t status) {
    cloud_store_item_type_t type = (cloud_store_item_type_t)s_section;
    bool is_app = (type == CLOUD_STORE_TYPE_APP);
    options_view_set_title(s_options, is_app ? "Apps" : "Asset Packs");

    s_rows[*rendered] = options_view_add_back_row(s_options, row_event_cb, (void *)(intptr_t)*rendered);
    s_row_meta[*rendered].kind = ROW_KIND_BACK;
    (*rendered)++;

    sorted_cloud_item_t *items = is_app ? s_sorted_apps : s_sorted_packs;
    int item_n = is_app ? s_sorted_apps_count : s_sorted_packs_count;
    for (int i = 0; i < item_n && *rendered < CLOUD_STORE_MAX_ROWS - 1; ++i) {
        add_item_row(rendered, type, items[i].original_index);
    }

    if (item_n == 0) {
        const char *msg = (status.state == CLOUD_STORE_STATE_FETCHING)
            ? "Loading..."
            : (is_app ? "No apps available" : "No asset packs available");
        s_rows[*rendered] = options_view_add_item(s_options, msg, row_event_cb, (void *)(intptr_t)*rendered);
        s_row_meta[*rendered].kind = ROW_KIND_EMPTY;
        (*rendered)++;
    }
}

static void render_store(void) {
    if (!s_options) return;
    options_view_clear(s_options);
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_meta, 0, sizeof(s_row_meta));

    cloud_store_status_t status = cloud_store_get_status();

    load_sorted_items(CLOUD_STORE_TYPE_APP, s_sorted_apps, &s_sorted_apps_count);
    load_sorted_items(CLOUD_STORE_TYPE_ASSET_PACK, s_sorted_packs, &s_sorted_packs_count);

    // A submenu can outlive its category (e.g. app support lost); fall back to
    // the top level rather than showing a stale/invalid section.
    if (s_section == CLOUD_STORE_TYPE_APP && !s_apps_available) {
        s_section = CLOUD_SECTION_TOP;
    }

    // Manage the loading overlay first: it sets its own status-bar title, so the
    // row-render below (which sets the real title) must run afterwards to win.
    update_spinner(status.state);

    int rendered = 0;
    if (s_section == CLOUD_SECTION_TOP) {
        render_top_level(&rendered, status);
    } else {
        render_section(&rendered, status);
    }

    (void)rendered;
    options_view_refresh_styles(s_options);
    update_selection();
}

static void start_refresh(bool user_initiated) {
    cloud_store_status_t status = cloud_store_get_status();
    if (status.state == CLOUD_STORE_STATE_FETCHING) {
        toast_show_duration("Already refreshing", TOAST_INFO, 900);
        render_store();
        return;
    }

    uint32_t now = lv_tick_get();
    if (user_initiated && s_last_refresh_ms != 0 && (uint32_t)(now - s_last_refresh_ms) < 5000) {
        toast_show_duration("Refresh already tried", TOAST_INFO, 900);
        render_store();
        return;
    }
    s_last_refresh_ms = now ? now : 1;

    esp_err_t err = cloud_store_refresh_async();
    if (err == ESP_ERR_INVALID_STATE) {
        toast_show_duration("Cloud task already running", TOAST_INFO, 1200);
    } else if (err != ESP_OK) {
        toast_show_duration("Cloud refresh failed to start", TOAST_ERROR, 1800);
    } else {
        toast_show_duration("Refreshing cloud list...", TOAST_INFO, 1000);
    }
    render_store();
}

static void confirm_install_cb(void *user_data) {
    (void)user_data;
    esp_err_t err = cloud_store_install_async(s_pending_type, s_pending_id);
    if (err != ESP_OK) {
        toast_show_duration("Install task failed to start", TOAST_ERROR, 1800);
        return;
    }
    if (!s_progress) s_progress = progress_bar_view_create("Downloading");
    if (s_progress) progress_bar_view_set_subtext(s_progress, "Starting download");
}

// Leave the current screen level: a submenu returns to the top-level menu; the
// top-level menu exits back to the app gallery.
static void go_up_level(void) {
    if (s_section != CLOUD_SECTION_TOP) {
        s_section = CLOUD_SECTION_TOP;
        s_selected_row = 0;
        render_store();
    } else {
        display_manager_switch_view(&apps_menu_view);
    }
}

static void select_current(void) {
    if (s_selected_row < 0 || s_selected_row >= CLOUD_STORE_MAX_ROWS) return;
    const row_meta_t *meta = &s_row_meta[s_selected_row];
    switch (meta->kind) {
        case ROW_KIND_REFRESH: start_refresh(true); return;
        case ROW_KIND_BACK:    go_up_level(); return;
        case ROW_KIND_FOLDER:
            s_section = meta->item_type;
            s_selected_row = 1; // land on the first item, past the Back row
            render_store();
            return;
        case ROW_KIND_ITEM:    break;
        default:               return; // empty / unused
    }

    cloud_store_item_t item;
    if (!cloud_store_get_item(meta->item_type, meta->original_index, &item)) return;
    s_pending_type = item.type;
    strncpy(s_pending_id, item.id, sizeof(s_pending_id) - 1);
    s_pending_id[sizeof(s_pending_id) - 1] = '\0';

    cloud_item_install_status_t st = item_install_status(&item);
    bool is_app = (meta->item_type == CLOUD_STORE_TYPE_APP);
    const char *title;
    const char *action;
    if (st == CLOUD_ITEM_UPDATE) {
        title = is_app ? "Update App?" : "Update Asset Pack?";
        action = "Update";
    } else if (st == CLOUD_ITEM_INSTALLED) {
        title = is_app ? "Reinstall App?" : "Reinstall Asset Pack?";
        action = "Reinstall";
    } else {
        title = is_app ? "Install App?" : "Download Asset Pack?";
        action = is_app ? "Install" : "Download";
    }

    // Keep the popup terse: item name, a short blurb (first line only), and the
    // version — not the whole description.
    char body[160];
    int off = snprintf(body, sizeof(body), "%s", item.name);
    if (item.description[0]) {
        char blurb[56];
        size_t bl = 0;
        for (const char *p = item.description; *p && bl < sizeof(blurb) - 1; ++p) {
            if (*p == '\n' || *p == '\r') break; // first line only
            blurb[bl++] = *p;
        }
        blurb[bl] = '\0';
        bool truncated = (bl < strlen(item.description)) && item.description[bl] != '\0';
        off += snprintf(body + off, sizeof(body) - off, "\n%s%s", blurb, truncated ? "..." : "");
    }
    if (item.version[0]) {
        snprintf(body + off, sizeof(body) - off, "\n\nVersion: %s", item.version);
    }
    popup_confirm_show(&s_confirm_popup, lv_layer_top(), title,
                       body, action, "Cancel", confirm_install_cb, NULL);
}

static void close_progress(void) {
    if (s_progress) {
        progress_bar_view_close(s_progress);
        s_progress = NULL;
    }
}

static void status_timer_cb(lv_timer_t *timer) {
    (void)timer;
    cloud_store_status_t status = cloud_store_get_status();

    if (s_progress && (status.state == CLOUD_STORE_STATE_DOWNLOADING || status.state == CLOUD_STORE_STATE_INSTALLING)) {
        bool downloading = (status.state == CLOUD_STORE_STATE_DOWNLOADING);
        progress_bar_view_update(s_progress, downloading ? "Downloading" : "Installing");
        progress_bar_view_set_progress(s_progress, status.bytes_done, status.bytes_total);
        if (downloading) {
            // Downloads are abortable; installs (flash write) are not.
            char sub[CLOUD_STORE_NAME_MAX + 24];
            snprintf(sub, sizeof(sub), "%s\n" LV_SYMBOL_LEFT " Back to cancel", status.active_name);
            progress_bar_view_set_subtext(s_progress, sub);
        } else {
            progress_bar_view_set_subtext(s_progress, status.active_name);
        }
    }

    if (status.state != s_last_status_state) {
        if (status.state == CLOUD_STORE_STATE_READY || status.state == CLOUD_STORE_STATE_FAILED) {
            // READY here can also mean a cancelled install returned to the
            // catalog; make sure any lingering progress bar is torn down.
            close_progress();
            render_store();
        }
        if (status.state == CLOUD_STORE_STATE_DONE) {
            close_progress();
            toast_show_duration(status.active_type == CLOUD_STORE_TYPE_APP ? "App installed" : "Asset pack downloaded", TOAST_SUCCESS, 2200);
            render_store();
        } else if (status.state == CLOUD_STORE_STATE_FAILED && s_progress) {
            close_progress();
            toast_show_duration(status.error[0] ? status.error : "Cloud install failed", TOAST_ERROR, 2500);
        }
        s_last_status_state = status.state;
    }
}

static void cloud_store_create(void) {
    cloud_store_manager_init();

    s_apps_available = cloud_store_apps_available();

    s_root = gui_screen_create_root_no_bg(NULL, NULL, lv_color_hex(GUI_DEFAULT_BG_COLOR), LV_OPA_TRANSP);
    cloud_store_view.root = s_root;

    s_options = options_view_create_no_bg(s_root, "Cloud Store");
    if (!asset_pack_has_psram()) {
        // The Store uses an opaque root and list, so retaining decoded images
        // only wastes scarce internal RAM until the user returns to a themed view.
        asset_pack_release_cached_images();
    }

    s_section = CLOUD_SECTION_TOP;
    s_selected_row = 0;
    s_last_refresh_ms = 0;
    render_store();
    s_status_timer = lv_timer_create(status_timer_cb, 300, NULL);

    cloud_store_status_t status = cloud_store_get_status();
    s_last_status_state = status.state;
    if (status.state == CLOUD_STORE_STATE_IDLE || (status.state == CLOUD_STORE_STATE_FAILED && cloud_store_get_count(CLOUD_STORE_TYPE_APP) == 0 && cloud_store_get_count(CLOUD_STORE_TYPE_ASSET_PACK) == 0)) {
        start_refresh(false);
    }
}

static void cloud_store_destroy(void) {
    popup_confirm_close(&s_confirm_popup);
    close_progress();
    if (s_loading) {
        scan_status_close(s_loading); // lives on lv_layer_top, not under s_root
        s_loading = NULL;
    }
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    if (s_options) {
        options_view_destroy(s_options);
        s_options = NULL;
    }
    if (s_root) {
        lvgl_obj_del_safe(&s_root);
        cloud_store_view.root = NULL;
    }
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_meta, 0, sizeof(s_row_meta));
    s_apps_available = false;
    s_section = CLOUD_SECTION_TOP;
    cloud_store_manager_cleanup();
}

// When a download is running, "back" cancels it and keeps the store open
// instead of leaving the view. Returns true if it consumed the back action.
static bool try_cancel_download(void) {
    if (!s_progress) return false;
    if (cloud_store_get_status().state != CLOUD_STORE_STATE_DOWNLOADING) return false;
    cloud_store_cancel_install();
    close_progress();
    toast_show_duration("Download cancelled", TOAST_INFO, 1400);
    return true;
}

static bool cloud_store_keyboard_activation_key(int key) {
    return key == LV_KEY_ENTER || key == 13 ||
           key == LV_KEY_ESC || key == '`' || key == 29;
}

static void cloud_store_input(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        if (popup_confirm_handle_touch(&s_confirm_popup, &event->data.touch_data)) return;
        return;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 0) {
            if (!try_cancel_download()) go_up_level();
        } else if (button == 1) {
            if (popup_confirm_is_open(s_confirm_popup)) popup_confirm_select(&s_confirm_popup);
            else select_current();
        } else if (button == 2) {
            if (popup_confirm_is_open(s_confirm_popup)) popup_confirm_move(s_confirm_popup, -1);
            else move_selection(-1);
        } else if (button == 4) {
            if (popup_confirm_is_open(s_confirm_popup)) popup_confirm_move(s_confirm_popup, 1);
            else move_selection(1);
        }
        return;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (event->is_repeat && cloud_store_keyboard_activation_key(key)) return;
        // While the confirm popup is up, keys drive the popup — not the list
        // behind it (the joystick/encoder paths already did this; keyboard did
        // not, so Enter re-triggered the row and Esc left the whole view).
        if (popup_confirm_is_open(s_confirm_popup)) {
            if (key == LV_KEY_ENTER || key == 13) {
                popup_confirm_select(&s_confirm_popup);
            } else if (key == LV_KEY_ESC || key == '`' || key == 29) {
                popup_confirm_close(&s_confirm_popup);
            } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN ||
                       key == 'h' || key == 'l' || key == 'k' || key == 'j' ||
                       key == ',' || key == '.' || key == ';' || key == '/') {
                popup_confirm_move(s_confirm_popup, 1);
            }
            return;
        }
        // Match the back/select key convention used by the other options_view
        // list screens (sd_browser, options): the hardware back key emits 29,
        // not 27 (27 == LV_KEY_ESC, so the old check was redundant and the real
        // back key was ignored).
        if (key == LV_KEY_ESC || key == '`' || key == 29) {
            if (!try_cancel_download()) go_up_level();
        } else if (key == LV_KEY_ENTER || key == 13) {
            select_current();
        } else if (key == LV_KEY_UP || key == 'k' || key == 'w' || key == ';' || key == ',') {
            move_selection(-1);
        } else if (key == LV_KEY_DOWN || key == 'j' || key == 's' || key == '.' || key == '/') {
            move_selection(1);
        }
        return;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (popup_confirm_is_open(s_confirm_popup)) {
            if (event->data.encoder.button) popup_confirm_select(&s_confirm_popup);
            else popup_confirm_move(s_confirm_popup, event->data.encoder.direction);
            return;
        }
        if (event->data.encoder.button) {
            select_current();
        } else if (event->data.encoder.direction > 0) {
            move_selection(1);
        } else if (event->data.encoder.direction < 0) {
            move_selection(-1);
        }
        return;
    }
#ifdef CONFIG_USE_ENCODER
    if (event->type == INPUT_TYPE_EXIT_BUTTON && event->data.exit_pressed) {
        if (popup_confirm_is_open(s_confirm_popup)) {
            popup_confirm_close(&s_confirm_popup);
        } else if (!try_cancel_download()) {
            go_up_level();
        }
        return;
    }
#endif
}

static void get_cloud_store_callback(void **callback) {
    *callback = cloud_store_input;
}

View cloud_store_view = {
    .root = NULL,
    .create = cloud_store_create,
    .destroy = cloud_store_destroy,
    .input_callback = cloud_store_input,
    .name = "Cloud Store",
    .get_hardwareinput_callback = get_cloud_store_callback,
};
