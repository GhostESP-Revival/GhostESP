#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/keyboard_screen.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "gui/popup.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include "managers/sd_card_manager.h"
#include "managers/fuel_gauge_manager.h"
#include "core/glog.h"

// popup helper forward declarations
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height);
lv_obj_t *popup_create_container_with_offset(lv_obj_t *parent, int width, int height, lv_coord_t y_offset);

lv_obj_t *popup_add_styled_button(lv_obj_t *container,
	const char *label_text,
    int btn_w, int btn_h,
    lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs,
    const lv_font_t *font, lv_event_cb_t cb, void *user_data);

lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs);

lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs);
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
#include "managers/nfc/mifare_classic.h"
#include "managers/nfc/mifare_attack.h"
#endif
#include "managers/chameleon_manager.h"
#include "managers/nfc/ndef.h"

// Forward declaration for nfc_get_detected_title
static const char* nfc_get_detected_title(void);

// freeRTOS used regardless of backend
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_NFC_PN532
#include "pn532.h"
#include "driver/i2c.h"
#include "pn532_driver.h"
#include "pn532_driver_i2c.h"
#endif

// always needed for parsing .nfc files and displaying details, even without PN532
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/write_ntag.h"

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation declared later after static variables are defined)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);
void mfc_ui_set_cache_mode(bool on);
void mfc_ui_set_paused(bool on);
bool nfc_is_scan_cancelled(void);
bool nfc_is_dict_skip_requested(void);

// Forward declaration of this view instance for internal references
extern View nfc_view;

static const char *TAG = "NFCView";

// touch nav button sizing to match options_screen
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5

static lv_style_t style_menu_item;
static lv_style_t style_menu_item_alt;
static lv_style_t style_selected_item;
static lv_style_t style_menu_label;
static bool styles_initialized = false;

// forward declarations for helpers used throughout this file
static void init_styles(void);
static lv_style_t *get_zebra_style(int idx);
static const lv_font_t* get_menu_font(void);
static void vertically_center_label(lv_obj_t *label, lv_obj_t *btn);
void nfc_option_event_cb(lv_event_t *e);
static void highlight_selected(void);
void nfc_view_input_cb(InputEvent *event);

static lv_obj_t *root = NULL;
static lv_obj_t *menu_container = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *emulate_btn = NULL;
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static int selected_index = 0;
static int num_items = 0; // will be set when building menu

// write file list state
static bool in_write_list = false;
static char **nfc_file_paths = NULL;
static size_t nfc_file_count = 0;

// saved file list state
static bool in_saved_list = false;
static char **saved_file_paths = NULL;
static size_t saved_file_count = 0;

// NFC write popup
static lv_obj_t *nfc_write_popup = NULL;
static lv_obj_t *nfc_write_cancel_btn = NULL;
static lv_obj_t *nfc_write_go_btn = NULL;
static lv_obj_t *nfc_write_title_label = NULL;
static lv_obj_t *nfc_write_details_label = NULL;
static int nfc_write_popup_selected = 0; // 0=Cancel, 1=Write
static volatile bool nfc_write_cancel = false;
static volatile bool nfc_write_in_progress = false;
static bool g_write_image_valid = false;
#ifdef CONFIG_NFC_PN532
static ntag_file_image_t g_write_image;
#endif
static char g_write_image_path[256] = {0};

// jit sd helpers for somethingsomething template (mirror infrared behavior)
static bool nfc_sd_begin(bool *display_was_suspended)
{
    if (display_was_suspended) *display_was_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        return sd_card_mount_for_flush(display_was_suspended) == ESP_OK;
    }
#endif
    return true;
}

static void nfc_sd_end(bool display_was_suspended)
{
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
#else
    (void)display_was_suspended;
#endif
}

// saved details popup
static lv_obj_t *saved_popup = NULL;
static lv_obj_t *saved_close_btn = NULL;
static lv_obj_t *saved_rename_btn = NULL;
static lv_obj_t *saved_delete_btn = NULL;
static lv_obj_t *saved_title_label = NULL;
static lv_obj_t *saved_details_label = NULL;
static int saved_popup_selected = 0;
static char g_saved_current_path[256] = {0};

// user mfc keys popup
static lv_obj_t *keys_popup = NULL;
static lv_obj_t *keys_close_btn = NULL;
static lv_obj_t *keys_title_label = NULL;
static lv_obj_t *keys_details_label = NULL;
static lv_obj_t *keys_up_btn = NULL;
static lv_obj_t *keys_down_btn = NULL;
static lv_obj_t *keys_scroll = NULL;
static int keys_popup_selected = 0;
static lv_obj_t *keys_btn_bar = NULL;

// Match options_screen theme palettes for selected row color
static const uint32_t theme_palettes[15][6] __attribute__((unused)) = {
    {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800},
    {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC},
    {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F},
    {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF},
    {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36},
    {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888},
    {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63},
    {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0},
    {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3},
    {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500},
    {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00},
    {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080},
    {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B},
    {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854},
    {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}
};

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation moved below after static phase variables are declared)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);

static int button_height_global = 0;
static bool is_small_screen_global = false;

// NFC scan popup (modeled after IR learning popup)
static lv_obj_t *nfc_scan_popup = NULL;
static lv_obj_t *nfc_btn_bar = NULL;
static lv_obj_t *nfc_scan_cancel_btn = NULL;
static lv_obj_t *nfc_scan_more_btn = NULL;
static lv_obj_t *nfc_scan_save_btn = NULL;
static lv_obj_t *nfc_scan_scroll_btn = NULL;
static lv_obj_t *nfc_scan_attack_btn = NULL;
static lv_obj_t *nfc_title_label = NULL;
static lv_obj_t *nfc_uid_label = NULL;
static lv_obj_t *nfc_type_label = NULL;
static lv_obj_t *nfc_details_label = NULL;
static lv_obj_t *nfc_details_scroll = NULL;
// Progress bar removed; we will update title and text instead
// Track dictionary brute-force phase for richer UI status
static int mfc_phase_sector = -1;
static int mfc_phase_first_block = -1;
static bool mfc_phase_key_b = false;
static int mfc_phase_total = 0;
static int nfc_popup_selected = 0; // 0 = Cancel, 1 = More (when available)
static int nfc_details_view_mode = 0; // 0=Summary, 1=Basic, 2=Full
static bool nfc_more_visible = false;
static bool nfc_details_visible = false;
static bool nfc_save_visible = false;
static bool nfc_attack_visible = false;
// When true, the MFC layer is performing a second-pass cache fill (live-read) after bruteforce.
static bool nfc_cache_fill_phase = false;
// When true, UI requests to skip dictionary attempts (basic read only)
static bool nfc_dict_skip_requested = false;
// When true, a tag was removed and we're waiting for re-present
static bool nfc_paused = false;
// When true, NFC details are ready and scan is complete
static bool nfc_details_ready = false;
// Active scan session to filter out stale async UI events
static uint32_t nfc_scan_session = 0;
// Simple boolean event payload for async calls
typedef struct { bool on; uint32_t session; } nfc_bool_evt_t;
// PN532 UID event payload for async label updates
typedef struct { uint32_t session; uint8_t uid[10]; uint8_t uid_len; } nfc_uid_evt_t;
// Dictionary progress payload for async calls
typedef struct { int c; int t; uint32_t s; } dict_prog_t;
// Static event pool to eliminate per-event malloc
#define NFC_EVENT_POOL_SIZE 8
static nfc_bool_evt_t nfc_bool_pool[NFC_EVENT_POOL_SIZE];
static dict_prog_t nfc_dict_pool[NFC_EVENT_POOL_SIZE];
static uint32_t nfc_bool_pool_mask = 0;
static uint32_t nfc_dict_pool_mask = 0;
static bool nfc_skip_label_applied = false;

static const char* get_details_split_point(const char *text) {
    if (!text) return NULL;
    const char *p = strstr(text, "Keys ");
    if (!p) return NULL;
    p = strchr(p, '\n');
    if (p) return p + 1;
    return NULL;
}

static bool has_extra_details(const char *text) {
    const char *p = get_details_split_point(text);
    return (p && *p != '\0');
}

static void nfc_reset_more_button_label(void) {
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
    }
    nfc_skip_label_applied = false;
    nfc_details_view_mode = 0;
}

// Pool allocation helpers
static nfc_bool_evt_t* nfc_bool_pool_alloc(void) {
    for (int i = 0; i < NFC_EVENT_POOL_SIZE; i++) {
        if (!(nfc_bool_pool_mask & (1U << i))) {
            nfc_bool_pool_mask |= (1U << i);
            return &nfc_bool_pool[i];
        }
    }
    return NULL;
}
static void nfc_bool_pool_free(nfc_bool_evt_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_bool_pool;
    if (idx >= 0 && idx < NFC_EVENT_POOL_SIZE) {
        nfc_bool_pool_mask &= ~(1U << idx);
    }
}
static dict_prog_t* nfc_dict_pool_alloc(void) {
    for (int i = 0; i < NFC_EVENT_POOL_SIZE; i++) {
        if (!(nfc_dict_pool_mask & (1U << i))) {
            nfc_dict_pool_mask |= (1U << i);
            return &nfc_dict_pool[i];
        }
    }
    return NULL;
}
static void nfc_dict_pool_free(dict_prog_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_dict_pool;
    if (idx >= 0 && idx < NFC_EVENT_POOL_SIZE) {
        nfc_dict_pool_mask &= ~(1U << idx);
    }
}

// Async setter for paused title/state
static void nfc_set_paused_async(void *ptr) {
    nfc_bool_evt_t *ev = (nfc_bool_evt_t*)ptr;
    if (!ev) return;
    if (ev->session != nfc_scan_session) { nfc_bool_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_bool_pool_free(ev); return; }
    nfc_paused = ev->on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (ev->on) {
            lv_label_set_text(nfc_title_label, "Paused - present tag to continue");
        } else {
            if (nfc_cache_fill_phase) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
            else if (!nfc_details_visible) lv_label_set_text(nfc_title_label, "Bruteforcing keys... 0%");
            else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
        }
    }
    nfc_bool_pool_free(ev);
}

// Exposed to MFC layer
void mfc_ui_set_paused(bool on) {
    if (!display_manager_is_available()) return;
    nfc_bool_evt_t *ev = nfc_bool_pool_alloc();
    if (!ev) return;
    ev->on = on;
    ev->session = nfc_scan_session;
    lv_async_call(nfc_set_paused_async, ev);
}

// Async setter for cache fill phase title/state
static void nfc_set_cache_mode_async(void *ptr) {
    nfc_bool_evt_t *ev = (nfc_bool_evt_t*)ptr;
    if (!ev) return;
    if (ev->session != nfc_scan_session) { nfc_bool_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_bool_pool_free(ev); return; }
    bool on = ev->on;
    nfc_cache_fill_phase = on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (on) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
        else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
    }
    nfc_bool_pool_free(ev);
}

// Exposed to MFC layer to toggle cache-fill phase
void mfc_ui_set_cache_mode(bool on) {
    if (!display_manager_is_available()) return;
    nfc_bool_evt_t *ev = nfc_bool_pool_alloc();
    if (!ev) return;
    ev->on = on;
    ev->session = nfc_scan_session;
    lv_async_call(nfc_set_cache_mode_async, ev);
}

#ifdef CONFIG_NFC_PN532
// Exposed for mifare_classic.c to honor UI skip request (weak extern there)
bool nfc_is_dict_skip_requested(void) { return nfc_dict_skip_requested; }
#endif

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
static const mfc_attack_hooks_t nfc_ui_attack_hooks = {
    .on_phase = mfc_ui_set_phase,
    .on_cache_mode = mfc_ui_set_cache_mode,
    .on_paused = mfc_ui_set_paused,
    .should_cancel = nfc_is_scan_cancelled,
    .should_skip_dict = nfc_is_dict_skip_requested
};
#endif

static void nfc_scan_cancel_cb(lv_event_t *e);
static void nfc_scan_more_cb(lv_event_t *e);
static void nfc_scan_save_cb(lv_event_t *e);
static void nfc_scan_scroll_cb(lv_event_t *e);
static void create_nfc_scan_popup(void);
void cleanup_nfc_scan_popup(void *obj);
static void update_nfc_popup_selection(void);
static void update_nfc_buttons_layout(void);
static void nfc_show_details_view(bool show);
static bool write_flipper_nfc_file(void);
// Worker task helpers
static void nfc_save_task(void *arg);
static void nfc_save_done_async(void *ptr);
// Deferred scan start if previous scan task hasn't exited yet (legacy - no longer used)
static void nfc_try_start_scan_timer_cb(lv_timer_t *t);
// Guard timer to avoid creating multiple retry timers (legacy - no longer used)
static lv_timer_t *nfc_scan_retry_timer = NULL;

// Write flow (file list and popup)
static void nfc_enter_write_list(void);
static void nfc_clear_write_list(void);
static void nfc_file_item_cb(lv_event_t *e);
static void back_to_root_menu(void);
static void create_nfc_write_popup(const char *path);
void cleanup_nfc_write_popup(void *obj);
static void nfc_write_cancel_cb(lv_event_t *e);
static void nfc_write_go_cb(lv_event_t *e);
static void update_nfc_write_popup_selection(void);
static void update_nfc_write_buttons_layout(void);
// saved flow
static void saved_enter_list(void);
void saved_clear_list(void);
static void saved_file_item_cb(lv_event_t *e);
static void create_saved_details_popup(const char *path);
void cleanup_saved_details_popup(void *obj);
static void saved_close_cb(lv_event_t *e);
static void update_saved_popup_selection(void);
static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w);
static void layout_popup_buttons_row(lv_obj_t *popup, lv_obj_t **btns, int count, lv_coord_t min_w, lv_coord_t max_w, lv_coord_t min_threshold, lv_coord_t gap, lv_coord_t yoff);
static void update_saved_buttons_layout(void);
static void update_saved_buttons_layout(void);
static char* build_mfc_details_from_file(const char *path, char **out_title);
static void saved_rename_cb(lv_event_t *e);
static void saved_delete_cb(lv_event_t *e);
static void saved_rename_keyboard_callback(const char *name);
typedef struct {
    char old_path[256];
    char new_path[256];
    int success;
} saved_rename_job_t;
static void saved_rename_ui_done_cb(void *param);
static void saved_rename_task(void *arg);
// keys popup
static void create_keys_popup(void);
static void cleanup_keys_popup(void *obj);
static void keys_close_cb(lv_event_t *e);
static void update_keys_popup_selection(void);
static void update_keys_buttons_layout(void);
static void update_keys_buttons_layout(void);
static void keys_scroll_up_cb(lv_event_t *e);
static void keys_scroll_down_cb(lv_event_t *e);

// chameleon ultra popup (basic controls)
static lv_obj_t *cu_popup = NULL;
static lv_obj_t *cu_title_label = NULL;
static lv_obj_t *cu_details_label = NULL;
static lv_obj_t *cu_close_btn = NULL;
static lv_obj_t *cu_connect_btn = NULL;
static lv_obj_t *cu_disconnect_btn = NULL;
static lv_obj_t *cu_reader_btn = NULL;
static lv_obj_t *cu_scan_hf_btn = NULL;
static lv_obj_t *cu_save_hf_btn = NULL;
static lv_obj_t *cu_more_btn = NULL;
static int cu_popup_selected = 0;
static bool cu_save_visible = false;
static volatile bool cu_busy = false;
static bool cu_more_expanded = false;
static void cu_state_timer_cb(lv_timer_t *t);
static lv_timer_t *cu_state_timer = NULL;

static void create_cu_popup(void);
void cleanup_cu_popup(void *obj);
static void update_cu_buttons_layout(void);
static void update_cu_popup_selection(void);
static void cu_close_cb(lv_event_t *e);
static void cu_connect_cb(lv_event_t *e);
static void cu_disconnect_cb(lv_event_t *e);
static void cu_reader_cb(lv_event_t *e);
static void cu_scan_hf_cb(lv_event_t *e);
static void cu_save_hf_cb(lv_event_t *e);
static void cu_more_cb(lv_event_t *e);
static void cu_connect_task(void *arg);
static void cu_disconnect_task(void *arg);
static void cu_reader_task(void *arg);
static void cu_scan_hf_task(void *arg);
static void cu_save_hf_task(void *arg);
static void cu_bool_done_async(void *ptr);
#ifdef CONFIG_NFC_PN532
static bool ensure_pn532_ready(void);
static void nfc_write_task(void *arg);
typedef struct { int current; int total; } nfc_wr_prog_t;
static bool nfc_write_progress_cb(int current, int total, void *user);
static void nfc_write_progress_async(void *ptr);
static void nfc_write_done_async(void *ptr);
#endif

// Dictionary progress callback -> UI updater
static void nfc_progress_update_async(void *ptr);
// PN532 UID/type updater forward declaration
static void nfc_update_labels_async(void *ptr);
// UI hook from MIFARE Classic layer to indicate sector/block/key phase (implementation)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) {
    if (!display_manager_is_available()) return;
    mfc_phase_sector = sector;
    mfc_phase_first_block = first_block;
    mfc_phase_key_b = key_b;
    mfc_phase_total = total_keys;
    dict_prog_t *dp = nfc_dict_pool_alloc();
    if (dp) { dp->c = 0; dp->t = total_keys; dp->s = nfc_scan_session; lv_async_call(nfc_progress_update_async, dp); }
}
static void mfc_dict_progress_cb(int current, int total, void *user) {
    (void)user;
    if (!display_manager_is_available()) return;
    if (total <= 0) return;
    int percent = (current * 100) / total;
    if (percent < 0) { percent = 0; }
    if (percent > 100) { percent = 100; }
    static int last_percent = -1;
    if (percent == last_percent) return;
    last_percent = percent;
    dict_prog_t *dp = nfc_dict_pool_alloc();
    if (!dp) return;
    dp->c = current; dp->t = total; dp->s = nfc_scan_session;
    lv_async_call(nfc_progress_update_async, dp);
}

// Static pool for UID events to eliminate malloc
#define NFC_UID_POOL_SIZE 4
static nfc_uid_evt_t nfc_uid_pool[NFC_UID_POOL_SIZE];
static uint32_t nfc_uid_pool_mask = 0;

static nfc_uid_evt_t* nfc_uid_pool_alloc(void) {
    for (int i = 0; i < NFC_UID_POOL_SIZE; i++) {
        if (!(nfc_uid_pool_mask & (1U << i))) {
            nfc_uid_pool_mask |= (1U << i);
            return &nfc_uid_pool[i];
        }
    }
    return NULL;
}
static void nfc_uid_pool_free(nfc_uid_evt_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_uid_pool;
    if (idx >= 0 && idx < NFC_UID_POOL_SIZE) {
        nfc_uid_pool_mask &= ~(1U << idx);
    }
}

// Async updater for PN532 UID/type summary lines
static void nfc_update_labels_async(void *ptr) {
    if (!ptr) return;
    nfc_uid_evt_t *ev = (nfc_uid_evt_t*)ptr;
    if (ev->session != nfc_scan_session) { nfc_uid_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_uid_pool_free(ev); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { nfc_uid_pool_free(ev); return; }
    char uid_text[64]; int pos = 0; pos += snprintf(uid_text, sizeof(uid_text), "UID:");
    for (int i = 0; i < ev->uid_len && pos < (int)sizeof(uid_text) - 4; ++i) {
        pos += snprintf(uid_text + pos, sizeof(uid_text) - pos, " %02X", ev->uid[i]);
    }
    if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
        lv_label_set_text(nfc_uid_label, uid_text);
    }
    if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
        lv_label_set_text(nfc_type_label, "Type: ISO14443A");
    }
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    nfc_uid_pool_free(ev);
}

static void nfc_progress_update_async(void *ptr) {
    if (!ptr) return;
    dict_prog_t *dp = (dict_prog_t*)ptr;
    if (dp->s != nfc_scan_session) { nfc_dict_pool_free(dp); return; }
    if (!display_manager_is_available()) { nfc_dict_pool_free(dp); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { nfc_dict_pool_free(dp); return; }
    int percent = 0;
    if (dp->t > 0) percent = (dp->c * 100) / dp->t;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    char phase[40];
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        snprintf(phase, sizeof(phase), " | Sec %d Blk %d Key %c", mfc_phase_sector, mfc_phase_first_block, mfc_phase_key_b ? 'B' : 'A');
    } else {
        phase[0] = '\0';
    }
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        char title[80];
        bool bruteforce_active = (!nfc_paused && !nfc_cache_fill_phase && !nfc_dict_skip_requested && (mfc_phase_total > 0));
        if (nfc_paused) snprintf(title, sizeof(title), "Paused - present tag to continue");
        else if (nfc_cache_fill_phase) snprintf(title, sizeof(title), "Reading sectors... %d%%", percent);
        else if (nfc_dict_skip_requested) snprintf(title, sizeof(title), "Basic read (skipping dict) ...");
        else if (bruteforce_active) snprintf(title, sizeof(title), "Bruteforcing keys... %d%%", percent);
        else if (nfc_details_ready) snprintf(title, sizeof(title), "%s", nfc_get_detected_title());
        else snprintf(title, sizeof(title), "Scanning NFC...");
        lv_label_set_text(nfc_title_label, title);
        if (nfc_details_ready && !bruteforce_active && !nfc_cache_fill_phase) lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    if (!nfc_paused && !nfc_cache_fill_phase && !nfc_dict_skip_requested && !nfc_details_ready && mfc_phase_total > 0 && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        if (!nfc_skip_label_applied) {
            lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
            nfc_more_visible = true;
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Skip");
            update_nfc_buttons_layout();
            update_nfc_popup_selection();
            nfc_skip_label_applied = true;
        }
    }
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
            char l1[32];
            snprintf(l1, sizeof(l1), "Sec %d Blk %d", mfc_phase_sector, mfc_phase_first_block);
            lv_label_set_text(nfc_uid_label, l1);
        }
        if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
            char l2[16];
            snprintf(l2, sizeof(l2), "Key %c", mfc_phase_key_b ? 'B' : 'A');
            lv_label_set_text(nfc_type_label, l2);
        }
    }
    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        char info[96];
        if (dp->t > 0) snprintf(info, sizeof(info), "Dictionary: %d/%d (%d%%)%s", dp->c, dp->t, percent, phase);
        else snprintf(info, sizeof(info), "Dictionary: %d (unknown total)%s", dp->c, phase);
        lv_label_set_text(nfc_details_label, info);
        lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    nfc_dict_pool_free(dp);
}

static volatile bool nfc_scan_cancel = false;
static volatile bool nfc_save_in_progress = false;
static volatile bool nfc_attack_in_progress = false;

// Expose cancel status to MIFARE Classic layer (cooperative cancellation)
bool nfc_is_scan_cancelled(void) { return nfc_scan_cancel; }


#ifdef CONFIG_NFC_PN532
static pn532_io_handle_t g_pn532 = NULL;
static pn532_io_t g_pn532_instance;
#endif
static TaskHandle_t nfc_scan_task_handle = NULL;
static char *nfc_details_text = NULL;
static char nfc_detected_title[64] = {0};
static uint32_t nfc_details_session = 0;
static uint8_t g_uid[10] = {0};
static uint8_t g_uid_len = 0;
static uint16_t g_atqa = 0;
static uint8_t g_sak = 0;
#ifdef CONFIG_NFC_PN532
static NTAG2XX_MODEL g_model = NTAG2XX_UNKNOWN;
#endif


typedef struct {
    char *text;      // allocated details text
    size_t text_len; // length of text
    uint32_t session; // scan session
} ndef_details_result_t;

static const char* nfc_get_detected_title(void) {
    return (nfc_detected_title[0] != '\0') ? nfc_detected_title : "NFC Tag";
}

static void nfc_update_title_from_details(const char *details) {
    if (!details) return;
    const char *card = strstr(details, "Card:");
    if (!card) return;
    card += 5;
    while (*card == ' ' || *card == '\t') card++;
    if (!*card) return;
    size_t idx = 0;
    while (card[idx] && card[idx] != '\n' && card[idx] != '|' && idx < sizeof(nfc_detected_title) - 1) {
        nfc_detected_title[idx] = card[idx];
        idx++;
    }
    nfc_detected_title[idx] = '\0';
}

static void nfc_set_details_async(void *ptr) {
    if (!ptr) return;
    ndef_details_result_t *res = (ndef_details_result_t *)ptr;
    if (res->session != nfc_scan_session) { if (res->text) free(res->text); free(res); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { if (res->text) free(res->text); free(res); return; }
    // Replace old details if any
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_text = res->text;
    nfc_details_ready = true;
    nfc_update_title_from_details(nfc_details_text);
    if (nfc_detected_title[0] == '\0') {
        snprintf(nfc_detected_title, sizeof(nfc_detected_title), "NFC Tag");
    }
    // reset phase state and update summary labels to indicate completion
    mfc_phase_sector = -1;
    mfc_phase_first_block = -1;
    mfc_phase_total = 0;
    // Revert label back to More after bruteforce completes
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
        lv_obj_clear_state(nfc_scan_more_btn, LV_STATE_DISABLED);
    }
    // don't stomp the title here; let scan/progress or details phases set it to avoid flicker
    if (!nfc_details_visible) {
        if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
            lv_label_set_text(nfc_type_label, "Scan complete - press More");
        }
    }
    // If already showing details, update label
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, nfc_detected_title);
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    if (nfc_details_visible && nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_label_set_text(nfc_details_label, nfc_details_text);
        lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    // Reset dict-skip flag for next scans
    nfc_dict_skip_requested = false;
    nfc_skip_label_applied = false;

    // ensure the More button is available once details are ready (ntag and classic)
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }

    // Reveal Save button now that details (and cache) are ready
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && !nfc_save_visible) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }

    // Resume normal I2C activity now that scanning/bruteforce has finished
    display_manager_set_low_i2c_mode(false);

    free(res);
}



#ifdef CONFIG_NFC_PN532
static void nfc_build_and_set_details(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len) {
    // Prefer MIFARE Classic summary if SAK indicates Classic
    if (mfc_is_classic_sak(g_sak)) {
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "Bruteforcing keys... 0%");
        // Reduce I2C contention during PN532 scanning/bruteforce
        display_manager_set_low_i2c_mode(true);
        char *text = mfc_build_details_summary(io, uid, uid_len, g_atqa, g_sak);
        // Check if scan was cancelled during MIFARE processing (e.g., while paused)
        if (nfc_scan_cancel || !text) {
            if (text) free(text);
            mfc_set_progress_callback(NULL, NULL);
            return;
        }
        ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        if (!res) { free(text); return; }
        res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
        if (display_manager_is_available()) lv_async_call(nfc_set_details_async, res);
        else { if (res->text) free(res->text); free(res); }
        mfc_set_progress_callback(NULL, NULL);
        return;
    }

    // Otherwise try NTAG/Ultralight (Type 2)
    uint8_t *mem = NULL; size_t mem_len = 0; NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    if (!ntag_t2_read_user_memory(io, &mem, &mem_len, &model)) {
        size_t cap = 256;
        ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        if (!res) return;
        res->text = (char*)malloc(cap);
        res->text_len = cap; res->session = nfc_scan_session;
        if (!res->text) { free(res); return; }
        char *w = res->text; snprintf(w, cap, "UID:"); size_t used = strlen(w); w += used; cap -= used;
        for (uint8_t i = 0; i < uid_len && cap > 3; ++i) { int n = snprintf(w, cap, " %02X", uid[i]); if (n > 0) { w += n; cap -= n; } }
        snprintf(w, cap, "\nNo NDEF data\n");
        if (display_manager_is_available()) lv_async_call(nfc_set_details_async, res);
        else { if (res->text) free(res->text); free(res); }
        return;
    }
    char *text = ntag_t2_build_details_from_mem(mem, mem_len, uid, uid_len, model);
    free(mem);
    if (!text) return;
    g_model = model;
    snprintf(nfc_detected_title, sizeof(nfc_detected_title), "%s", ntag_t2_model_str(model));
    ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
    if (!res) { free(text); return; }
    res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
    if (display_manager_is_available()) lv_async_call(nfc_set_details_async, res);
    else { if (res->text) free(res->text); free(res); }
    return;
}
#endif

// backend switch helper
static bool using_chameleon_backend(void) {
#if defined(CONFIG_NFC_CHAMELEON)
    return chameleon_manager_is_ready();
#else
    return false;
#endif
}

// chameleon ultra scan result -> ui
typedef struct {
    uint32_t session;
    uint8_t uid[10];
    uint8_t uid_len;
    uint16_t atqa;
    uint8_t sak;
} cu_scan_result_t;

#if defined(CONFIG_NFC_CHAMELEON)
static void nfc_refresh_cu_details_from_cache(void) {
    uint32_t current = chameleon_manager_get_cached_details_session();
    if (nfc_details_ready && nfc_details_text && nfc_details_session == current) {
        return;
    }
    if (nfc_details_text) {
        free(nfc_details_text);
        nfc_details_text = NULL;
    }
    const char *cached = chameleon_manager_get_cached_details();
    if (cached) {
        nfc_details_text = strdup(cached);
        nfc_details_ready = (nfc_details_text != NULL);
        if (nfc_details_ready) {
            nfc_update_title_from_details(nfc_details_text);
        }
    } else {
        nfc_details_ready = false;
    nfc_detected_title[0] = '\0';
    }
    nfc_details_session = current;
    nfc_reset_more_button_label();
}
#endif

// Static pool for Chameleon scan results to eliminate malloc
#define CU_SCAN_POOL_SIZE 4
static cu_scan_result_t cu_scan_pool[CU_SCAN_POOL_SIZE];
static uint32_t cu_scan_pool_mask = 0;

static cu_scan_result_t* cu_scan_pool_alloc(void) {
    for (int i = 0; i < CU_SCAN_POOL_SIZE; i++) {
        if (!(cu_scan_pool_mask & (1U << i))) {
            cu_scan_pool_mask |= (1U << i);
            return &cu_scan_pool[i];
        }
    }
    return NULL;
}
static void cu_scan_pool_free(cu_scan_result_t *ptr) {
    if (!ptr) return;
    int idx = ptr - cu_scan_pool;
    if (idx >= 0 && idx < CU_SCAN_POOL_SIZE) {
        cu_scan_pool_mask &= ~(1U << idx);
    }
}

static void nfc_set_cu_scan_async(void *ptr) {
    cu_scan_result_t *r = (cu_scan_result_t*)ptr;
    if (!r) return;
    if (r->session != nfc_scan_session) { cu_scan_pool_free(r); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { cu_scan_pool_free(r); return; }
    // Update summary labels
    if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
        char uid_text[64]; int pos = 0; pos += snprintf(uid_text, sizeof(uid_text), "UID:");
        for (int i = 0; i < r->uid_len && pos < (int)sizeof(uid_text) - 4; ++i) pos += snprintf(uid_text + pos, sizeof(uid_text) - pos, " %02X", r->uid[i]);
        lv_label_set_text(nfc_uid_label, uid_text);
    }
    if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
        lv_label_set_text(nfc_type_label, "Type: ISO14443A");
    }
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "NFC Tag");
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    // Reveal buttons: More and Save
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
    }
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    cu_scan_pool_free(r);
}

// cu worker tasks
static void nfc_scan_cu_task(void *arg) {
    (void)arg;
    if (nfc_scan_cancel) { vTaskDelete(NULL); return; }
    bool ok = chameleon_manager_scan_hf();
    if (ok && !nfc_scan_cancel) {
        uint8_t uid[10] = {0}; uint8_t ul = 0; uint16_t atqa = 0; uint8_t sak = 0;
        if (chameleon_manager_get_last_hf_scan(uid, &ul, &atqa, &sak)) {
            cu_scan_result_t *res = cu_scan_pool_alloc();
            if (res) {
                res->session = nfc_scan_session;
                res->uid_len = ul; if (ul > sizeof(res->uid)) res->uid_len = sizeof(res->uid);
                memcpy(res->uid, uid, res->uid_len);
                res->atqa = atqa; res->sak = sak;
                lv_async_call(nfc_set_cu_scan_async, res);
            }
            // If MIFARE Classic (0x08/0x18/0x09), perform dict-based read on CU
#if defined(CONFIG_NFC_CHAMELEON)
            if (sak == 0x08 || sak == 0x18 || sak == 0x09) {
                chameleon_manager_set_attack_hooks(&nfc_ui_attack_hooks);
                chameleon_manager_set_progress_callback(mfc_dict_progress_cb, NULL);
                (void)chameleon_manager_mf1_read_classic_with_dict(false);
                // Refresh details text from CU cache
                nfc_refresh_cu_details_from_cache();
                // Ensure Save button is visible
                if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
                    lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
                    nfc_save_visible = true;
                    update_nfc_buttons_layout();
                    update_nfc_popup_selection();
                }
            }
#endif
        }
    }
    vTaskDelete(NULL);
}

static void nfc_save_cu_task(void *arg) {
    (void)arg;
    bool ok = false;
    // Prefer cached NTAG dump (works without tag present)
    if (chameleon_manager_has_cached_ntag_dump()) {
        ok = chameleon_manager_save_ntag_dump(NULL);
    } else if (chameleon_manager_last_scan_is_ntag()) {
        // Fallback: try to read now if possible
        if (chameleon_manager_read_ntag_card()) ok = chameleon_manager_save_ntag_dump(NULL);
    } else if (chameleon_manager_mf1_has_cache()) {
        ok = chameleon_manager_mf1_save_flipper_dump(NULL);
    } else {
        // Try reading Classic now, then save
        (void)chameleon_manager_mf1_read_classic_with_dict(false);
        if (chameleon_manager_mf1_has_cache()) ok = chameleon_manager_mf1_save_flipper_dump(NULL);
    }
    if (!ok) {
        ok = chameleon_manager_save_last_hf_scan(NULL);
    }
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(nfc_save_done_async, res); }
    else { lv_async_call(nfc_save_done_async, NULL); }
    nfc_save_in_progress = false;
    vTaskDelete(NULL);
}

#ifdef CONFIG_NFC_PN532
static void nfc_scan_task(void *arg) {
    const char *TAGT = "NFCScan";
    ESP_LOGI(TAGT, "scan_task: start (cancel=%d)", nfc_scan_cancel);
    mfc_set_attack_hooks(&nfc_ui_attack_hooks);
    if (g_pn532 == NULL) {
        g_pn532 = &g_pn532_instance;
        // Prefer a single I2C controller for all devices sharing the same pins.
        // Match the Fuel Gauge manager's chosen port by target to avoid two controllers
        // driving the same physical SDA/SCL.
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        // Use I2C_NUM_0 exclusively to share controller with fuel gauge
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #elif defined(CONFIG_IDF_TARGET_ESP32C5)
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #elif defined(I2C_NUM_1)
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_1 };
    #else
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #endif
        bool ok = false;
        for (int pi = 0; pi < 2 && !ok; ++pi) {
            i2c_port_t port = try_ports[pi];
            ESP_LOGI(TAGT, "attempting PN532 on I2C port %d", (int)port);
            if (pn532_new_driver_i2c(
                    (gpio_num_t)CONFIG_NFC_SDA_PIN,
                    (gpio_num_t)CONFIG_NFC_SCL_PIN,
                    (gpio_num_t)CONFIG_NFC_RST_PIN,
                    (gpio_num_t)CONFIG_NFC_IRQ_PIN,
                    port,
                    g_pn532) != ESP_OK) {
                ESP_LOGE(TAGT, "pn532_new_driver_i2c failed (port=%d)", (int)port);
                pn532_delete_driver(g_pn532);
                continue;
            }
            if (pn532_init(g_pn532) == ESP_OK) {
                pn532_set_passive_activation_retries(g_pn532, 0xFF);
                ESP_LOGI(TAGT, "scan_task: PN532 initialized on port %d", (int)port);
                ok = true;
            } else {
                ESP_LOGE(TAGT, "pn532_init failed (port=%d)", (int)port);
                pn532_release(g_pn532);
                pn532_delete_driver(g_pn532);
            }
        }
        if (!ok) {
            ESP_LOGE(TAGT, "PN532 init failed on all ports, running i2c scan then exiting");
            nfc_scan_task_handle = NULL;
            vTaskDelete(NULL);
        }
    }

    while (!nfc_scan_cancel) {
        uint8_t uid[8] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0; uint8_t sak = 0;
        esp_err_t r = pn532_read_passive_target_id_ex(g_pn532, 0x00, uid + 1, &uid_len, &atqa, &sak, 200);
        if (r == ESP_OK && uid_len > 0 && uid_len <= 7) {
            uid[0] = uid_len;
            nfc_uid_evt_t *ev = nfc_uid_pool_alloc();
            if (ev) {
                ev->session = nfc_scan_session;
                ev->uid_len = uid_len;
                if (uid_len > sizeof(ev->uid)) ev->uid_len = sizeof(ev->uid);
                memcpy(ev->uid, uid + 1, ev->uid_len);
                if (display_manager_is_available()) lv_async_call(nfc_update_labels_async, ev);
                else nfc_uid_pool_free(ev);
            }
            if (nfc_scan_cancel) break;
            g_uid_len = uid_len; memcpy(g_uid, uid + 1, uid_len); g_atqa = atqa; g_sak = sak; g_model = NTAG2XX_UNKNOWN;
            ESP_LOGI(TAGT, "scan_task: UID found, building details (len=%u)", uid_len);
            nfc_build_and_set_details(g_pn532, uid + 1, uid_len);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (g_pn532) {
        if (nfc_scan_cancel) {
            ESP_LOGI(TAGT, "scan_task: releasing PN532 (cancel=%d)", nfc_scan_cancel);
            pn532_release(g_pn532);
            pn532_delete_driver(g_pn532);
            g_pn532 = NULL;
        } else {
            ESP_LOGI(TAGT, "scan_task: keeping PN532 for Save (cancel=%d)", nfc_scan_cancel);
        }
    }
    nfc_scan_task_handle = NULL;
    ESP_LOGI(TAGT, "scan_task: exit");
    vTaskDelete(NULL);
}
#endif

static void execute_selected(void) { /* no bottom status text in this view */ }

static void init_styles(void) {
    if (styles_initialized) return;
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item, 0);
    lv_style_set_radius(&style_menu_item, 0);

    lv_style_init(&style_menu_item_alt);
    lv_style_set_bg_color(&style_menu_item_alt, lv_color_hex(0x232323));
    lv_style_set_bg_opa(&style_menu_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item_alt, 0);
    lv_style_set_radius(&style_menu_item_alt, 0);

    lv_style_init(&style_selected_item);
    lv_style_set_bg_color(&style_selected_item, lv_color_hex(0x3A3A3A));
    lv_style_set_bg_opa(&style_selected_item, LV_OPA_COVER);
    lv_style_set_border_width(&style_selected_item, 0);
    lv_style_set_radius(&style_selected_item, 0);

    styles_initialized = true;
}

static const lv_font_t* get_menu_font(void) {
    return (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
}

static void vertically_center_label(lv_obj_t *label, lv_obj_t *btn) {
    if (!label) return;
    lv_coord_t btn_h = lv_obj_get_height(btn ? btn : lv_obj_get_parent(label));
    lv_coord_t label_h = lv_obj_get_height(label);
    lv_coord_t btn_y_center_pad = (btn_h - label_h) / 2;
    if (btn_y_center_pad < 0) btn_y_center_pad = 0;
    lv_obj_set_style_pad_top(label, btn_y_center_pad, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

static void update_selected_style_from_theme(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t theme_bg = lv_color_hex(theme_palettes[theme][0]);
    lv_style_set_bg_color(&style_selected_item, theme_bg);
    lv_style_set_bg_grad_dir(&style_selected_item, LV_GRAD_DIR_NONE);
    lv_style_set_bg_grad_color(&style_selected_item, theme_bg);
}

static void highlight_selected(void) {
    if (!menu_container) return;
    for (int i = 0; i < num_items; ++i) {
        lv_obj_t *child = lv_obj_get_child(menu_container, i);
        if (!child) continue;
        lv_obj_t *label = lv_obj_get_child(child, 0);
        if (i == selected_index) {
            uint8_t theme = settings_get_menu_theme(&G_Settings);
            if (theme >= 15) theme = 0;
            lv_color_t accent = lv_color_hex(theme_palettes[theme][0]);
            lv_obj_set_style_bg_color(child, accent, LV_PART_MAIN);
            if (label) {
                if (theme == 3) lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
                else lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_scroll_to_view(child, LV_ANIM_OFF);
        } else {
            bool zebra = settings_get_zebra_menus_enabled(&G_Settings);
            uint32_t base = zebra && (i % 2) ? 0x232323 : 0x1E1E1E;
            lv_obj_set_style_bg_color(child, lv_color_hex(base), LV_PART_MAIN);
            if (label) lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

// forward declare back_event_cb so it can be used before its definition
static void back_event_cb(lv_event_t *e);
// forward declare option dispatcher used by multiple input paths
void nfc_option_event_cb(lv_event_t *e);

void nfc_view_input_cb(InputEvent *event) {
    if (!root) return;
    // Handle NFC scan popup input first
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return; // handle on release for consistency with this view
            // Cancel button
            if (nfc_scan_cancel_btn && lv_obj_is_valid(nfc_scan_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_scan_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) {
                    nfc_scan_cancel_cb(NULL);
                    return;
                }
            }
            // More button (if visible)
            if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
                lv_area_t b; lv_obj_get_coords(nfc_scan_more_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) {
                    nfc_scan_more_cb(NULL);
                    return;
                }
            }
            // Right action button: Scroll in parsed view, otherwise Save
            if (nfc_details_view_mode == 2) {
                if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn) &&
                    !lv_obj_has_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN)) {
                    lv_area_t c; lv_obj_get_coords(nfc_scan_scroll_btn, &c);
                    if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) {
                        nfc_scan_scroll_cb(NULL);
                        return;
                    }
                }
            } else if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
                lv_area_t c; lv_obj_get_coords(nfc_scan_save_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) {
                    nfc_scan_save_cb(NULL);
                    return;
                }
            }
            update_nfc_popup_selection();
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the NFC scan popup
            nfc_scan_cancel_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (ji == 0) { // left
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                update_nfc_popup_selection();
            } else if (ji == 3) { // right
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                update_nfc_popup_selection();
            } else if (ji == 1) { // select/press
                if (nfc_popup_selected == 0) {
                    nfc_scan_cancel_cb(NULL);
                } else if (nfc_more_visible && nfc_popup_selected == 1) {
                    nfc_scan_more_cb(NULL);
                } else {
                    // Right button: either Save or Scroll
                    if (nfc_details_view_mode == 2) nfc_scan_scroll_cb(NULL);
                    else if (nfc_save_visible) nfc_scan_save_cb(NULL);
                }
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else {
                    if (nfc_details_view_mode == 2) nfc_scan_scroll_cb(NULL);
                    else if (nfc_save_visible) nfc_scan_save_cb(NULL);
                }
                return;
            }
            // Rotation toggles selection when More is available
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (event->data.encoder.direction != 0 && total > 1) {
                // invert encoder rotation: clockwise (direction>0) should move selection up
                if (event->data.encoder.direction > 0) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                else nfc_popup_selected = (nfc_popup_selected + 1) % total;
            }
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (kv == 9) {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total; else nfc_popup_selected = 0;
                update_nfc_popup_selection();
            } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                update_nfc_popup_selection();
            } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                update_nfc_popup_selection();
            } else if (kv == 's' || kv == 'S') {
                if (nfc_save_visible) { nfc_scan_save_cb(NULL); return; }
            } else if (kv == 'm' || kv == 'M') {
                if (nfc_more_visible) { nfc_scan_more_cb(NULL); return; }
            } else if (kv == 13 || kv == 10) {
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else if (nfc_save_visible && ((nfc_more_visible && nfc_popup_selected == 2) || (!nfc_more_visible && nfc_popup_selected == 1))) nfc_scan_save_cb(NULL);
                return;
            } else if (kv == 27 || kv == 'c' || kv == 'C') {
                nfc_scan_cancel_cb(NULL);
                return;
            } else {
                update_nfc_popup_selection();
            }
        }
        return; // consume input while popup is open
    }
    // Handle saved details popup input
    if (saved_popup && lv_obj_is_valid(saved_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) {
                lv_area_t a; lv_obj_get_coords(saved_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { saved_close_cb(NULL); return; }
            }
            if (saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) {
                lv_area_t b; lv_obj_get_coords(saved_rename_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { saved_rename_cb(NULL); return; }
            }
            if (saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) {
                lv_area_t c; lv_obj_get_coords(saved_delete_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) { saved_delete_cb(NULL); return; }
            }
            update_saved_popup_selection();
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the saved details popup
            saved_close_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 0) { // left
                saved_popup_selected = (saved_popup_selected + 3 - 1) % 3;
                update_saved_popup_selection();
            } else if (ji == 3) { // right
                saved_popup_selected = (saved_popup_selected + 1) % 3;
                update_saved_popup_selection();
            } else if (ji == 1) { // select
                if (saved_popup_selected == 0) saved_close_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (saved_popup_selected == 0) saved_close_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            }
            if (event->data.encoder.direction != 0) {
                // invert encoder rotation: clockwise (direction>0) moves selection up
                if (event->data.encoder.direction > 0) saved_popup_selected = (saved_popup_selected + 3 - 1) % 3;
                else saved_popup_selected = (saved_popup_selected + 1) % 3;
                update_saved_popup_selection();
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 9) { saved_popup_selected = (saved_popup_selected + 1) % 3; update_saved_popup_selection(); }
            else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') { saved_popup_selected = (saved_popup_selected + 3 - 1) % 3; update_saved_popup_selection(); }
            else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') { saved_popup_selected = (saved_popup_selected + 1) % 3; update_saved_popup_selection(); }
            else if (kv == 13 || kv == 10) {
                if (saved_popup_selected == 0) saved_close_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            } else if (kv == 27 || kv == 'c' || kv == 'C') { saved_close_cb(NULL); return; }
        }
        return;
    }
    // Handle keys popup input
    if (keys_popup && lv_obj_is_valid(keys_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (keys_close_btn && lv_obj_is_valid(keys_close_btn)) {
                lv_area_t a; lv_obj_get_coords(keys_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { keys_close_cb(NULL); return; }
            }
            update_keys_popup_selection();
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the keys popup
            keys_close_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            // left/right switch focus Up <-> Close <-> Down, press activates
            if (ji == 0) { // left
                keys_popup_selected = (keys_popup_selected + 3 - 1) % 3;
                update_keys_popup_selection();
            } else if (ji == 3) { // right
                keys_popup_selected = (keys_popup_selected + 1) % 3;
                update_keys_popup_selection();
            } else if (ji == 1) { // press
                if (keys_popup_selected == 0) keys_scroll_up_cb(NULL);
                else if (keys_popup_selected == 1) { keys_close_cb(NULL); return; }
                else keys_scroll_down_cb(NULL);
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                // activate focused button: 0=Up,1=Close,2=Down
                if (keys_popup_selected == 0) keys_scroll_up_cb(NULL);
                else if (keys_popup_selected == 1) { keys_close_cb(NULL); return; }
                else keys_scroll_down_cb(NULL);
            }
            if (event->data.encoder.direction != 0) {
                // invert encoder rotation: clockwise (direction>0) moves selection up
                if (event->data.encoder.direction > 0) keys_popup_selected = (keys_popup_selected + 3 - 1) % 3;
                else keys_popup_selected = (keys_popup_selected + 1) % 3;
                update_keys_popup_selection();
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 13 || kv == 10 || kv == 27 || kv == 'c' || kv == 'C') { keys_close_cb(NULL); return; }
            else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') { keys_scroll_up_cb(NULL); }
            else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') { keys_scroll_down_cb(NULL); }
        }
        return;
    }
    // Handle Chameleon Ultra popup input
    if (cu_popup && lv_obj_is_valid(cu_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) {
                lv_area_t a; lv_obj_get_coords(cu_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { cu_close_cb(NULL); return; }
            }
            if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) {
                lv_area_t b; lv_obj_get_coords(cu_connect_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { cu_connect_cb(NULL); return; }
            }
            if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) {
                lv_area_t c; lv_obj_get_coords(cu_disconnect_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) { cu_disconnect_cb(NULL); return; }
            }
            if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn)) {
                lv_area_t r; lv_obj_get_coords(cu_reader_btn, &r);
                if (d->point.x >= r.x1 && d->point.x <= r.x2 && d->point.y >= r.y1 && d->point.y <= r.y2) { cu_reader_cb(NULL); return; }
            }
            if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn)) {
                lv_area_t s; lv_obj_get_coords(cu_scan_hf_btn, &s);
                if (d->point.x >= s.x1 && d->point.x <= s.x2 && d->point.y >= s.y1 && d->point.y <= s.y2) { cu_scan_hf_cb(NULL); return; }
            }
            if (cu_save_visible && cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn)) {
                lv_area_t sv; lv_obj_get_coords(cu_save_hf_btn, &sv);
                if (d->point.x >= sv.x1 && d->point.x <= sv.x2 && d->point.y >= sv.y1 && d->point.y <= sv.y2) { cu_save_hf_cb(NULL); return; }
            }
            update_cu_popup_selection();
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) { cu_close_cb(NULL); return; }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            lv_obj_t *btns[8];
            int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (cu_more_btn && lv_obj_is_valid(cu_more_btn)) btns[total++] = cu_more_btn;
            if (cu_more_expanded && chameleon_manager_is_connected()) {
                if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn) && !lv_obj_has_flag(cu_reader_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_reader_btn;
                if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn) && !lv_obj_has_flag(cu_scan_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_scan_hf_btn;
                if (cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn) && !lv_obj_has_flag(cu_save_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_save_hf_btn;
            }
            if (ji == 0) { if (total > 1) { cu_popup_selected = (cu_popup_selected + total - 1) % total; update_cu_popup_selection(); } }
            else if (ji == 3) { if (total > 1) { cu_popup_selected = (cu_popup_selected + 1) % total; update_cu_popup_selection(); } }
            else if (ji == 1) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            lv_obj_t *btns[8]; int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (cu_more_btn && lv_obj_is_valid(cu_more_btn)) btns[total++] = cu_more_btn;
            if (cu_more_expanded && chameleon_manager_is_connected()) {
                if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn) && !lv_obj_has_flag(cu_reader_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_reader_btn;
                if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn) && !lv_obj_has_flag(cu_scan_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_scan_hf_btn;
                if (cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn) && !lv_obj_has_flag(cu_save_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_save_hf_btn;
            }
            if (event->data.encoder.button) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            }
            if (event->data.encoder.direction != 0 && total > 1) {
                if (event->data.encoder.direction > 0) cu_popup_selected = (cu_popup_selected + total - 1) % total;
                else cu_popup_selected = (cu_popup_selected + 1) % total;
            }
            update_cu_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            lv_obj_t *btns[8]; int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (kv == 9) {
                if (total > 1) cu_popup_selected = (cu_popup_selected + 1) % total;
                update_cu_popup_selection();
            } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') {
                if (total > 1) cu_popup_selected = (cu_popup_selected + total - 1) % total;
                update_cu_popup_selection();
            } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') {
                if (total > 1) cu_popup_selected = (cu_popup_selected + 1) % total;
                update_cu_popup_selection();
            } else if (kv == 13 || kv == 10) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            } else if (kv == 27 || kv == 'c' || kv == 'C') { cu_close_cb(NULL); return; }
        }
        return;
    }

    // Handle NFC write popup input
    if (nfc_write_popup && lv_obj_is_valid(nfc_write_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (nfc_write_cancel_btn && lv_obj_is_valid(nfc_write_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_write_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { nfc_write_cancel_cb(NULL); return; }
            }
            if (nfc_write_go_btn && lv_obj_is_valid(nfc_write_go_btn)) {
                lv_area_t b; lv_obj_get_coords(nfc_write_go_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { nfc_write_go_cb(NULL); return; }
            }
            update_nfc_write_popup_selection();
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the NFC write popup
            nfc_write_cancel_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 0 || ji == 3) {
                nfc_write_popup_selected = (nfc_write_popup_selected ^ 1);
                update_nfc_write_popup_selection();
            } else if (ji == 1) {
                if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) { if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL); return; }
            if (event->data.encoder.direction != 0) { nfc_write_popup_selected = (nfc_write_popup_selected ^ 1); }
            update_nfc_write_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 9) { nfc_write_popup_selected = (nfc_write_popup_selected ^ 1); update_nfc_write_popup_selection(); }
            else if (kv == 13 || kv == 10) { if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL); return; }
            else if (kv == 27 || kv == 'c' || kv == 'C') { nfc_write_cancel_cb(NULL); return; }
        }
        return;
    }
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) return; // handle only on release
        int x = d->point.x;
        int y = d->point.y;
        // check buttons
        for (int i = 0; i < num_items; ++i) {
            lv_obj_t *btn = lv_obj_get_child(menu_container, i);
            if (!btn) continue;
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                selected_index = i;
                highlight_selected();
                lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                return;
            }
        }
        // touch outside -> back
        if (in_write_list) back_to_root_menu(); else if (in_saved_list) back_to_root_menu(); else display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 2) { // up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (btn == 4) { // down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (btn == 1) { // select
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
            } else {
                execute_selected();
            }
        } else if (btn == 0) { // back
            if (in_write_list) back_to_root_menu(); else if (in_saved_list) back_to_root_menu(); else display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_index);
            if (sel) {
                lv_event_send(sel, LV_EVENT_CLICKED, NULL);
            } else execute_selected();
        } else {
            if (event->data.encoder.direction > 0)
                selected_index = (selected_index + 1) % num_items;
            else
                selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13) { // Enter
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) {
                lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
            } else execute_selected();
        } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') { // up
            selected_index = (selected_index - 1 + num_items) % num_items;
            highlight_selected();
        } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') { // down
            selected_index = (selected_index + 1) % num_items;
            highlight_selected();
        } else if (kv == 29 || kv == '`') { // Esc
            if (in_write_list) back_to_root_menu(); else if (in_saved_list) back_to_root_menu(); else display_manager_switch_view(&main_menu_view);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (in_write_list) back_to_root_menu(); else if (in_saved_list) back_to_root_menu(); else display_manager_switch_view(&main_menu_view);
#endif
    }
}

void nfc_option_event_cb(lv_event_t *e) {
    // user_data is const char* option
    const char *opt = (const char *)lv_event_get_user_data(e);
    if (!opt) return;
    if (strcmp(opt, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        return;
    }

    if (strcmp(opt, "Scan") == 0) {
        create_nfc_scan_popup();
        return;
    }

    if (strcmp(opt, "Write") == 0) {
        nfc_enter_write_list();
        return;
    }

    if (strcmp(opt, "Saved") == 0) {
        saved_enter_list();
        return;
    }

    if (strcmp(opt, "User Keys") == 0) {
        create_keys_popup();
        return;
    }

    if (strcmp(opt, "Chameleon Ultra") == 0) {
        create_cu_popup();
        return;
    }


}

// touchscreen scroll callbacks
static void scroll_nfc_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}
static void scroll_nfc_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}
static void back_event_cb(lv_event_t *e) {
    if (in_write_list || in_saved_list) back_to_root_menu();
    else display_manager_switch_view(&main_menu_view);
}

lv_style_t* get_zebra_style(int index) {
    if (settings_get_zebra_menus_enabled(&G_Settings)) return (index % 2 == 0) ? &style_menu_item : &style_menu_item_alt;
    return &style_menu_item;
}

void cleanup_nfc_scan_popup(void *obj) {
#ifdef CONFIG_NFC_PN532
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: begin (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
#else
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: begin");
#endif
    if (nfc_scan_popup) {
        lv_obj_del(nfc_scan_popup);
        nfc_scan_popup = NULL;
        nfc_btn_bar = NULL;
        nfc_scan_cancel_btn = NULL;
        nfc_scan_more_btn = NULL;
        nfc_scan_save_btn = NULL;
        nfc_title_label = NULL;
        nfc_uid_label = NULL;
        nfc_type_label = NULL;
        nfc_details_label = NULL;
        nfc_details_scroll = NULL;
    }
    // restore bottom nav buttons on touch builds
#ifdef CONFIG_USE_TOUCHSCREEN
    if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
    if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    if (back_btn && lv_obj_is_valid(back_btn)) lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef CONFIG_NFC_PN532
    // Signal the scan task to exit gracefully (avoid calling LVGL from that task)
    nfc_scan_cancel = true;
    // Always restore normal I2C activity on popup close to avoid UI/input issues on some boards
    display_manager_set_low_i2c_mode(false);
#ifdef CONFIG_HAS_FUEL_GAUGE
    // Pause fuel gauge while the NFC scan task winds down to avoid I2C contention
    fuel_gauge_manager_set_paused(true);
#endif
    // If a deferred retry timer exists, delete it now (legacy cleanup)
    if (nfc_scan_retry_timer) {
        lv_timer_del(nfc_scan_retry_timer);
        nfc_scan_retry_timer = NULL;
    }
    // Wait briefly for the scan task to exit and release PN532 itself
    uint32_t waited_ms = 0;
    while (nfc_scan_task_handle != NULL && waited_ms < 800) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (nfc_scan_task_handle != NULL) {
        ESP_LOGW(TAG, "cleanup_nfc_scan_popup: scan task still running after %ums (skipping force delete)", (unsigned)waited_ms);
    }
    // If for any reason PN532 is still held here and the task has exited, release as a safety net
    if (nfc_scan_task_handle == NULL && g_pn532) {
        ESP_LOGI(TAG, "cleanup_nfc_scan_popup: releasing PN532 resources (post-exit)");
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
    nfc_detected_title[0] = '\0';
    // Clear paused/cache flags so a new scan doesn't inherit a stale state
    nfc_paused = false;
    nfc_cache_fill_phase = false;
    // Synchronously update UI to clear any paused state immediately
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "Cancelled");
    }
    mfc_set_progress_callback(NULL, NULL);
#endif
#ifdef CONFIG_NFC_PN532
    // Resume fuel gauge after task has had time to stop
#ifdef CONFIG_HAS_FUEL_GAUGE
    fuel_gauge_manager_set_paused(false);
#endif
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: end (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
#else
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: end");
#endif
}

static void nfc_scan_cancel_cb(lv_event_t *e) {
    cleanup_nfc_scan_popup(NULL);
}

static void nfc_scan_scroll_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI("NFC", "Scroll button pressed");
    if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) {
        ESP_LOGW("NFC", "Scroll container invalid or null");
        return;
    }
    lv_obj_t *scroller = nfc_details_scroll;

    lv_coord_t h = lv_obj_get_height(scroller);
    lv_coord_t y_before = lv_obj_get_scroll_y(scroller);
    lv_coord_t step = (h > 40) ? (h - 40) : (h / 2);
    if (step < 10) step = 10;

    ESP_LOGI("NFC", "Scroll before: y=%d, h=%d, step=%d", y_before, h, step);

    lv_obj_scroll_by_bounded(scroller, 0, -step, LV_ANIM_OFF);

    lv_coord_t y_after = lv_obj_get_scroll_y(scroller);
    ESP_LOGI("NFC", "Scroll after: y=%d", y_after);

    if (y_after == y_before) {
        ESP_LOGI("NFC", "Reached bottom or no scroll possible; wrapping to top");
        lv_obj_scroll_to_y(scroller, 0, LV_ANIM_ON);
    }
}

static void nfc_scan_more_cb(lv_event_t *e) {
    (void)e;
    // If bruteforcing is active, treat More as Skip (basic read)
    if (!nfc_dict_skip_requested && mfc_phase_sector >= 0) {
        nfc_dict_skip_requested = true;
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "Basic read (skipping dict) ...");
        }
        // Update button to reflect action taken
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Skipping...");
            lv_obj_add_state(nfc_scan_more_btn, LV_STATE_DISABLED);
        }
        return;
    }
    
    // Toggle/Cycle details view
    #if defined(CONFIG_NFC_CHAMELEON)
    if (using_chameleon_backend() && !nfc_details_visible) {
        nfc_refresh_cu_details_from_cache();
    }
    #endif

    if (nfc_details_view_mode == 0) {
        // Summary -> Basic
        nfc_details_view_mode = 1;
        nfc_show_details_view(true);
    } else if (nfc_details_view_mode == 1) {
        // Basic -> Full (if available)
        if (has_extra_details(nfc_details_text)) {
            nfc_details_view_mode = 2;
            nfc_show_details_view(true);
        } else {
            // No extra details, go to Summary
            nfc_details_view_mode = 0;
            nfc_show_details_view(false);
        }
    } else {
        // Full -> Summary
        nfc_details_view_mode = 0;
        nfc_show_details_view(false);
    }
}

static void nfc_scan_save_cb(lv_event_t *e) {
    if (nfc_save_in_progress) return;
    nfc_save_in_progress = true;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "Saving...");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_add_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
#if defined(CONFIG_NFC_CHAMELEON)
    if (using_chameleon_backend()) {
        BaseType_t rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 6144, NULL, 5, NULL);
        if (rc != pdPASS) rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 4096, NULL, 5, NULL);
        if (rc != pdPASS) rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 3072, NULL, 5, NULL);
        if (rc != pdPASS) {
            nfc_save_in_progress = false;
            if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "NFC Tag");
            if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
            ESP_LOGE(TAG, "nfc_save_cu_task create failed");
        }
        return;
    }
#endif
#ifdef CONFIG_NFC_PN532
    mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
    BaseType_t rc = xTaskCreate(nfc_save_task, "nfc_save", 6144, NULL, 5, NULL);
    if (rc != pdPASS) rc = xTaskCreate(nfc_save_task, "nfc_save", 4096, NULL, 5, NULL);
    if (rc != pdPASS) {
        nfc_save_in_progress = false;
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "NFC Tag");
        if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
        mfc_set_progress_callback(NULL, NULL);
        ESP_LOGE(TAG, "nfc_save_task create failed");
    }
#endif
}

static bool write_flipper_nfc_file(void) {
    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    sd_card_create_directory(dir);
#ifdef CONFIG_NFC_PN532
    if (g_uid_len == 0 || g_pn532 == NULL) {
        ESP_LOGW(TAG, "No NFC UID/driver to save");
        return false;
    }

    // Build filename: <Model>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < g_uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_uid[i]);
        if (i + 1 < g_uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];

    if (mfc_is_classic_sak(g_sak)) {
        // Prefer cached save (io=NULL) so user can save without card present
        bool ok = mfc_save_flipper_file(NULL, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        if (!ok && g_pn532) {
            ESP_LOGW(TAG, "Offline save failed; retrying with live PN532");
            ok = mfc_save_flipper_file(g_pn532, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        }
        if (!ok) {
            ESP_LOGE(TAG, "Failed to save Mifare Classic file");
            return false;
        }
        ESP_LOGI(TAG, "Mifare Classic file saved");
        return true;
    }

    // Non-Classic (NTAG/Ultralight) path
    const char *model_str = ntag_t2_model_str(g_model);
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", dir, model_str, uid_part);

    // Determine total pages by model (NTAG/Ultralight)
    int pages_total = 0;
    switch (g_model) {
        case NTAG2XX_NTAG213: pages_total = 45; break;
        case NTAG2XX_NTAG215: pages_total = 135; break;
        case NTAG2XX_NTAG216: pages_total = 231; break;
        default: pages_total = 135; break;
    }

    // Header
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < g_uid_len && pos < (int)sizeof(buf) - 4; ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", g_uid[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (g_atqa >> 8) & 0xFF, g_atqa & 0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", g_sak);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "NTAG/Ultralight type: %s\n", model_str);

    if (sd_card_write_file(path, buf, (size_t)pos) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header: %s", path);
        return false;
    }

    // batch metadata writes: signature, version, counters
    char *meta = NULL; size_t meta_cap = 512; size_t mpos = 0;
    meta = (char*)malloc(meta_cap);
    if (!meta) {
        // fallback: original per-section appends
        uint8_t sig[32];
        if (ntag2xx_read_signature(g_pn532, sig) != ESP_OK) memset(sig, 0, sizeof(sig));
        pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Signature:");
        for (int i = 0; i < 32 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", sig[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
        sd_card_append_file(path, buf, (size_t)pos);
        uint8_t ver[8]; if (ntag2xx_get_version(g_pn532, ver) != ESP_OK) memset(ver, 0, sizeof(ver));
        pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare version:");
        for (int i = 0; i < 8 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", ver[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
        sd_card_append_file(path, buf, (size_t)pos);
        for (int ci = 0; ci < 3; ++ci) {
            uint32_t cv = 0; uint8_t tr = 0;
            esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv);
            esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
            pos = 0; pos += snprintf(buf + pos, sizeof(buf) - pos, "Counter %d: %u\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0);
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Tearing %d: %02X\n", ci, (ert == ESP_OK) ? tr : 0);
            sd_card_append_file(path, buf, (size_t)pos);
        }
    } else {
        // signature
        uint8_t sig[32];
        if (ntag2xx_read_signature(g_pn532, sig) != ESP_OK) memset(sig, 0, sizeof(sig));
        int w = snprintf(NULL, 0, "Signature:");
        if (mpos + (size_t)w + 1 > meta_cap) { size_t nc = meta_cap * 2 + (size_t)w + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        memcpy(meta + mpos, "Signature:", (size_t)w); mpos += (size_t)w;
        for (int i = 0; i < 32; ++i) {
            char tmp[4]; int tp = snprintf(tmp, sizeof(tmp), " %02X", sig[i]);
            if (mpos + (size_t)tp + 1 > meta_cap) { size_t nc = meta_cap * 2 + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, tmp, (size_t)tp); mpos += (size_t)tp;
        }
        if (mpos + 1 > meta_cap) { size_t nc = meta_cap + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        meta[mpos++] = '\n';
        // version
        uint8_t ver[8]; if (ntag2xx_get_version(g_pn532, ver) != ESP_OK) memset(ver, 0, sizeof(ver));
        w = snprintf(NULL, 0, "Mifare version:");
        if (mpos + (size_t)w + 1 > meta_cap) { size_t nc = meta_cap * 2 + (size_t)w + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        memcpy(meta + mpos, "Mifare version:", (size_t)w); mpos += (size_t)w;
        for (int i = 0; i < 8; ++i) {
            char tmp[4]; int tp = snprintf(tmp, sizeof(tmp), " %02X", ver[i]);
            if (mpos + (size_t)tp + 1 > meta_cap) { size_t nc = meta_cap * 2 + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, tmp, (size_t)tp); mpos += (size_t)tp;
        }
        if (mpos + 1 > meta_cap) { size_t nc = meta_cap + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        meta[mpos++] = '\n';
        // counters and tearing flags
        for (int ci = 0; ci < 3; ++ci) {
            uint32_t cv = 0; uint8_t tr = 0;
            esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv);
            esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
            char line[64];
            int lp = snprintf(line, sizeof(line), "Counter %d: %u\nTearing %d: %02X\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0, ci, (ert == ESP_OK) ? tr : 0);
            if (mpos + (size_t)lp > meta_cap) { size_t nc = meta_cap * 2 + (size_t)lp + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, line, (size_t)lp); mpos += (size_t)lp;
        }
        if (mpos > 0) sd_card_append_file(path, meta, mpos);
        free(meta); meta = NULL;
    }
meta_fallback: ;

    // Read all pages and build page dump
    size_t cap = (size_t)pages_total * 48 + 64;
    char *pages = (char*)malloc(cap);
    if (!pages) {
        ESP_LOGE(TAG, "OOM building page dump");
        return false;
    }
    int ppos = 0; int pages_read = 0;
    for (int pg = 0; pg < pages_total; pg += 4) {
        uint8_t block[16] = {0};
        if (ntag2xx_read_page(g_pn532, (uint8_t)pg, block, 16) == ESP_OK) {
            int chunk = (pages_total - pg >= 4) ? 4 : (pages_total - pg);
            pages_read += chunk;
        }
        // format up to 4 pages from this block
        for (int off = 0; off < 4 && pg + off < pages_total; ++off) {
            uint8_t *data = &block[off * 4];
            ppos += snprintf(pages + ppos, cap - ppos, "Page %d: %02X %02X %02X %02X\n",
                             pg + off, data[0], data[1], data[2], data[3]);
            if (ppos >= (int)cap - 64) break;
        }
        if (ppos >= (int)cap - 64) break;
    }

    // Pages meta then pages dump
    pos = snprintf(buf, sizeof(buf), "Pages total: %d\nPages read: %d\n", pages_total, pages_read);
    sd_card_append_file(path, buf, (size_t)pos);
    sd_card_append_file(path, pages, (size_t)ppos);
    free(pages);

    // Footer
    const char *footer = "Failed authentication attempts: 0\n";
    sd_card_append_file(path, footer, strlen(footer));

    ESP_LOGI(TAG, "NFC file saved: %s", path);
    if (did) nfc_sd_end(susp);
    return true;
#else
    ESP_LOGW(TAG, "NFC not enabled; nothing to save");
    return false;
#endif
}

static void create_nfc_scan_popup(void) {
    ESP_LOGI(TAG, "create_nfc_scan_popup");
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        cleanup_nfc_scan_popup(NULL);
    }
    if (!root || !lv_obj_is_valid(root)) return;
    // We'll reset the cancel flag right before (re)starting the scan task
    nfc_dict_skip_requested = false;
    // Ensure fresh UI state (avoid showing stale paused/cache states)
    nfc_paused = false;
    nfc_cache_fill_phase = false;
    nfc_details_ready = false;
    // New scan session: invalidate any stale async events from prior scan
    nfc_scan_session++;
    // scale to screen, leave margin for edges
    int popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 0;
    
    if (LV_VER_RES <= 135) {
        // Cardputer: maximize vertical space usage
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 190) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 110) popup_h = 110;
        y_offset = 10; // Account for status bar
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 160;
        y_offset = 10; // Account for status bar
    }
    nfc_scan_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    // Fonts
    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    nfc_title_label = popup_create_title_label(nfc_scan_popup, "Scanning NFC...", title_font, 22);

    // Placeholder fields (UID / Type)
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    nfc_uid_label = popup_create_body_label(nfc_scan_popup, "UID: -- -- -- -- -- -- -- --", 0, false, body_font, 40);
    if (nfc_uid_label) lv_obj_set_style_text_color(nfc_uid_label, lv_color_hex(0xCCCCCC), 0);

    nfc_type_label = popup_create_body_label(nfc_scan_popup, "Type: --", 0, false, body_font, 60);
    if (nfc_type_label) lv_obj_set_style_text_color(nfc_type_label, lv_color_hex(0xCCCCCC), 0);

    // Progress indicators removed; we will update the title and details text instead

    // Cancel button
    int btn_w = 90, btn_h = 34;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    nfc_scan_cancel_btn = popup_add_styled_button(nfc_scan_popup, "Cancel", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, nfc_scan_cancel_cb, NULL);

    // More button (hidden until a tag is scanned)
    nfc_scan_more_btn = popup_add_styled_button(nfc_scan_popup, "More", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, nfc_scan_more_cb, NULL);
    if (nfc_scan_more_btn) lv_obj_add_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);

    // Save button (hidden until a tag is scanned)
    nfc_scan_save_btn = popup_add_styled_button(nfc_scan_popup, "Save", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_scan_save_cb, NULL);
    if (nfc_scan_save_btn) lv_obj_add_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);

    // Scroll button (hidden until Parsed view)
    nfc_scan_scroll_btn = popup_add_styled_button(nfc_scan_popup, "Scroll", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_scan_scroll_cb, NULL);
    if (nfc_scan_scroll_btn) lv_obj_add_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);

    // Initial state: only cancel visible, centered
    nfc_more_visible = false;
    nfc_save_visible = false;
    nfc_popup_selected = 0;
    nfc_details_visible = false;
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
#if defined(CONFIG_NFC_CHAMELEON)
    if (chameleon_manager_is_ready()) {
        ESP_LOGI(TAG, "create_nfc_scan_popup: starting CU scan task");
        nfc_scan_cancel = false;
        xTaskCreate(nfc_scan_cu_task, "nfc_scan_cu", 4096, NULL, 5, NULL);
        return;
    }
#endif
#ifdef CONFIG_NFC_PN532
    // Since we force-delete stuck tasks in cleanup, we should never have a running task here
    if (nfc_scan_task_handle != NULL) {
        ESP_LOGE(TAG, "create_nfc_scan_popup: unexpected running task, force cleaning up");
        vTaskDelete(nfc_scan_task_handle);
        nfc_scan_task_handle = NULL;
        if (g_pn532) {
            pn532_release(g_pn532);
            pn532_delete_driver(g_pn532);
            g_pn532 = NULL;
        }
    }
    ESP_LOGI(TAG, "create_nfc_scan_popup: starting scan task");
    nfc_scan_cancel = false;
    mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
    xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
#endif
}

#ifdef CONFIG_NFC_PN532
static void nfc_try_start_scan_timer_cb(lv_timer_t *t) {
    if (nfc_scan_task_handle == NULL) {
        ESP_LOGI(TAG, "nfc_try_start_scan_timer_cb: starting scan task after prior exit");
        nfc_scan_cancel = false;
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
        if (t) {
            lv_timer_del(t);
            nfc_scan_retry_timer = NULL;
        }
    }
}
#endif

// Run heavy save on a worker task to avoid blocking LVGL thread
static void nfc_save_task(void *arg) {
    bool ok = write_flipper_nfc_file();
    // Notify UI on completion with result
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(nfc_save_done_async, res); }
    else { lv_async_call(nfc_save_done_async, NULL); }
    nfc_save_in_progress = false;
    vTaskDelete(NULL);
}

static void nfc_save_done_async(void *ptr) {
    bool ok = (ptr != NULL) ? *((bool*)ptr) : false;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, ok ? "Saved!" : "Save failed");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
#ifdef CONFIG_NFC_PN532
    mfc_set_progress_callback(NULL, NULL);
#endif
    if (ptr) nfc_bool_pool_free(ptr);
}

static void update_nfc_popup_selection(void) {
    if (!nfc_scan_cancel_btn) return;
    
    // Update Cancel button
    popup_set_button_selected(nfc_scan_cancel_btn, nfc_popup_selected == 0);
    
    // Update More button if visible
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn) && nfc_more_visible) {
        popup_set_button_selected(nfc_scan_more_btn, nfc_popup_selected == 1);
    }
    
    // Update right-side action button: Save (summary/basic) or Scroll (parsed view)
    int right_index = nfc_more_visible ? 2 : 1;
    if (nfc_details_view_mode != 2) {
        if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && nfc_save_visible) {
            popup_set_button_selected(nfc_scan_save_btn, nfc_popup_selected == right_index);
        }
    } else {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn) &&
            !lv_obj_has_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN)) {
            popup_set_button_selected(nfc_scan_scroll_btn, nfc_popup_selected == right_index);
        }
    }
    // Re-apply button layout after selection/style changes so sizes stay consistent
    update_nfc_buttons_layout();
}

static void update_nfc_buttons_layout(void) {
    if (!nfc_scan_cancel_btn || !nfc_scan_popup) return;

    int yoff = nfc_details_visible ? -8 : -10;
    lv_obj_t *btns[3];
    int count = 0;

    btns[count++] = nfc_scan_cancel_btn;

    if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        btns[count++] = nfc_scan_more_btn;
    }

    // In Parsed view (Mode 2), show Scroll button instead of Save
    if (nfc_details_view_mode == 2) {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn)) {
            lv_obj_clear_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);
            if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_add_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
            btns[count++] = nfc_scan_scroll_btn;
        }
    } else {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn)) lv_obj_add_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);
        if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
            lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
            btns[count++] = nfc_scan_save_btn;
        }
    }

    popup_layout_buttons_responsive(nfc_scan_popup, btns, count, yoff, NULL);
}

static void update_saved_buttons_layout(void) {
    if (!saved_close_btn || !saved_popup) return;

    lv_obj_t *btns[3];
    int count = 0;

    if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) btns[count++] = saved_close_btn;
    if (saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) btns[count++] = saved_rename_btn;
    if (saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) btns[count++] = saved_delete_btn;

    if (count == 0) return;

    PopupButtonLayoutConfig cfg = {0};
    cfg.min_threshold = 48; // keep legacy minimum for wide popups
    popup_layout_buttons_responsive(saved_popup, btns, count, -8, &cfg);
}

static void update_keys_buttons_layout(void) {
    if (!keys_close_btn || !keys_popup) return;

    lv_obj_t *btns[3];
    int count = 0;

    if (keys_up_btn && lv_obj_is_valid(keys_up_btn)) btns[count++] = keys_up_btn;
    if (keys_close_btn && lv_obj_is_valid(keys_close_btn)) btns[count++] = keys_close_btn;
    if (keys_down_btn && lv_obj_is_valid(keys_down_btn)) btns[count++] = keys_down_btn;

    if (count == 0) return;

    PopupButtonLayoutConfig cfg = {0};
    cfg.min_w = (LV_HOR_RES <= 240) ? 48 : 54;
    cfg.max_w = (LV_HOR_RES <= 240) ? 100 : 130;
    cfg.min_threshold = 40;
    popup_layout_buttons_responsive(keys_popup, btns, count, -8, &cfg);
}

static void nfc_update_details_scroll_layout(void) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) return;
    if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) return;
    if (!nfc_title_label || !lv_obj_is_valid(nfc_title_label)) return;
    if (!nfc_scan_cancel_btn || !lv_obj_is_valid(nfc_scan_cancel_btn)) return;

    lv_obj_update_layout(nfc_scan_popup);

    lv_area_t popup_a;
    lv_area_t title_a;
    lv_area_t btn_a;
    lv_obj_get_coords(nfc_scan_popup, &popup_a);
    lv_obj_get_coords(nfc_title_label, &title_a);
    lv_obj_get_coords(nfc_scan_cancel_btn, &btn_a);

    lv_coord_t popup_w = lv_obj_get_width(nfc_scan_popup);
    lv_coord_t popup_h = lv_obj_get_height(nfc_scan_popup);

    lv_coord_t top_y = title_a.y2 - popup_a.y1 + 2;
    if (top_y < 0) top_y = 0;

    lv_coord_t bottom_y = btn_a.y1 - 4;
    if (bottom_y > popup_h - 4) bottom_y = popup_h - 4;

    lv_coord_t scroll_h = bottom_y - top_y;
    if (scroll_h <= 0) return;

    const lv_font_t *font = NULL;
    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        font = lv_obj_get_style_text_font(nfc_details_label, LV_PART_MAIN);
    }
    lv_coord_t line_h = font ? lv_font_get_line_height(font) : 0;
    if (line_h > 0 && scroll_h > line_h) {
        // Make the viewport slightly shorter than the text height so scrolling always has effect
        scroll_h -= line_h;
    }

    lv_coord_t scroll_w = popup_w - 20;
    if (scroll_w < 20) scroll_w = popup_w;

    lv_obj_set_size(nfc_details_scroll, scroll_w, scroll_h);
    lv_obj_align(nfc_details_scroll, LV_ALIGN_TOP_MID, 0, top_y);

    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_obj_set_width(nfc_details_label, scroll_w - 4);
        lv_obj_align(nfc_details_label, LV_ALIGN_TOP_MID, 0, 0);
    }

    lv_obj_update_layout(nfc_details_scroll);
}

static void nfc_show_details_view(bool show) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) return;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    if (show) {
        // Hide summary fields
        if (nfc_uid_label) lv_obj_add_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_add_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        // Title and button label
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "NFC Details");
            // Details title slightly down from top for spacing
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 4);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            const char *btn_text = "Less";
            if (nfc_details_view_mode == 1) {
                // In basic mode, button leads to Parsed if available, else back to Summary (Less)
                if (has_extra_details(nfc_details_text)) btn_text = "Parsed";
            }
            if (lbl) lv_label_set_text(lbl, btn_text);
        }
        // Create details scroll container if needed
        if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) {
            lv_coord_t popup_w = lv_obj_get_width(nfc_scan_popup);
            lv_coord_t popup_h = lv_obj_get_height(nfc_scan_popup);
            lv_coord_t scroll_h = popup_h - 75; // Leave room for title and buttons
            if (scroll_h < 50) scroll_h = 50;
            nfc_details_scroll = popup_create_scroll_area(nfc_scan_popup, popup_w - 20, scroll_h, LV_ALIGN_TOP_MID, 0, 35);
        }

        // Create details label inside scroll container
        if (!nfc_details_label || !lv_obj_is_valid(nfc_details_label)) {
            lv_coord_t text_w = lv_obj_get_width(nfc_details_scroll) - 4;
            nfc_details_label = popup_create_body_label(nfc_details_scroll, "", text_w, true, body_font, 0);
        } else if (lv_obj_get_parent(nfc_details_label) != nfc_details_scroll) {
            // Reparent if label was created in a different view mode
            lv_obj_set_parent(nfc_details_label, nfc_details_scroll);
            lv_coord_t text_w = lv_obj_get_width(nfc_details_scroll) - 4;
            lv_obj_set_width(nfc_details_label, text_w);
        }

        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
            lv_obj_align(nfc_details_label, LV_ALIGN_TOP_MID, 0, 0);
            lv_label_set_long_mode(nfc_details_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
        }
        // Set details text
        const char *source_text = NULL;
        #if defined(CONFIG_NFC_CHAMELEON)
        if (using_chameleon_backend()) {
            nfc_refresh_cu_details_from_cache();
            source_text = nfc_details_text;
        } else
        #endif
        {
            #ifdef CONFIG_NFC_PN532
            source_text = nfc_details_text;
            #endif
        }

        const char *final_text = "Reading tag data...";
        char *tmp_text = NULL;

        if (nfc_details_ready && source_text) {
            final_text = source_text;
            // specific logic for mode 1 (Basic) truncation
            if (nfc_details_view_mode == 1) {
                const char *split = get_details_split_point(source_text);
                if (split) {
                    size_t len = split - source_text;
                    tmp_text = (char*)malloc(len + 1);
                    if (tmp_text) {
                        memcpy(tmp_text, source_text, len);
                        tmp_text[len] = '\0';
                        final_text = tmp_text;
                    }
                }
            }
            // specific logic for mode 2 (Parsed) - show only the tail
            else if (nfc_details_view_mode == 2) {
                const char *split = get_details_split_point(source_text);
                if (split) {
                    final_text = split;
                    // Remove header line (e.g. #SmartRider) if present
                    if (final_text[0] == '#') {
                        const char *nl = strchr(final_text, '\n');
                        if (nl) final_text = nl + 1;
                    }
                    // Trim leading newlines to remove extra gap
                    while (final_text[0] == '\n' || final_text[0] == '\r') {
                        final_text++;
                    }
                }
            }
        } else {
            #ifndef CONFIG_NFC_PN532
            #ifndef CONFIG_NFC_CHAMELEON
            final_text = "NFC not available";
            #endif
            #endif
        }

        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
            lv_label_set_text(nfc_details_label, final_text);
        }
        if (tmp_text) free(tmp_text);

        if (nfc_details_scroll && lv_obj_is_valid(nfc_details_scroll)) {
            lv_obj_clear_flag(nfc_details_scroll, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        nfc_details_visible = true;
        nfc_popup_selected = 1; // focus Less button
        update_nfc_popup_selection();
        nfc_update_details_scroll_layout();
    } else {
        nfc_details_view_mode = 0; // Reset mode when hiding
        // Hide details, show summary (keep spacing consistent for 240x320 too)
        if (nfc_details_scroll && lv_obj_is_valid(nfc_details_scroll)) lv_obj_add_flag(nfc_details_scroll, LV_OBJ_FLAG_HIDDEN);
        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) lv_obj_add_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_uid_label) lv_obj_clear_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_clear_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, nfc_get_detected_title());
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "More");
        }
        nfc_details_visible = false;
        nfc_popup_selected = 0; // focus Cancel
        update_nfc_popup_selection();
        // Update buttons layout for summary spacing
        update_nfc_buttons_layout();
    }
}

// ---- Write Flow Implementation ----
static bool has_nfc_ext(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + (len - 4);
    return (ext[0] == '.' && (ext[1] == 'n' || ext[1] == 'N') && (ext[2] == 'f' || ext[2] == 'F') && (ext[3] == 'c' || ext[3] == 'C'));
}

static void nfc_clear_write_list(void) {
    if (nfc_file_paths) {
        for (size_t i = 0; i < nfc_file_count; ++i) {
            free(nfc_file_paths[i]);
        }
        free(nfc_file_paths);
    }
    nfc_file_paths = NULL;
    nfc_file_count = 0;
}

static void nfc_file_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    create_nfc_write_popup(path);
}

static void back_to_root_menu(void) {
    if (!root || !menu_container) return;
    in_write_list = false;
    in_saved_list = false;
    nfc_clear_write_list();
    saved_clear_list();
    lv_obj_clean(menu_container);

    // rebuild Scan and Write rows (same as in create)
    // Add Scan button
    scan_btn = lv_list_add_btn(menu_container, NULL, "Scan");
    lv_obj_set_height(scan_btn, button_height_global);
    lv_obj_add_style(scan_btn, get_zebra_style(0), 0);
    lv_obj_t *slabel = lv_obj_get_child(scan_btn, 0);
    if (slabel) {
        lv_obj_set_style_text_font(slabel, get_menu_font(), 0);
        vertically_center_label(slabel, scan_btn);
        lv_obj_add_style(slabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(scan_btn, (void *)"Scan");
    lv_obj_add_event_cb(scan_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Scan");

    // Add Saved button
    lv_obj_t *saved_btn = lv_list_add_btn(menu_container, NULL, "Saved");
    lv_obj_set_height(saved_btn, button_height_global);
    lv_obj_add_style(saved_btn, get_zebra_style(1), 0);
    lv_obj_t *svlabel = lv_obj_get_child(saved_btn, 0);
    if (svlabel) {
        lv_obj_set_style_text_font(svlabel, get_menu_font(), 0);
        vertically_center_label(svlabel, saved_btn);
        lv_obj_add_style(svlabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(saved_btn, (void *)"Saved");
    lv_obj_add_event_cb(saved_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Saved");

    // Add User Keys button
    lv_obj_t *keys_btn = lv_list_add_btn(menu_container, NULL, "User Keys");
    lv_obj_set_height(keys_btn, button_height_global);
    lv_obj_add_style(keys_btn, get_zebra_style(2), 0);
    lv_obj_t *klabel = lv_obj_get_child(keys_btn, 0);
    if (klabel) {
        lv_obj_set_style_text_font(klabel, get_menu_font(), 0);
        vertically_center_label(klabel, keys_btn);
        lv_obj_add_style(klabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(keys_btn, (void *)"User Keys");
    lv_obj_add_event_cb(keys_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"User Keys");

    // Add Chameleon Ultra button
#if defined(CONFIG_NFC_CHAMELEON)
    lv_obj_t *cu_btn = lv_list_add_btn(menu_container, NULL, "Chameleon Ultra");
    lv_obj_set_height(cu_btn, button_height_global);
    lv_obj_add_style(cu_btn, get_zebra_style(3), 0);
    lv_obj_t *culabel = lv_obj_get_child(cu_btn, 0);
    if (culabel) {
        lv_obj_set_style_text_font(culabel, get_menu_font(), 0);
        vertically_center_label(culabel, cu_btn);
        lv_obj_add_style(culabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(cu_btn, (void *)"Chameleon Ultra");
    lv_obj_add_event_cb(cu_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Chameleon Ultra");
#endif

    // Add Write button
    emulate_btn = lv_list_add_btn(menu_container, NULL, "Write");
    lv_obj_set_height(emulate_btn, button_height_global);
    lv_obj_add_style(emulate_btn, get_zebra_style(4), 0);
    lv_obj_t *elabel = lv_obj_get_child(emulate_btn, 0);
    if (elabel) {
        lv_obj_set_style_text_font(elabel, get_menu_font(), 0);
        vertically_center_label(elabel, emulate_btn);
        lv_obj_add_style(elabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(emulate_btn, (void *)"Write");
    lv_obj_add_event_cb(emulate_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Write");

    num_items = 5;

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    // Ensure hardware back row exists on main menu (encoder/joystick builds)
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style(5), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_set_user_data(back_row, (void *)"__BACK_OPTION__");
        lv_obj_add_event_cb(back_row, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"__BACK_OPTION__");
        num_items++;
    }
#endif

    selected_index = 0;
    highlight_selected();
}

static void nfc_enter_write_list(void) {
    if (!menu_container) return;
    in_write_list = true;
    nfc_clear_write_list();
    lv_obj_clean(menu_container);

    // List files from /mnt/ghostesp/nfc/
    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        // First pass to count
        size_t count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (has_nfc_ext(de->d_name)) count++;
        }
        rewinddir(d);
        if (count > 0) {
            nfc_file_paths = (char**)calloc(count, sizeof(char*));
        }
        size_t idx = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!has_nfc_ext(de->d_name)) continue;
            size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1; // dir + '/' + name + NUL
            char *copy = (char*)malloc(need);
            if (!copy) continue;
            snprintf(copy, need, "%s/%s", dir, de->d_name);
            if (nfc_file_paths && idx < count) nfc_file_paths[idx++] = copy;
            ESP_LOGI(TAG, "nfc_enter_write_list: %s", copy);

            lv_obj_t *row = lv_list_add_btn(menu_container, NULL, de->d_name);
            lv_obj_set_height(row, button_height_global);
            lv_obj_add_style(row, get_zebra_style((int)idx & 1), 0);
            lv_obj_t *label = lv_obj_get_child(row, 0);
            if (label) {
                lv_obj_set_style_text_font(label, get_menu_font(), 0);
                vertically_center_label(label, row);
                lv_obj_add_style(label, &style_menu_label, 0);
            }
            lv_obj_add_event_cb(row, nfc_file_item_cb, LV_EVENT_CLICKED, copy);
        }
        nfc_file_count = idx;
        ESP_LOGI(TAG, "nfc_enter_write_list: %u .nfc files", (unsigned)nfc_file_count);
        closedir(d);
    }

    // If no files, show a placeholder row
    if (nfc_file_count == 0) {
        lv_obj_t *row = lv_list_add_btn(menu_container, NULL, "No .nfc files");
        lv_obj_set_height(row, button_height_global);
        lv_obj_add_style(row, get_zebra_style(0), 0);
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (label) {
            lv_obj_set_style_text_font(label, get_menu_font(), 0);
            vertically_center_label(label, row);
            lv_obj_add_style(label, &style_menu_label, 0);
        }
    }

    // Hardware back row (like options screen)
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style((int)(nfc_file_count + 1) & 1), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_add_event_cb(back_row, back_event_cb, LV_EVENT_CLICKED, NULL);
        num_items = (int)nfc_file_count + 2;
    } else {
        num_items = (int)nfc_file_count + 1;
    }
#endif

    selected_index = 0;
    highlight_selected();
    if (did) nfc_sd_end(susp);
}

void saved_clear_list(void) {
    if (saved_file_paths) {
        for (size_t i = 0; i < saved_file_count; ++i) free(saved_file_paths[i]);
        free(saved_file_paths);
    }
    saved_file_paths = NULL;
    saved_file_count = 0;
}

static void saved_file_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    create_saved_details_popup(path);
}

static void saved_enter_list(void) {
    if (!menu_container) return;
    in_saved_list = true;
    saved_clear_list();
    lv_obj_clean(menu_container);

    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de; size_t count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (has_nfc_ext(de->d_name)) count++;
        }
        rewinddir(d);
        if (count > 0) saved_file_paths = (char**)calloc(count, sizeof(char*));
        size_t idx = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!has_nfc_ext(de->d_name)) continue;
            size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1;
            char *copy = (char*)malloc(need);
            if (!copy) continue;
            snprintf(copy, need, "%s/%s", dir, de->d_name);
            if (saved_file_paths && idx < count) saved_file_paths[idx++] = copy;
            ESP_LOGI(TAG, "saved_enter_list: %s", copy);

            lv_obj_t *row = lv_list_add_btn(menu_container, NULL, de->d_name);
            lv_obj_set_height(row, button_height_global);
            lv_obj_add_style(row, get_zebra_style((int)idx & 1), 0);
            lv_obj_t *label = lv_obj_get_child(row, 0);
            if (label) {
                lv_obj_set_style_text_font(label, get_menu_font(), 0);
                vertically_center_label(label, row);
                lv_obj_add_style(label, &style_menu_label, 0);
            }
            lv_obj_add_event_cb(row, saved_file_item_cb, LV_EVENT_CLICKED, copy);
        }
        saved_file_count = idx;
        ESP_LOGI(TAG, "saved_enter_list: %u .nfc files", (unsigned)saved_file_count);
        closedir(d);
    }

    if (saved_file_count == 0) {
        lv_obj_t *row = lv_list_add_btn(menu_container, NULL, "No .nfc files");
        lv_obj_set_height(row, button_height_global);
        lv_obj_add_style(row, get_zebra_style(0), 0);
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (label) {
            lv_obj_set_style_text_font(label, get_menu_font(), 0);
            vertically_center_label(label, row);
            lv_obj_add_style(label, &style_menu_label, 0);
        }
    }

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style((int)(saved_file_count + 1) & 1), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_add_event_cb(back_row, back_event_cb, LV_EVENT_CLICKED, NULL);
        num_items = (int)saved_file_count + 2;
    } else {
        num_items = (int)saved_file_count + 1;
    }
#endif

    selected_index = 0;
    highlight_selected();
    if (did) nfc_sd_end(susp);
}

static void update_nfc_write_popup_selection(void) {
    // Update Cancel button
    if (nfc_write_cancel_btn && lv_obj_is_valid(nfc_write_cancel_btn)) {
        popup_set_button_selected(nfc_write_cancel_btn, nfc_write_popup_selected == 0);
    }
    
    // Update Write button
    if (nfc_write_go_btn && lv_obj_is_valid(nfc_write_go_btn)) {
        popup_set_button_selected(nfc_write_go_btn, nfc_write_popup_selected == 1);
    }
}

static void update_saved_popup_selection(void) {
    if (!saved_close_btn || !lv_obj_is_valid(saved_close_btn)) return;
    lv_obj_t *btns[3] = { saved_close_btn, saved_rename_btn, saved_delete_btn };
    popup_update_selection(btns, 3, saved_popup_selected);
    update_saved_buttons_layout();
}

static void keys_close_cb(lv_event_t *e) { (void)e; cleanup_keys_popup(NULL); }
static void keys_scroll_up_cb(lv_event_t *e);
static void keys_scroll_down_cb(lv_event_t *e);

static void cleanup_keys_popup(void *obj) {
    (void)obj;
    if (keys_popup) { lv_obj_del(keys_popup); keys_popup = NULL; }
    keys_close_btn = NULL;
    keys_title_label = NULL;
    keys_details_label = NULL;
    keys_popup_selected = 0;
}

static void update_keys_popup_selection(void) {
    // Use the popup_update_selection helper for the array of buttons
    lv_obj_t *btns[3] = { keys_up_btn, keys_close_btn, keys_down_btn };
    popup_update_selection(btns, 3, keys_popup_selected);
}

static void keys_scroll_up_cb(lv_event_t *e) {
    lv_obj_t *scroll = keys_scroll;
    if (e) {
        lv_obj_t *ud = (lv_obj_t *)lv_event_get_user_data(e);
        if (ud) scroll = ud;
    }
    if (!scroll || !lv_obj_is_valid(scroll)) return;
    lv_coord_t y = lv_obj_get_scroll_y(scroll);
    lv_obj_scroll_to_y(scroll, y - 40, LV_ANIM_OFF);
}

static void keys_scroll_down_cb(lv_event_t *e) {
    lv_obj_t *scroll = keys_scroll;
    if (e) {
        lv_obj_t *ud = (lv_obj_t *)lv_event_get_user_data(e);
        if (ud) scroll = ud;
    }
    if (!scroll || !lv_obj_is_valid(scroll)) return;
    lv_coord_t y = lv_obj_get_scroll_y(scroll);
    lv_obj_scroll_to_y(scroll, y + 40, LV_ANIM_OFF);
}

static void create_keys_popup(void) {
    if (!root) return;
    if (keys_popup && lv_obj_is_valid(keys_popup)) cleanup_keys_popup(NULL);
    int popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 0;
    
    if (LV_VER_RES <= 135) {
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 110) popup_h = 110;
        y_offset = 10;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 170;
        y_offset = 10;
    }
    keys_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;

    keys_title_label = popup_create_title_label(keys_popup, "User MFC Keys", title_font, 10);

    // Create scrollable container for keys and set fixed popup height
    keys_scroll = popup_create_scroll_area(keys_popup, LV_HOR_RES - 50, popup_h - 80, LV_ALIGN_TOP_MID, 0, 26);

    keys_details_label = popup_create_body_label(keys_scroll, "", LV_HOR_RES - 60, true, body_font, 0);
    if (keys_details_label) lv_obj_align(keys_details_label, LV_ALIGN_TOP_LEFT, 0, 0);

    // read keys file and show (build text on heap to avoid stack pressure)
    bool display_was_suspended = false;
    bool sd_ready = nfc_sd_begin(&display_was_suspended);
    size_t cap = 512; size_t pos = 0;
    char *buf = NULL;
    FILE *f = NULL;

    if (!sd_ready) {
        lv_label_set_text(keys_details_label, "No user keys file found");
        goto keys_cleanup;
    }

    buf = (char*)malloc(cap);
    if (!buf) {
        lv_label_set_text(keys_details_label, "(Out of memory)");
        goto keys_cleanup;
    }
    buf[0] = '\0';

    f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "r");
    if (!f) {
        lv_label_set_text(keys_details_label, "No user keys file found");
        goto keys_cleanup;
    }

    char line[256];
    int keys_on_line = 0;
    while (fgets(line, sizeof(line), f)) {
        // normalize: keep only hex chars, uppercase, and split into 12-length chunks
        char hexbuf[256]; size_t h = 0;
        for (char *p = line; *p && h < sizeof(hexbuf)-1; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'f') c -= 32;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
                hexbuf[h++] = c;
            }
        }
        hexbuf[h] = '\0';
        // output each 12-hex key; two per line: "KEY KEY\n"
        size_t i = 0;
        while (i + 12 <= h) {
            // ensure capacity, attempt write, grow if truncated
            for (;;) {
                int n;
                if (keys_on_line == 0) {
                    n = snprintf(buf + pos, cap - pos, "%.*s", 12, &hexbuf[i]);
                } else {
                    n = snprintf(buf + pos, cap - pos, " %.*s\n", 12, &hexbuf[i]);
                }
                if (n < 0) { break; }
                if ((size_t)n < (cap - pos)) { pos += (size_t)n; break; }
                size_t new_cap = cap * 2;
                char *nbuf = (char*)realloc(buf, new_cap);
                if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
                buf = nbuf; cap = new_cap;
            }
            keys_on_line = (keys_on_line == 0) ? 1 : 0;
            i += 12;
        }
        if (cap - pos < 64) {
            size_t new_cap = cap * 2;
            char *nbuf = (char*)realloc(buf, new_cap);
            if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
            buf = nbuf; cap = new_cap;
        }
    }

    // if one key left without its pair, terminate the line
    if (keys_on_line == 1) {
        for (;;) {
            int n = snprintf(buf + pos, cap - pos, "\n");
            if (n < 0) break;
            if ((size_t)n < (cap - pos)) { pos += (size_t)n; break; }
            size_t new_cap = cap * 2;
            char *nbuf = (char*)realloc(buf, new_cap);
            if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
            buf = nbuf; cap = new_cap;
        }
    }

    if (pos == 0) {
        lv_label_set_text(keys_details_label, "(Empty)");
    } else {
        lv_label_set_text(keys_details_label, buf);
    }

keys_cleanup:
    if (f) fclose(f);
    if (buf) free(buf);
    if (sd_ready) nfc_sd_end(display_was_suspended);

    // Bottom controls: Up | Close | Down
    int btn_w = 60, btn_h = 34; if (LV_VER_RES <= 240) { btn_w = 54; btn_h = 30; }
    keys_up_btn = popup_add_styled_button(keys_popup, LV_SYMBOL_UP, btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, keys_scroll_up_cb, keys_scroll);
    int close_w = 90; if (LV_VER_RES <= 240) close_w = 80;
    keys_close_btn = popup_add_styled_button(keys_popup, "Close", close_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, keys_close_cb, NULL);
    keys_down_btn = popup_add_styled_button(keys_popup, LV_SYMBOL_DOWN, btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, keys_scroll_down_cb, keys_scroll);

    update_keys_buttons_layout();
    keys_popup_selected = 1; // default focus on Close
    update_keys_popup_selection();
}

// ---- chameleon ultra basic popup ----
void cleanup_cu_popup(void *obj) {
    (void)obj;
    if (cu_popup) { lv_obj_del(cu_popup); cu_popup = NULL; }
    cu_title_label = NULL;
    cu_details_label = NULL;
    cu_close_btn = NULL;
    cu_connect_btn = NULL;
    cu_disconnect_btn = NULL;
    cu_reader_btn = NULL;
    cu_scan_hf_btn = NULL;
    cu_save_hf_btn = NULL;
    cu_more_btn = NULL;
    cu_popup_selected = 0;
    cu_save_visible = false;
    cu_busy = false;
    cu_more_expanded = false;
    if (cu_state_timer) { lv_timer_del(cu_state_timer); cu_state_timer = NULL; }
}

static void cu_close_cb(lv_event_t *e) { (void)e; cleanup_cu_popup(NULL); }

static void cu_bool_done_async(void *ptr) {
    cu_busy = false;

    if (cu_title_label && lv_obj_is_valid(cu_title_label)) {
        lv_label_set_text(cu_title_label, "Chameleon Ultra");
    }
    if (cu_details_label && lv_obj_is_valid(cu_details_label)) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
    }
    update_cu_buttons_layout();
    update_cu_popup_selection();
    if (ptr) nfc_bool_pool_free(ptr);
}

static void cu_connect_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_connect(10, NULL);
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(cu_bool_done_async, res); }
    else { lv_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_disconnect_task(void *arg) {
    (void)arg;
    chameleon_manager_disconnect();
    bool ok = !chameleon_manager_is_connected();
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(cu_bool_done_async, res); }
    else { lv_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_reader_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_set_reader_mode();
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(cu_bool_done_async, res); }
    else { lv_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_scan_hf_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_scan_hf();
    if (ok) cu_save_visible = true;
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(cu_bool_done_async, res); }
    else { lv_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_save_hf_task(void *arg) {
    (void)arg;
    bool ok = false;
    glog("Saving last HF scan header...");
    ok = chameleon_manager_save_last_hf_scan(NULL);
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; lv_async_call(cu_bool_done_async, res); }
    else { lv_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void update_cu_buttons_layout(void) {
    if (!cu_popup) return;
    bool connected = chameleon_manager_is_connected();
    if (connected) {
        if (cu_connect_btn) lv_obj_add_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
        if (cu_disconnect_btn) lv_obj_clear_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (cu_disconnect_btn) lv_obj_add_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        if (cu_connect_btn) lv_obj_clear_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // collect visible buttons in order: Close, Connect/Disconnect
    lv_obj_t *btns[3]; int count = 0;
    if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) btns[count++] = cu_close_btn;
    if (connected) {
        if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_disconnect_btn;
    } else {
        if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_connect_btn;
    }
    if (count == 0) return;
    popup_layout_buttons_responsive(cu_popup, btns, count, -8, NULL);
}

static void update_cu_popup_selection(void) {
    lv_obj_t *btns[3]; int count = 0;
    if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) btns[count++] = cu_close_btn;
    if (!chameleon_manager_is_connected()) {
        if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_connect_btn;
    } else {
        if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_disconnect_btn;
    }
    for (int i = 0; i < count; ++i) popup_set_button_selected(btns[i], cu_popup_selected == i);
    update_cu_buttons_layout();
}

static void cu_connect_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Connecting...");
    xTaskCreate(cu_connect_task, "cu_connect", 4096, NULL, 5, NULL);
}

static void cu_disconnect_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Chameleon Ultra");
    xTaskCreate(cu_disconnect_task, "cu_disconnect", 4096, NULL, 5, NULL);
}

static void cu_reader_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Setting reader mode...");
    xTaskCreate(cu_reader_task, "cu_reader", 4096, NULL, 5, NULL);
}

static void cu_scan_hf_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Scanning HF...");
    xTaskCreate(cu_scan_hf_task, "cu_scan_hf", 4096, NULL, 5, NULL);
}

static void cu_save_hf_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Saving...");
    BaseType_t rc = xTaskCreate(cu_save_hf_task, "cu_save_hf", 4096, NULL, 5, NULL);
    if (rc != pdPASS) {
        rc = xTaskCreate(cu_save_hf_task, "cu_save_hf", 3072, NULL, 5, NULL);
    }
    if (rc != pdPASS) {
        cu_busy = false;
        if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Save failed");
        ESP_LOGE(TAG, "cu_save_hf_task create failed");
    }
}

static void cu_more_cb(lv_event_t *e) { (void)e; }

static void create_cu_popup(void) {
    if (!root) return;
    if (cu_popup && lv_obj_is_valid(cu_popup)) cleanup_cu_popup(NULL);
    int popup_w = (LV_HOR_RES <= 240) ? (LV_HOR_RES - 20) : (LV_HOR_RES - 30);
    int popup_h;
    int y_offset = 0;
    
    if (LV_VER_RES <= 135) {
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 110) popup_h = 110;
        y_offset = 10;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 160;
        y_offset = 10;
    }
    cu_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;

    cu_title_label = popup_create_title_label(cu_popup, "Chameleon Ultra", title_font, 10);
    cu_details_label = popup_create_body_label(cu_popup, "", LV_HOR_RES - 50, true, body_font, 26);
    if (cu_details_label) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
        lv_obj_set_style_text_align(cu_details_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cu_details_label, LV_ALIGN_TOP_MID, 0, 26);
    }

    int btn_w = (LV_HOR_RES <= 240) ? 80 : 90; int btn_h = (LV_HOR_RES <= 240) ? 30 : 34;
    cu_close_btn = popup_add_styled_button(cu_popup, "Close", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, cu_close_cb, NULL);
    cu_connect_btn = popup_add_styled_button(cu_popup, "Connect", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, cu_connect_cb, NULL);
    cu_disconnect_btn = popup_add_styled_button(cu_popup, "Disconnect", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, cu_disconnect_cb, NULL);
    // strip advanced controls; chameleon popup is connect-only now

    if (!chameleon_manager_is_connected()) {
        if (cu_disconnect_btn) lv_obj_add_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (cu_connect_btn) lv_obj_add_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
    }

    cu_popup_selected = 0;
    update_cu_buttons_layout();
    update_cu_popup_selection();

    // start lightweight state refresh timer to keep popup live without reopening
    if (cu_state_timer) { lv_timer_del(cu_state_timer); cu_state_timer = NULL; }
    cu_state_timer = lv_timer_create(cu_state_timer_cb, 300, NULL);
}

static void cu_state_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!cu_popup || !lv_obj_is_valid(cu_popup)) return;
    if (cu_details_label && lv_obj_is_valid(cu_details_label)) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
    }
    update_cu_buttons_layout();
}


void cleanup_nfc_write_popup(void *obj) {
    (void)obj;
    if (nfc_write_popup) { lv_obj_del(nfc_write_popup); nfc_write_popup = NULL; }
    nfc_write_cancel_btn = NULL; nfc_write_go_btn = NULL;
    nfc_write_title_label = NULL; nfc_write_details_label = NULL;
    nfc_write_popup_selected = 0;
    // Do not force-cancel here; caller controls cancel flag
    #ifdef CONFIG_NFC_PN532
    if (g_write_image_valid && !nfc_write_in_progress) { ntag_file_free(&g_write_image); g_write_image_valid = false; }
    #else
    g_write_image_valid = false;
    #endif
}

static char* build_compact_write_details(const ntag_file_image_t *img) {
    if (!img) return NULL;
    size_t cap = 768;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    // UID | Type
    pos += snprintf(out + pos, cap - pos, "UID:");
    for (uint8_t i = 0; i < img->uid_len && pos < cap - 4; ++i) {
        pos += snprintf(out + pos, cap - pos, " %02X", img->uid[i]);
    }
    pos += snprintf(out + pos, cap - pos, " | Type: %s\n", ntag_t2_model_str(img->model));
    // Pages | First user page
    pos += snprintf(out + pos, cap - pos, "Pages: %d | First user: %d\n", img->pages_total, img->first_user_page);
    // NDEF summary
    const uint8_t *mem = NULL; size_t mem_len = 0;
    if (img->full_pages && img->pages_total > img->first_user_page) {
        mem = &img->full_pages[(size_t)img->first_user_page * 4];
        mem_len = (size_t)(img->pages_total - img->first_user_page) * 4;
    }
    if (mem && mem_len > 0) {
        size_t off = 0, len = 0;
        if (ntag_t2_find_ndef(mem, mem_len, &off, &len) && off + len <= mem_len) {
            char *full = ndef_build_details_from_message(mem + off, len, img->uid, img->uid_len, ntag_t2_model_str(img->model));
            if (full) {
                // Extract the first decoded record line (e.g., URL ..., Text ..., SmartPoster ...)
                const char *p = strstr(full, "\nR");
                if (!p) {
                    // handle if the very first line starts with R
                    if (full[0] == 'R') p = full;
                } else {
                    p++; // move to 'R'
                }
                if (p && p[0] == 'R') {
                    // find the colon after R# and a space after colon
                    const char *colon = strchr(p, ':');
                    const char *start = NULL;
                    if (colon) {
                        start = colon + 1;
                        if (*start == ' ') start++;
                    } else {
                        start = p; // fallback, include R# prefix
                    }
                    const char *endl = strchr(start, '\n');
                    if (!endl) endl = start + strlen(start);
                    pos += snprintf(out + pos, cap - pos, "%.*s\n", (int)(endl - start), start);
                } else {
                    // Fallback to size-only summary
                    pos += snprintf(out + pos, cap - pos, "NDEF: %uB\n", (unsigned)len);
                }
                free(full);
            } else {
                pos += snprintf(out + pos, cap - pos, "NDEF: %uB\n", (unsigned)len);
            }
        } else {
            pos += snprintf(out + pos, cap - pos, "NDEF: none\n");
        }
    } else {
        pos += snprintf(out + pos, cap - pos, "NDEF: unknown\n");
    }
    return out;
}

// Very lightweight Flipper MIFARE Classic parser for Saved popup
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
static char* build_mfc_details_from_file(const char *path, char **out_title) {
    if (out_title) *out_title = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[256];
    bool is_mfc = false;
    char *title = NULL;
    // extract UID, ATQA, SAK, and type string
    uint8_t uid[10] = {0}; int uid_len = 0;
    unsigned atqa_hi = 0, atqa_lo = 0; unsigned sak = 0;
    char type_str[48] = {0};

    // First pass: basic metadata
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Device type:", 12) == 0 && strstr(line, "Mifare Classic")) {
            is_mfc = true;
        } else if (strncmp(line, "Mifare Classic type:", 21) == 0) {
            // includes size: 4K/1K/Mini
            char tmp[40] = {0};
            if (sscanf(line + 21, " %39[^\n\r]", tmp) == 1) {
                size_t prefix_len = strlen("MIFARE Classic ");
                size_t max_tmp = (sizeof(type_str) > prefix_len + 1) ? (sizeof(type_str) - prefix_len - 1) : 0;
                if (max_tmp > 0) snprintf(type_str, sizeof(type_str), "MIFARE Classic %.*s", (int)max_tmp, tmp);
                else type_str[0] = '\0';
            }
        } else if (strncmp(line, "UID:", 4) == 0) {
            const char *p = line + 4; int ncon = 0; unsigned b = 0;
            while (*p && uid_len < (int)sizeof(uid)) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (sscanf(p, " %2x%n", &b, &ncon) == 1) { uid[uid_len++] = (uint8_t)b; p += ncon; }
                else break;
            }
        } else if (strncmp(line, "ATQA:", 6) == 0) {
            sscanf(line + 6, " %2x %2x", &atqa_hi, &atqa_lo);
        } else if (strncmp(line, "SAK:", 4) == 0) {
            sscanf(line + 4, " %2x", &sak);
        }
    }
    if (!is_mfc) { fclose(f); return NULL; }
    if (type_str[0] == '\0') snprintf(type_str, sizeof(type_str), "MIFARE Classic");

    // Determine type and total sectors/keys
    MFC_TYPE mt = mfc_type_from_sak((uint8_t)sak);
    int sectors_total = mfc_sector_count(mt);
    if (sectors_total == 0) sectors_total = 16;
    int keys_total = sectors_total * 2;

    // Prepare block count and allocate storage for block data
    int blocks_total = 0;
    for (int s = 0; s < sectors_total; ++s) blocks_total += mfc_blocks_in_sector(mt, s);

    // Rewind and parse Block lines to assess readable blocks and keys
    rewind(f);
    // allocate array of int[blocks_total][16], -1 for unknown, 0-255 for bytes
    int *blocks = (int*)malloc((size_t)blocks_total * 16 * sizeof(int));
    if (!blocks) { fclose(f); return NULL; }
    for (int i = 0; i < blocks_total * 16; ++i) blocks[i] = -1;
    while (fgets(line, sizeof(line), f)) {
        int blk = -1;
        if (sscanf(line, "Block %d:", &blk) == 1 && blk >= 0 && blk < blocks_total) {
            const char *p = strchr(line, ':');
            if (!p) {
                continue;
            }
            p++;
            int off = blk * 16;
            // parse up to 16 tokens
            for (int bi = 0; bi < 16; ++bi) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (p[0] == '?' && p[1] == '?') { blocks[off + bi] = -1; p += 2; }
                else {
                    unsigned v = 0; int consumed = 0;
                    if (sscanf(p, "%2x%n", &v, &consumed) == 1) { blocks[off + bi] = (int)v; p += consumed; }
                    else { blocks[off + bi] = -1; while (*p && *p != ' ') ++p; }
                }
            }
        }
    }
    fclose(f);

    // Compute readable sectors and keys found
    int sectors_readable = 0;
    int keys_found = 0;
    for (int s = 0; s < sectors_total; ++s) {
        int first = mfc_first_block_of_sector(mt, s);
        int blocks_in_sec = mfc_blocks_in_sector(mt, s);
        bool sector_has_data = false;
        // scan blocks in sector
        for (int b = 0; b < blocks_in_sec; ++b) {
            int blk = first + b;
            if (blk < 0 || blk >= blocks_total) continue;
            int off = blk * 16;
            for (int bi = 0; bi < 16; ++bi) {
                if (blocks[off + bi] >= 0) { sector_has_data = true; break; }
            }
            if (sector_has_data) break;
        }
        if (sector_has_data) sectors_readable++;
        // check trailer for keys (last block)
        int trailer = first + blocks_in_sec - 1;
        if (trailer >= 0 && trailer < blocks_total) {
            int offt = trailer * 16;
            // Key A in bytes 0..5
            bool key_a_known = true; for (int k = 0; k < 6; ++k) if (blocks[offt + k] < 0) { key_a_known = false; break; }
            if (key_a_known) keys_found++;
            // Key B in bytes 10..15
            bool key_b_known = true; for (int k = 10; k < 16; ++k) if (blocks[offt + k] < 0) { key_b_known = false; break; }
            if (key_b_known) keys_found++;
        }
    }
    free(blocks);

    // build title
    title = (char*)malloc(48);
    if (title) snprintf(title, 48, "%s", type_str);

    // build details compact
    size_t cap = 512; char *out = (char*)malloc(cap);
    if (!out) { if (title) free(title); if (out_title) *out_title = NULL; return NULL; }
    int pos = 0;
    pos += snprintf(out + pos, cap - pos, "UID:");
    for (int i = 0; i < uid_len && pos < (int)cap - 4; ++i) pos += snprintf(out + pos, cap - pos, " %02X", uid[i]);
    pos += snprintf(out + pos, cap - pos, "\nATQA: %02X %02X | SAK: %02X\n", (unsigned)atqa_hi, (unsigned)atqa_lo, (unsigned)sak);
    pos += snprintf(out + pos, cap - pos, "Sectors: %d/%d | Keys: %d/%d\n", sectors_readable, sectors_total, keys_found, keys_total);

    if (out_title) *out_title = title; else if (title) free(title);

    // Try to find NDEF in saved blocks and append a concise single-line summary
    for (int s = 0; s < sectors_total; ++s) {
        if (s == 16 && sectors_total > 16) continue; // skip MAD2 on 4K
        int first = mfc_first_block_of_sector(mt, s);
        int blocks_in_sec = mfc_blocks_in_sector(mt, s);
        int data_blocks = blocks_in_sec - 1;
        if (data_blocks <= 0) continue;
        size_t sec_bytes = (size_t)data_blocks * 16;
        uint8_t *sec_buf = (uint8_t*)malloc(sec_bytes);
        if (!sec_buf) break;
        size_t woff = 0;
        for (int b = 0; b < data_blocks; ++b) {
            int blk = first + b;
            int off = blk * 16;
            for (int bi = 0; bi < 16; ++bi) {
                int v = -1;
                if (blk >= 0 && blk < blocks_total) v = blocks[off + bi];
                sec_buf[woff + bi] = (v >= 0) ? (uint8_t)v : 0x00;
            }
            woff += 16;
        }
        #if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
        size_t off = 0, mlen = 0;
        if (ntag_t2_find_ndef(sec_buf, sec_bytes, &off, &mlen) && off < sec_bytes && mlen > 0) {
            // assemble contiguous view across subsequent sectors to cover message
            size_t need = off + mlen;
            size_t have = sec_bytes;
            int ss = s + 1;
            while (have < need && ss < sectors_total) {
                if (ss == 16 && sectors_total > 16) { ss++; continue; }
                int bl2 = mfc_blocks_in_sector(mt, ss);
                have += (size_t)(bl2 - 1) * 16;
                ss++;
            }
            size_t total_cap = have;
            uint8_t *cat = (uint8_t*)malloc(total_cap);
            if (cat) {
                // copy first sector
                memcpy(cat, sec_buf, sec_bytes);
                size_t cat_off = sec_bytes;
                for (int s2 = s + 1; cat_off < total_cap && s2 < sectors_total; ++s2) {
                    if (s2 == 16 && sectors_total > 16) continue;
                    int f2 = mfc_first_block_of_sector(mt, s2);
                    int bl2 = mfc_blocks_in_sector(mt, s2);
                    for (int b2 = 0; b2 < bl2 - 1 && cat_off < total_cap; ++b2) {
                        int absb2 = f2 + b2;
                        int offb = absb2 * 16;
                        for (int bi = 0; bi < 16 && cat_off < total_cap; ++bi) {
                            int v = -1;
                            if (absb2 >= 0 && absb2 < blocks_total) v = blocks[offb + bi];
                            cat[cat_off++] = (v >= 0) ? (uint8_t)v : 0x00;
                        }
                    }
                }

                char *ndef_text = ndef_build_details_from_message(cat + off, mlen, uid, uid_len, type_str);
                if (ndef_text) {
                    // extract first record line
                    const char *p = strstr(ndef_text, "\nR");
                    if (!p) { if (ndef_text[0] == 'R') p = ndef_text; }
                    else p++;
                    if (p && p[0] == 'R') {
                        const char *colon = strchr(p, ':');
                        const char *start = NULL;
                        if (colon) { start = colon + 1; if (*start == ' ') start++; }
                        else start = p;
                        const char *endl = strchr(start, '\n');
                        if (!endl) endl = start + strlen(start);
                        // append directly after existing lines (no extra blank)
                        int napp = snprintf(out + pos, cap - pos, "NDEF: %.*s\n", (int)(endl - start), start);
                        if (napp > 0) { pos += napp; }
                    }
                    free(ndef_text);
                    free(cat);
                    free(sec_buf);
                    break; // stop after first found
                }
                free(cat);
            }
        }
        #endif
        free(sec_buf);
    }

    return out;
}
#endif

static void create_nfc_write_popup(const char *path) {
    if (!root) return;
    // Load image
    #ifdef CONFIG_NFC_PN532
    memset(&g_write_image, 0, sizeof(g_write_image));
    // jit sd mount only for somethingsomething template via nfc_sd_begin()
    bool susp_rd = false; bool did_rd = nfc_sd_begin(&susp_rd);
    g_write_image_valid = ntag_file_load(path, &g_write_image);
    if (did_rd) nfc_sd_end(susp_rd);
    strncpy(g_write_image_path, path, sizeof(g_write_image_path) - 1);
    g_write_image_path[sizeof(g_write_image_path) - 1] = '\0';
    ESP_LOGI(TAG, "create_nfc_write_popup: path=%s valid=%d", g_write_image_path, (int)g_write_image_valid);
    #else
    g_write_image_valid = false;
    strncpy(g_write_image_path, path, sizeof(g_write_image_path) - 1);
    g_write_image_path[sizeof(g_write_image_path) - 1] = '\0';
    #endif

    if (nfc_write_popup && lv_obj_is_valid(nfc_write_popup)) cleanup_nfc_write_popup(NULL);
    int popup_w;
    if (LV_HOR_RES <= 240) popup_w = LV_HOR_RES - 20; else popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 0;
    
    if (LV_VER_RES <= 135) {
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 120) popup_h = 120;
        y_offset = 10;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 160;
        y_offset = 10;
    }
    nfc_write_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;

    const char *nfc_write_title_text = g_write_image_valid ? "Write Tag" :
    #ifdef CONFIG_NFC_PN532
        "Invalid file"
    #else
        "NFC disabled"
    #endif
    ;
    nfc_write_title_label = popup_create_title_label(nfc_write_popup, nfc_write_title_text, title_font, 10);

    nfc_write_details_label = popup_create_body_label(nfc_write_popup, "", LV_HOR_RES - 50, true, body_font, 26);
    #ifdef CONFIG_NFC_PN532
    if (g_write_image_valid) {
        char *det = build_compact_write_details(&g_write_image);
        if (det) {
            lv_label_set_text(nfc_write_details_label, det);
            ESP_LOGI(TAG, "write_popup details:\n%s", det);
            free(det);
        } else {
            lv_label_set_text(nfc_write_details_label, "File parsed");
        }
    } else {
        lv_label_set_text(nfc_write_details_label, "Failed to parse .nfc file");
    }
    #else
    lv_label_set_text(nfc_write_details_label, "Writing tags requires NFC hardware");
    #endif

    int btn_w = 90, btn_h = 34;
    if (LV_HOR_RES <= 240) { btn_w = 80; btn_h = 30; }

    nfc_write_cancel_btn = popup_add_styled_button(nfc_write_popup, "Cancel", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, nfc_write_cancel_cb, NULL);

    nfc_write_go_btn = popup_add_styled_button(nfc_write_popup, "Write", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_write_go_cb, NULL);
    if (!g_write_image_valid && nfc_write_go_btn) lv_obj_add_state(nfc_write_go_btn, LV_STATE_DISABLED);

    nfc_write_popup_selected = 0;
    update_nfc_write_popup_selection();
}

static void saved_close_cb(lv_event_t *e) { (void)e; cleanup_saved_details_popup(NULL); }
static void saved_rename_cb(lv_event_t *e) {
    (void)e;
    if (g_saved_current_path[0] == '\0') return;
    // derive current base name without extension
    const char *slash = strrchr(g_saved_current_path, '/');
    const char *fname = slash ? slash + 1 : g_saved_current_path;
    char base[64] = {0};
    strncpy(base, fname, sizeof(base) - 1);
    char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
    keyboard_view_set_submit_callback(saved_rename_keyboard_callback);
    // placeholder should be the current base name (without extension) so typing "example" results in example.nfc
    keyboard_view_set_placeholder(base[0] ? base : fname);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
static void saved_delete_cb(lv_event_t *e) {
    (void)e;
    if (g_saved_current_path[0] == '\0') return;
    bool susp = false; nfc_sd_begin(&susp);
    if (remove(g_saved_current_path) == 0) {
        ESP_LOGI(TAG, "deleted file: %s", g_saved_current_path);
    } else {
        ESP_LOGE(TAG, "failed delete: %s", g_saved_current_path);
    }
    nfc_sd_end(susp);
    cleanup_saved_details_popup(NULL);
    // refresh list
    if (!in_saved_list) saved_enter_list(); else saved_enter_list();
}
static void saved_rename_keyboard_callback(const char *name) {
    if (!name || !*name) { display_manager_switch_view(&nfc_view); return; }
    if (g_saved_current_path[0] == '\0') { display_manager_switch_view(&nfc_view); return; }
    // Build new path safely
    char dir[192]; strncpy(dir, g_saved_current_path, sizeof(dir)-1); dir[sizeof(dir)-1] = '\0';
    char *last = strrchr(dir, '/'); if (last) *last = '\0'; else dir[0] = '\0';
    char safe[200];
    size_t max_name = 180; // conservative limit
    size_t copy_len = (max_name < sizeof(safe) - 1) ? max_name : (sizeof(safe) - 1);
    strncpy(safe, name, copy_len);
    safe[copy_len] = '\0';
    for (size_t i = strlen(safe); i > 0 && (safe[i-1] == ' ' || safe[i-1] == '\r' || safe[i-1] == '\n' || safe[i-1] == '\t'); --i) safe[i-1] = '\0';
    for (char *p = safe; *p; ++p) { if (*p == '/' || *p == '\\') *p = '_'; }
    bool has_ext = false; size_t sl = strlen(safe);
    if (sl >= 4) { const char *ext = &safe[sl - 4]; if ((ext[0] == '.') && ((ext[1] | 0x20) == 'n') && ((ext[2] | 0x20) == 'f') && ((ext[3] | 0x20) == 'c')) has_ext = true; }

    saved_rename_job_t *job = (saved_rename_job_t*)malloc(sizeof(saved_rename_job_t));
    if (!job) { display_manager_switch_view(&nfc_view); return; }
    strncpy(job->old_path, g_saved_current_path, sizeof(job->old_path)-1); job->old_path[sizeof(job->old_path)-1] = '\0';
    {
        size_t N = sizeof(job->new_path);
        size_t dir_len = strlen(dir);
        size_t ext_len = has_ext ? 0 : 4; // ".nfc"
        if (dir_len + 1 + ext_len + 1 >= N) {
            // not enough room for any name; produce a safe fallback
            snprintf(job->new_path, N, "%s/renamed.nfc", dir);
        } else {
            int max_safe = (int)(N - dir_len - ext_len - 2); // room for '/', ext, and NUL
            if (max_safe < 0) max_safe = 0;
            if (has_ext) {
                snprintf(job->new_path, N, "%s/%.*s", dir, max_safe, safe);
            } else {
                snprintf(job->new_path, N, "%s/%.*s.nfc", dir, max_safe, safe);
            }
        }
    }
    job->success = 0;

    // Do the rename in a background task to avoid LVGL tick stack overflow
    xTaskCreate(saved_rename_task, "saved_rename", 4096, job, 5, NULL);
}

static void saved_rename_task(void *arg) {
    saved_rename_job_t *job = (saved_rename_job_t*)arg;
    if (!job) { vTaskDelete(NULL); return; }
    bool susp = false; bool did = nfc_sd_begin(&susp);
    int res = rename(job->old_path, job->new_path);
    job->success = (res == 0);
    if (did) nfc_sd_end(susp);
    lv_async_call(saved_rename_ui_done_cb, job);
    vTaskDelete(NULL);
}

static void saved_rename_ui_done_cb(void *param) {
    saved_rename_job_t *job = (saved_rename_job_t*)param;
    if (!job) return;
    if (job->success) {
        ESP_LOGI(TAG, "renamed: %s -> %s", job->old_path, job->new_path);
        strncpy(g_saved_current_path, job->new_path, sizeof(g_saved_current_path)-1);
        g_saved_current_path[sizeof(g_saved_current_path)-1] = '\0';
    } else {
        ESP_LOGE(TAG, "rename failed: %s -> %s (errno=%d)", job->old_path, job->new_path, errno);
    }
    // Close popup and refresh list on UI thread
    cleanup_saved_details_popup(NULL);
    display_manager_switch_view(&nfc_view);
    saved_enter_list();
    free(job);
}

void cleanup_saved_details_popup(void *obj) {
    (void)obj;
    if (saved_popup) { lv_obj_del(saved_popup); saved_popup = NULL; }
    saved_close_btn = NULL;
    saved_rename_btn = NULL;
    saved_delete_btn = NULL;
    saved_title_label = NULL;
    saved_details_label = NULL;
    saved_popup_selected = 0;
}

static void create_saved_details_popup(const char *path) {
    if (!root) return;
    if (saved_popup && lv_obj_is_valid(saved_popup)) cleanup_saved_details_popup(NULL);
    int popup_w;
    if (LV_HOR_RES <= 240) popup_w = LV_HOR_RES - 20; else popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 0;
    
    if (LV_VER_RES <= 135) {
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 120) popup_h = 120;
        y_offset = 10;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 160;
        y_offset = 10;
    }
    saved_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;

    saved_title_label = popup_create_title_label(saved_popup, "Saved Tag", title_font, 10);

    saved_details_label = popup_create_body_label(saved_popup, "", LV_HOR_RES - 50, true, body_font, 26);

    // store current path for rename/delete
    strncpy(g_saved_current_path, path, sizeof(g_saved_current_path) - 1);
    g_saved_current_path[sizeof(g_saved_current_path) - 1] = '\0';

    // parse file and show details (supports NTAG and MIFARE Classic)
    bool susp_load = false; bool did_load = nfc_sd_begin(&susp_load);
    char *title = NULL;
    char *mfc_det = NULL;
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
    mfc_det = build_mfc_details_from_file(path, &title);
#endif
    if (mfc_det) {
        if (title) { lv_label_set_text(saved_title_label, title); free(title); title = NULL; }
        lv_label_set_text(saved_details_label, mfc_det);
        free(mfc_det);
    } else {
        // Always allow NTAG file parsing, even without PN532
        ntag_file_image_t img; memset(&img, 0, sizeof(img));
        bool ok = ntag_file_load(path, &img);
        if (ok) {
            const char *label = ntag_t2_model_str(img.model);
            lv_label_set_text(saved_title_label, label);
            char *det = build_compact_write_details(&img);
            if (det) { lv_label_set_text(saved_details_label, det); free(det); }
            else { lv_label_set_text(saved_details_label, "File parsed"); }
            ntag_file_free(&img);
        } else {
            lv_label_set_text(saved_details_label, "Failed to parse .nfc file");
        }
    }
    if (did_load) nfc_sd_end(susp_load);

    int btn_w = 90, btn_h = 34; if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    // Buttons: Close (left), Rename (mid), Delete (right)
    saved_close_btn = popup_add_styled_button(saved_popup, "Close", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, saved_close_cb, NULL);
    saved_rename_btn = popup_add_styled_button(saved_popup, "Rename", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, saved_rename_cb, NULL);
    saved_delete_btn = popup_add_styled_button(saved_popup, "Delete", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, saved_delete_cb, NULL);

    saved_popup_selected = 0;
    update_saved_popup_selection();
}

static void nfc_write_cancel_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "nfc_write_cancel_cb: in_progress=%d", (int)nfc_write_in_progress);
    nfc_write_cancel = true;
    if (!nfc_write_in_progress) {
        cleanup_nfc_write_popup(NULL);
    } else {
        if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, "Cancelling...");
    }
}

#ifdef CONFIG_NFC_PN532
static bool ensure_pn532_ready(void) {
    if (g_pn532) return true;
    g_pn532 = &g_pn532_instance;
#if defined(CONFIG_HAS_FUEL_GAUGE) || defined(CONFIG_USE_BQ27220_FUEL_GAUGE)
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #elif defined(CONFIG_IDF_TARGET_ESP32C5)
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #else
        i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
    #endif
#else
    i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
#endif

    for (int pi = 0; pi < 2; ++pi) {
        i2c_port_t port = try_ports[pi];
        if (pn532_new_driver_i2c(
                (gpio_num_t)CONFIG_NFC_SDA_PIN,
                (gpio_num_t)CONFIG_NFC_SCL_PIN,
                (gpio_num_t)CONFIG_NFC_RST_PIN,
                (gpio_num_t)CONFIG_NFC_IRQ_PIN,
                port,
                g_pn532) != ESP_OK) {
            pn532_delete_driver(g_pn532);
            continue;
        }
        if (pn532_init(g_pn532) == ESP_OK) {
            pn532_set_passive_activation_retries(g_pn532, 0xFF);
            return true;
        }
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
    }
    g_pn532 = NULL;
    return false;
}

static bool nfc_write_progress_cb(int current, int total, void *user) {
    (void)user;
    nfc_wr_prog_t *p = (nfc_wr_prog_t*)malloc(sizeof(nfc_wr_prog_t));
    if (p) { p->current = current; p->total = total; lv_async_call(nfc_write_progress_async, p); }
    return !nfc_write_cancel;
}

static void nfc_write_progress_async(void *ptr) {
    if (!ptr) return;
    nfc_wr_prog_t *p = (nfc_wr_prog_t*)ptr;
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) {
        int percent = (p->total > 0) ? (p->current * 100) / p->total : 0;
        if (percent < 0) {
            percent = 0;
        }
        if (percent > 100) {
            percent = 100;
        }
        char t[48];
        snprintf(t, sizeof(t), "Writing... %d%%", percent);
        lv_label_set_text(nfc_write_title_label, t);
    }
    free(p);
}

static void nfc_write_done_async(void *ptr) {
    bool ok = (ptr != NULL) ? *((bool*)ptr) : false; if (ptr) free(ptr);
    nfc_write_in_progress = false;
    if (g_write_image_valid) { ntag_file_free(&g_write_image); g_write_image_valid = false; }
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, ok ? "Write complete" : "Write failed");
    ESP_LOGI(TAG, "nfc_write_done: %s", ok ? "success" : "fail");
}

static void nfc_write_task(void *arg) {
    (void)arg;
    bool ok = false;
    display_manager_set_low_i2c_mode(true);
    if (!ensure_pn532_ready()) {
        ok = false;
        goto done;
    }
    // Wait for tag presence
    ESP_LOGI(TAG, "nfc_write_task: waiting for tag...");
    for (;;) {
        if (nfc_write_cancel) { ok = false; goto done; }
        uint8_t uid[8] = {0}; uint8_t uid_len = 0; uint16_t atqa = 0; uint8_t sak = 0;
        if (pn532_read_passive_target_id_ex(g_pn532, 0x00, uid, &uid_len, &atqa, &sak, 200) == ESP_OK && uid_len > 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Write
    ESP_LOGI(TAG, "nfc_write_task: starting write file=%s", g_write_image_path);
    ok = ntag_write_to_tag(g_pn532, &g_write_image, nfc_write_progress_cb, NULL);

done:;
    display_manager_set_low_i2c_mode(false);
    if (g_pn532) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    bool *res = (bool*)malloc(sizeof(bool));
    if (res) { *res = ok; lv_async_call(nfc_write_done_async, res); }
    else { lv_async_call(nfc_write_done_async, NULL); }
    vTaskDelete(NULL);
}
#endif

static void nfc_write_go_cb(lv_event_t *e) {
    (void)e;
    if (!g_write_image_valid || nfc_write_in_progress) return;
    nfc_write_cancel = false;
    nfc_write_in_progress = true;
    ESP_LOGI(TAG, "nfc_write_go: %s", g_write_image_path);
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, "Present tag to write...");
#ifdef CONFIG_NFC_PN532
    xTaskCreate(nfc_write_task, "nfc_write", 6144, NULL, 5, NULL);
#endif
}

// ---- End Write Flow ----

void nfc_view_create(void) {
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    root = lv_obj_create(lv_scr_act());
    nfc_view.root = root;
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    init_styles();

    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = 20;
    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
#else
    const int BUTTON_AREA_HEIGHT = 0;
#endif
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;

    is_small_screen_global = is_small_screen;
    button_height_global = is_small_screen ? 40 : 55;

    menu_container = lv_list_create(root);
    lv_obj_set_style_radius(menu_container, 0, LV_PART_MAIN);
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_pad_top(menu_container, 0, 0);
    lv_obj_set_style_pad_bottom(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);

    // Add Scan button
    scan_btn = lv_list_add_btn(menu_container, NULL, "Scan");
    lv_obj_set_height(scan_btn, button_height_global);
    lv_obj_add_style(scan_btn, get_zebra_style(0), 0);
    lv_obj_t *slabel = lv_obj_get_child(scan_btn, 0);
    if (slabel) {
        lv_obj_set_style_text_font(slabel, get_menu_font(), 0);
        vertically_center_label(slabel, scan_btn);
        lv_obj_add_style(slabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(scan_btn, (void *)"Scan");
    lv_obj_add_event_cb(scan_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Scan");

    // Add Saved button
    lv_obj_t *saved_btn = lv_list_add_btn(menu_container, NULL, "Saved");
    lv_obj_set_height(saved_btn, button_height_global);
    lv_obj_add_style(saved_btn, get_zebra_style(1), 0);
    lv_obj_t *svlabel = lv_obj_get_child(saved_btn, 0);
    if (svlabel) {
        lv_obj_set_style_text_font(svlabel, get_menu_font(), 0);
        vertically_center_label(svlabel, saved_btn);
        lv_obj_add_style(svlabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(saved_btn, (void *)"Saved");
    lv_obj_add_event_cb(saved_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Saved");

    // Add User Keys button
    lv_obj_t *keys_btn = lv_list_add_btn(menu_container, NULL, "User Keys");
    lv_obj_set_height(keys_btn, button_height_global);
    lv_obj_add_style(keys_btn, get_zebra_style(2), 0);
    lv_obj_t *klabel = lv_obj_get_child(keys_btn, 0);
    if (klabel) {
        lv_obj_set_style_text_font(klabel, get_menu_font(), 0);
        vertically_center_label(klabel, keys_btn);
        lv_obj_add_style(klabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(keys_btn, (void *)"User Keys");
    lv_obj_add_event_cb(keys_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"User Keys");

    // Add Chameleon Ultra button
#if defined(CONFIG_NFC_CHAMELEON)
    lv_obj_t *cu_btn = lv_list_add_btn(menu_container, NULL, "Chameleon Ultra");
    lv_obj_set_height(cu_btn, button_height_global);
    lv_obj_add_style(cu_btn, get_zebra_style(3), 0);
    lv_obj_t *culabel = lv_obj_get_child(cu_btn, 0);
    if (culabel) {
        lv_obj_set_style_text_font(culabel, get_menu_font(), 0);
        vertically_center_label(culabel, cu_btn);
        lv_obj_add_style(culabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(cu_btn, (void *)"Chameleon Ultra");
    lv_obj_add_event_cb(cu_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Chameleon Ultra");
#endif

    // Add Write button
    emulate_btn = lv_list_add_btn(menu_container, NULL, "Write");
    lv_obj_set_height(emulate_btn, button_height_global);
    lv_obj_add_style(emulate_btn, get_zebra_style(4), 0);
    lv_obj_t *elabel = lv_obj_get_child(emulate_btn, 0);
    if (elabel) {
        lv_obj_set_style_text_font(elabel, get_menu_font(), 0);
        vertically_center_label(elabel, emulate_btn);
        lv_obj_add_style(elabel, &style_menu_label, 0);
    }
    lv_obj_set_user_data(emulate_btn, (void *)"Write");
    lv_obj_add_event_cb(emulate_btn, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"Write");

    num_items = 5;

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    // Add Back option row for hardware users, mirroring options_screen behavior
    lv_obj_t *back_row = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
    if (back_row) {
        lv_obj_set_height(back_row, button_height_global);
        lv_obj_add_style(back_row, get_zebra_style(5), 0);
        lv_obj_t *blabel = lv_obj_get_child(back_row, 0);
        if (blabel) {
            lv_obj_set_style_text_font(blabel, get_menu_font(), 0);
            vertically_center_label(blabel, back_row);
            lv_obj_add_style(blabel, &style_menu_label, 0);
        }
        lv_obj_set_user_data(back_row, (void *)"__BACK_OPTION__");
        lv_obj_add_event_cb(back_row, nfc_option_event_cb, LV_EVENT_CLICKED, (void *)"__BACK_OPTION__");
        num_items++;
    }
#endif

    // add touchscreen nav buttons + back button (same style as options_screen)
#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_nfc_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);
    /* hide scroll buttons until we know whether the list is scrollable */
    lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);

    scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_nfc_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);
    lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);

    back_btn = lv_btn_create(root);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
#endif

    highlight_selected();

    display_manager_add_status_bar("NFC");
#ifdef CONFIG_USE_TOUCHSCREEN
    /* reveal scroll buttons only if the menu is actually scrollable */
    if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_coord_t scroll_bottom = lv_obj_get_scroll_bottom(menu_container);
        lv_coord_t scroll_top = lv_obj_get_scroll_top(menu_container);
        if (scroll_bottom > 0 || scroll_top > 0) {
            if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

void nfc_view_destroy(void) {
    ESP_LOGI(TAG, "nfc_view_destroy");
    // Ensure any running scan is cancelled and resources are released
    cleanup_nfc_scan_popup(NULL); // sets nfc_scan_cancel=true
    nfc_scan_cancel = true;
    // Cancel any active write and cleanup popup
    nfc_write_cancel = true;
    cleanup_nfc_write_popup(NULL);
    // cleanup chameleon popup
    cleanup_cu_popup(NULL);
    // Cleanup saved popup and list
    cleanup_saved_details_popup(NULL);
    saved_clear_list();
    in_saved_list = false;

    if (root) {
        lv_obj_del(root);
        root = NULL;
    }
    nfc_view.root = NULL;
    menu_container = NULL;
    scan_btn = NULL;
    emulate_btn = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    nfc_scan_popup = NULL;
    nfc_scan_cancel_btn = NULL;
    nfc_title_label = NULL;
    nfc_uid_label = NULL;
    nfc_type_label = NULL;
    nfc_details_label = NULL;

#ifdef CONFIG_NFC_PN532
    // If scan task already exited, release PN532 here as a safety net
    if (nfc_scan_task_handle == NULL && g_pn532) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
#endif
}

void get_nfc_callback(void **cb) {
    if (cb) *cb = nfc_view_input_cb;
}

View nfc_view = {
    .root = NULL,
    .create = nfc_view_create,
    .destroy = nfc_view_destroy,
    .input_callback = nfc_view_input_cb,
    .name = "NFC",
    .get_hardwareinput_callback = get_nfc_callback
};

static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w) {
    if (desired < min_w) return min_w;
    if (desired > max_w) return max_w;
    return desired;
}

static void layout_popup_buttons_row(
    lv_obj_t *popup,
    lv_obj_t **btns,
    int count,
    lv_coord_t min_w,
    lv_coord_t max_w,
    lv_coord_t min_threshold,
    lv_coord_t gap,
    lv_coord_t yoff
) {
    if (!popup || !btns || count <= 0) return;

    /* Respect the popup's own left/right padding so we don't double-count margins. */
    lv_coord_t popup_w = lv_obj_get_width(popup);
    lv_coord_t left_pad = lv_obj_get_style_pad_left(popup, LV_PART_MAIN);
    lv_coord_t right_pad = lv_obj_get_style_pad_right(popup, LV_PART_MAIN);
    if (left_pad == 0 && right_pad == 0) {
        /* fallback to prior behavior for older themes */
        left_pad = 10; right_pad = 10;
    }
    lv_coord_t available_w = popup_w - left_pad - right_pad;
    if (available_w < 0) available_w = popup_w;

    /* Compute a per-button width that fits the available area, honor min/max */
    lv_coord_t btn_w = (available_w - (gap * (count - 1))) / count;
    btn_w = clamp_button_width(btn_w, min_w, max_w);

    while (((btn_w * count) + (gap * (count - 1))) > available_w && btn_w > min_threshold) {
        btn_w--;
    }
    if (btn_w < min_threshold) btn_w = min_threshold;

    /* Center the group within available area (inside popup padding) */
    lv_coord_t total_w = (btn_w * count) + (gap * (count - 1));
    lv_coord_t start_x = left_pad;
    if (available_w > total_w) start_x += (available_w - total_w) / 2;

    lv_coord_t x = start_x;
    lv_coord_t btn_h = 0;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *btn = btns[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        if (btn_h == 0) {
            btn_h = lv_obj_get_height(btn);
            if (btn_h <= 0) btn_h = (LV_HOR_RES <= 240) ? 30 : 34;
        }
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, yoff);
        x += btn_w + gap;
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_center(lbl);
    }
}
