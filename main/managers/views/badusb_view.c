#include "sdkconfig.h"

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)

#include "managers/views/badusb_view.h"
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/settings_manager.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/popup.h"
#include "gui/lvgl_safe.h"
#include "managers/views/error_popup.h"
#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "core/esp_comm_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "managers/sd_card_manager.h"
#include "managers/badusb_builtin_script.h"

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#endif

static const char *TAG = "badusb_view";

// JIT SD helpers for configs that unmount SD after init (e.g. somethingsomething)
static bool badusb_sd_begin(bool *display_was_suspended)
{
    if (display_was_suspended) *display_was_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
        strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
        esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
        if (mount_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(mount_err));
            return false;
        }
        return true;
    }
#endif
    return true;
}

static void badusb_sd_end(bool display_was_suspended)
{
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
        strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
#else
    (void)display_was_suspended;
#endif
}

typedef enum {
    BADUSB_MENU_MAIN,
    BADUSB_MENU_SCRIPT_SELECT,
    BADUSB_MENU_SETTINGS,
} BadUsbMenuState;

static BadUsbMenuState current_menu_state = BADUSB_MENU_MAIN;

static const char *badusb_main_options[] = {
    "Run Script",
    "Settings",
    "< Back",
    NULL
};

#define BADUSB_SETTINGS_COUNT 8
static char settings_labels[BADUSB_SETTINGS_COUNT][80];
static const char *settings_options[BADUSB_SETTINGS_COUNT + 1];
static const char *kb_layout_names[] = {"US", "DE", "FR", "UK", "ES"};

static void populate_settings_labels(void) {
    snprintf(settings_labels[0], 80, "VID: 0x%04X", settings_get_badusb_vid(&G_Settings));
    snprintf(settings_labels[1], 80, "PID: 0x%04X", settings_get_badusb_pid(&G_Settings));
    snprintf(settings_labels[2], 80, "Manufacturer: %s", settings_get_badusb_manufacturer(&G_Settings));
    snprintf(settings_labels[3], 80, "Product: %s", settings_get_badusb_product(&G_Settings));
    uint8_t layout = settings_get_badusb_kb_layout(&G_Settings);
    if (layout >= KB_LAYOUT_COUNT) layout = KB_LAYOUT_US;
    snprintf(settings_labels[4], 80, "Layout: %s", kb_layout_names[layout]);
    snprintf(settings_labels[5], 80, "Randomize: %s", settings_get_badusb_randomize(&G_Settings) ? "On" : "Off");
    strcpy(settings_labels[6], "Reset To Defaults");
    strcpy(settings_labels[7], "< Back");
    for (int i = 0; i < BADUSB_SETTINGS_COUNT; i++) {
        settings_options[i] = settings_labels[i];
    }
    settings_options[BADUSB_SETTINGS_COUNT] = NULL;
}

#define MAX_SCRIPTS 32
#define MAX_SCRIPT_NAME 64
static char script_names[MAX_SCRIPTS][MAX_SCRIPT_NAME];
static const char *script_options[MAX_SCRIPTS + 2];
static int script_count = 0;

static lv_obj_t *root = NULL;
static options_view_t *g_ov = NULL;
static lv_obj_t *menu_container = NULL;
static int selected_item_index = 0;
static int num_items = 0;

static int touch_start_x = 0;
static int touch_start_y = 0;
static bool touch_started = false;
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int SWIPE_THRESHOLD_RATIO = 1;
#else
static const int SWIPE_THRESHOLD_RATIO = 10;
#endif

static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5

static lv_obj_t *badusb_running_popup = NULL;
static lv_obj_t *badusb_popup_title_lbl = NULL;
static lv_obj_t *badusb_popup_body_lbl = NULL;
static lv_timer_t *vsense_poll_timer = NULL;
static char vsense_pending_script[MAX_SCRIPT_NAME];

static bool badusb_is_remote(void) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
    return true;
#else
    return false;
#endif
}

static void select_item(int index) {
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;
    selected_item_index = index;
    if (g_ov) {
        options_view_set_selected(g_ov, selected_item_index);
    }
}

static void populate_script_list(void) {
    script_count = 0;

    strncpy(script_names[script_count], BADUSB_BUILTIN_SCRIPT_NAME, MAX_SCRIPT_NAME - 1);
    script_names[script_count][MAX_SCRIPT_NAME - 1] = '\0';
    script_count++;

    bool display_was_suspended = false;
    if (badusb_sd_begin(&display_was_suspended)) {
        const char *dir_path = "/mnt/ghostesp/badusb";
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && script_count < MAX_SCRIPTS) {
                size_t len = strlen(entry->d_name);
                if (len > 4 && strcmp(entry->d_name + len - 4, ".txt") == 0) {
                    strncpy(script_names[script_count], entry->d_name, MAX_SCRIPT_NAME - 1);
                    script_names[script_count][MAX_SCRIPT_NAME - 1] = '\0';
                    script_count++;
                }
            }
            closedir(dir);
        }
        badusb_sd_end(display_was_suspended);
    }

    for (int i = 0; i < script_count; i++) {
        script_options[i] = script_names[i];
    }
    script_options[script_count] = "< Back";
    script_options[script_count + 1] = NULL;
}

static void add_items_with_userdata(options_view_t *ov, const char **labels) {
    if (!ov || !labels) return;
    for (int i = 0; labels[i]; i++) {
        lv_obj_t *btn = options_view_add_item(ov, labels[i], NULL, NULL);
        if (btn) {
            lv_obj_set_user_data(btn, (void *)labels[i]);
        }
    }
}

static void on_item_click(lv_event_t *e) {
    (void)e;
}

static void scroll_up_cb(lv_event_t *e) {
    (void)e;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
        lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
    }
}

static void scroll_down_cb(lv_event_t *e) {
    (void)e;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
        lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
    }
}

static void update_scroll_buttons_visibility(void) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;
    lv_obj_update_layout(menu_container);
    
    lv_coord_t scroll_bottom = lv_obj_get_scroll_bottom(menu_container);
    lv_coord_t scroll_top = lv_obj_get_scroll_top(menu_container);
    bool needs_scroll = (scroll_bottom > 0) || (scroll_top > 0);
    
    if (needs_scroll) {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
            lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_up_btn);
        }
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
            lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_down_btn);
        }
    } else {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rebuild_menu(void);
static void go_back(void);

static void back_btn_cb(lv_event_t *e) {
    (void)e;
    go_back();
}

static void badusb_vid_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid VID");
        return;
    }
    uint16_t val = (uint16_t)strtol(text, NULL, 16);
    settings_set_badusb_vid(&G_Settings, val);
    settings_persist_setting(SETTING_BADUSB_VID);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_pid_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid PID");
        return;
    }
    uint16_t val = (uint16_t)strtol(text, NULL, 16);
    settings_set_badusb_pid(&G_Settings, val);
    settings_persist_setting(SETTING_BADUSB_PID);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_mfr_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid name");
        return;
    }
    settings_set_badusb_manufacturer(&G_Settings, text);
    settings_persist_setting(SETTING_BADUSB_MANUFACTURER);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_prod_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid name");
        return;
    }
    settings_set_badusb_product(&G_Settings, text);
    settings_persist_setting(SETTING_BADUSB_PRODUCT);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_cancel_cb(lv_event_t *e) {
    (void)e;
    bool remote = badusb_is_remote();
    if (remote) {
        simulateCommand("commsend badusb stop");
    } else {
#ifdef CONFIG_HAS_BADUSB
        badusb_manager_stop();
#endif
    }
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;
    error_popup_create("BadUSB stopped");
}

static void show_running_popup_ex(const char *script_name, bool waiting_for_usb) {
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;

    int popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 10;

    if (LV_VER_RES < 160) {
        popup_h = LV_VER_RES - 40;
        if (popup_h < 100) popup_h = 100;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 190) ? (LV_VER_RES - 40) : 130;
        if (popup_h < 110) popup_h = 110;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 130 : 140;
    }

    badusb_running_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;

    const char *title = waiting_for_usb ? "Waiting for USB..." : "BadUSB Running";
    badusb_popup_title_lbl = popup_create_title_label(badusb_running_popup, title, title_font, 12);

    char body[80];
    if (waiting_for_usb) {
        snprintf(body, sizeof(body), "Plug in to execute\n%s", script_name);
    } else {
        snprintf(body, sizeof(body), "Script: %s", script_name);
    }
    
    // smaller screens need tighter spacing to avoid overlap with cancel button
    int body_y_offset = (LV_VER_RES < 160) ? 32 : ((LV_VER_RES <= 200) ? 35 : 40);
    badusb_popup_body_lbl = popup_create_body_label(badusb_running_popup, body, popup_w - 20, true, body_font, body_y_offset);
    if (badusb_popup_body_lbl) {
        lv_obj_set_style_text_align(badusb_popup_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    // Store script name for status updates
    strncpy(vsense_pending_script, script_name, MAX_SCRIPT_NAME - 1);
    vsense_pending_script[MAX_SCRIPT_NAME - 1] = '\0';

    int btn_w = 90, btn_h = 30;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 28; }
    lv_obj_t *cancel_btn = popup_add_styled_button(badusb_running_popup, "Cancel", btn_w, btn_h,
                                                   LV_ALIGN_BOTTOM_MID, 0, -10, body_font,
                                                   badusb_cancel_cb, NULL);
    if (cancel_btn) {
        popup_set_button_selected(cancel_btn, true);
    }
}

static void show_running_popup(const char *script_name) {
    show_running_popup_ex(script_name, false);
}

// Update the popup in-place when receiving status from S3 (remote) or VSENSE poll (standalone)
static void badusb_popup_set_running(void) {
    if (!badusb_running_popup || !lv_obj_is_valid(badusb_running_popup)) return;
    if (badusb_popup_title_lbl && lv_obj_is_valid(badusb_popup_title_lbl)) {
        lv_label_set_text(badusb_popup_title_lbl, "BadUSB Running");
    }
    if (badusb_popup_body_lbl && lv_obj_is_valid(badusb_popup_body_lbl)) {
        char body[80];
        snprintf(body, sizeof(body), "Script: %s", vsense_pending_script);
        lv_label_set_text(badusb_popup_body_lbl, body);
    }
}

static void badusb_popup_set_done(void) {
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;
}

#ifdef CONFIG_HAS_BADUSB
// LVGL timer callback for standalone VSENSE polling
static void vsense_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (badusb_vsense_connected()) {
        // VBUS detected - update popup to "Running"
        badusb_popup_set_running();
        // Stop polling
        if (vsense_poll_timer) {
            lv_timer_del(vsense_poll_timer);
            vsense_poll_timer = NULL;
        }
    }
}
#endif

// Public: called from command handler when S3 sends "badusb status <state>" to C5
void badusb_view_update_status(const char *status) {
    if (!status) return;
    if (strcmp(status, "waiting") == 0) {
        // S3 is waiting for VBUS - show waiting popup if not already showing
        if (!badusb_running_popup || !lv_obj_is_valid(badusb_running_popup)) {
            show_running_popup_ex(vsense_pending_script, true);
        }
    } else if (strcmp(status, "running") == 0) {
        badusb_popup_set_running();
    } else if (strcmp(status, "done") == 0) {
        badusb_popup_set_done();
    }
}

#ifdef CONFIG_HAS_BADUSB_REMOTE
#define STREAM_CHUNK_SIZE 56
#define BADUSB_SETTINGS_SEND_DELAY_MS 50

static void badusb_send_settings_to_peer(void) {
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "set_vid 0x%04X", settings_get_badusb_vid(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_pid 0x%04X", settings_get_badusb_pid(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_mfr \"%s\"", settings_get_badusb_manufacturer(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_prod \"%s\"", settings_get_badusb_product(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_rand %u", settings_get_badusb_randomize(&G_Settings) ? 1 : 0);
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_layout %u", settings_get_badusb_kb_layout(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));
}

static bool badusb_send_script_to_peer(const char *name) {
    bool display_was_suspended = false;
    if (!badusb_sd_begin(&display_was_suspended)) {
        error_popup_create("Failed to mount SD");
        return false;
    }

    char path[128];
    snprintf(path, sizeof(path), "/mnt/ghostesp/badusb/%s", name);

    FILE *f = fopen(path, "r");
    if (!f) {
        badusb_sd_end(display_was_suspended);
        error_popup_create("Failed to open script");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        badusb_sd_end(display_was_suspended);
        error_popup_create("Invalid script size");
        return false;
    }

    char exec_data[32];
    snprintf(exec_data, sizeof(exec_data), "exec %ld", file_size);
    if (!esp_comm_manager_send_command("badusb", exec_data)) {
        fclose(f);
        badusb_sd_end(display_was_suspended);
        error_popup_create("Failed to send command");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t chunk[STREAM_CHUNK_SIZE];
    size_t total_sent = 0;
    bool ok = true;

    while (total_sent < (size_t)file_size) {
        size_t n = fread(chunk, 1, STREAM_CHUNK_SIZE, f);
        if (n == 0) break;

        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_BADUSB, chunk, n)) {
            ok = false;
            break;
        }
        total_sent += n;

        vTaskDelay(pdMS_TO_TICKS(15));
    }

    fclose(f);
    badusb_sd_end(display_was_suspended);

    if (!ok || total_sent != (size_t)file_size) {
        error_popup_create("Script transfer failed");
        return false;
    }

    ESP_LOGI(TAG, "Streamed %zu bytes to peer", total_sent);
    return true;
}
static bool badusb_send_builtin_to_peer(void) {
    size_t script_size = BADUSB_BUILTIN_SCRIPT_LEN;

    char exec_data[32];
    snprintf(exec_data, sizeof(exec_data), "exec %zu", script_size);
    if (!esp_comm_manager_send_command("badusb", exec_data)) {
        error_popup_create("Failed to send command");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    const uint8_t *data = (const uint8_t *)badusb_builtin_script;
    size_t total_sent = 0;
    bool ok = true;

    while (total_sent < script_size) {
        size_t remaining = script_size - total_sent;
        size_t n = (remaining < STREAM_CHUNK_SIZE) ? remaining : STREAM_CHUNK_SIZE;

        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_BADUSB, data + total_sent, n)) {
            ok = false;
            break;
        }
        total_sent += n;
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (!ok || total_sent != script_size) {
        error_popup_create("Script transfer failed");
        return false;
    }

    ESP_LOGI(TAG, "Streamed %zu bytes (built-in) to peer", total_sent);
    return true;
}
#endif // CONFIG_HAS_BADUSB_REMOTE

static void handle_option(const char *option) {
    if (!option) return;
    bool remote = badusb_is_remote();

    if (current_menu_state == BADUSB_MENU_MAIN) {
        if (strcmp(option, "Settings") == 0) {
            populate_settings_labels();
            current_menu_state = BADUSB_MENU_SETTINGS;
            rebuild_menu();
        } else if (strcmp(option, "Run Script") == 0) {
            if (remote && !esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to peer");
                return;
            }
            populate_script_list();
            if (script_count == 0) {
                error_popup_create("No scripts found");
                return;
            }
            current_menu_state = BADUSB_MENU_SCRIPT_SELECT;
            rebuild_menu();
        } else if (strcmp(option, "< Back") == 0) {
            go_back();
        }
    } else if (current_menu_state == BADUSB_MENU_SETTINGS) {
        if (strncmp(option, "VID:", 4) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_vid_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "PID:", 4) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_pid_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Manufacturer:", 13) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_mfr_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Product:", 8) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_prod_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Layout:", 7) == 0) {
            uint8_t layout = settings_get_badusb_kb_layout(&G_Settings);
            layout = (layout + 1) % KB_LAYOUT_COUNT;
            settings_set_badusb_kb_layout(&G_Settings, layout);
            settings_persist_setting(SETTING_BADUSB_KB_LAYOUT);
            snprintf(settings_labels[4], 80, "Layout: %s", kb_layout_names[layout]);
            options_view_update_item_text(g_ov, 4, settings_labels[4]);
        } else if (strncmp(option, "Randomize:", 10) == 0) {
            bool enabled = !settings_get_badusb_randomize(&G_Settings);
            settings_set_badusb_randomize(&G_Settings, enabled);
            settings_persist_setting(SETTING_BADUSB_RANDOMIZE);
            snprintf(settings_labels[5], 80, "Randomize: %s", enabled ? "On" : "Off");
            options_view_update_item_text(g_ov, 5, settings_labels[5]);
        } else if (strcmp(option, "Reset To Defaults") == 0) {
            settings_reset_badusb_defaults(&G_Settings);
            settings_persist_setting(SETTING_BADUSB_VID);
            settings_persist_setting(SETTING_BADUSB_PID);
            settings_persist_setting(SETTING_BADUSB_MANUFACTURER);
            settings_persist_setting(SETTING_BADUSB_PRODUCT);
            settings_persist_setting(SETTING_BADUSB_RANDOMIZE);
            settings_persist_setting(SETTING_BADUSB_KB_LAYOUT);
            populate_settings_labels();
            rebuild_menu();
        } else if (strcmp(option, "< Back") == 0) {
            go_back();
        }
    } else if (current_menu_state == BADUSB_MENU_SCRIPT_SELECT) {
        if (strcmp(option, "< Back") == 0) {
            go_back();
            return;
        }
        bool is_builtin = (strcmp(option, BADUSB_BUILTIN_SCRIPT_NAME) == 0);
        if (remote) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
            badusb_send_settings_to_peer();
            bool ok = is_builtin ? badusb_send_builtin_to_peer()
                                 : badusb_send_script_to_peer(option);
            if (ok) {
                show_running_popup_ex(option, true);
            }
#endif
        } else {
#ifdef CONFIG_HAS_BADUSB
            bool has_vsense = badusb_has_vsense();
            bool already_connected = has_vsense && badusb_vsense_connected();

            if (is_builtin) {
                char *buf = strdup(badusb_builtin_script);
                if (buf) {
                    badusb_manager_execute_buffer(buf, BADUSB_BUILTIN_SCRIPT_LEN);
                }
            } else {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "badusb run %s", option);
                simulateCommand(cmd);
            }

            if (has_vsense && !already_connected) {
                show_running_popup_ex(option, true);
                if (vsense_poll_timer) {
                    lv_timer_del(vsense_poll_timer);
                }
                vsense_poll_timer = lv_timer_create(vsense_poll_timer_cb, 100, NULL);
            } else {
                show_running_popup(option);
            }
#endif
        }
    }
}

static void go_back(void) {
    if (current_menu_state != BADUSB_MENU_MAIN) {
        current_menu_state = BADUSB_MENU_MAIN;
        rebuild_menu();
    } else {
        display_manager_switch_view(&main_menu_view);
    }
}

static void rebuild_menu(void) {
    if (!g_ov) return;

    options_view_clear(g_ov);
    selected_item_index = 0;
    num_items = 0;

    const char **options = NULL;
    const char *title = "BadUSB";

    switch (current_menu_state) {
        case BADUSB_MENU_MAIN:
            options = badusb_main_options;
            break;
        case BADUSB_MENU_SCRIPT_SELECT:
            options = script_options;
            title = "Select Script";
            break;
        case BADUSB_MENU_SETTINGS:
            options = settings_options;
            title = "Settings";
            break;
    }

    if (options) {
        options_view_set_title(g_ov, title);
        add_items_with_userdata(g_ov, options);
        for (const char **p = options; *p; p++) num_items++;
    }

    menu_container = options_view_get_list(g_ov);
    if (num_items > 0) {
        select_item(0);
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

void badusb_view_create(void) {
    display_manager_fill_screen(lv_color_hex(0x121212));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root(NULL, NULL, lv_color_hex(0x121212), LV_OPA_COVER);
    badusb_view.root = root;

    g_ov = options_view_create(root, "BadUSB");
    menu_container = options_view_get_list(g_ov);

#ifdef CONFIG_USE_TOUCHSCREEN
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, LV_HOR_RES, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    current_menu_state = BADUSB_MENU_MAIN;
    selected_item_index = 0;
    num_items = 0;

    const char **options = badusb_main_options;
    add_items_with_userdata(g_ov, options);
    for (const char **p = options; *p; p++) num_items++;
    if (num_items > 0) select_item(0);

#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);
    lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);

    scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_down_cb, LV_EVENT_CLICKED, NULL);
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
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    update_scroll_buttons_visibility();
#endif
}

void badusb_view_destroy(void) {
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;

    if (g_ov) {
        options_view_destroy(g_ov);
        g_ov = NULL;
    }

    lvgl_obj_del_safe(&root);
    badusb_view.root = NULL;
    menu_container = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    selected_item_index = 0;
    num_items = 0;
    current_menu_state = BADUSB_MENU_MAIN;
}

static void get_badusb_callback(void **callback) {
    *callback = badusb_view.input_callback;
}

void badusb_view_input_cb(InputEvent *event) {
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        if (event->type == INPUT_TYPE_KEYBOARD) {
            uint8_t key = event->data.key_value;
            if (key == 13 || key == 10 || key == 27 || key == 29 || key == 'c' || key == 'C') {
                badusb_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            if (event->data.joystick_index == 0 || event->data.joystick_index == 1) {
                badusb_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                badusb_cancel_cb(NULL);
                return;
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
                lv_area_t area; lv_obj_get_coords(scroll_up_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    scroll_up_cb(NULL);
                    touch_started = false;
                    return;
                }
            }
            if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
                lv_area_t area; lv_obj_get_coords(scroll_down_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    scroll_down_cb(NULL);
                    touch_started = false;
                    return;
                }
            }
            if (back_btn && lv_obj_is_valid(back_btn)) {
                lv_area_t area; lv_obj_get_coords(back_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    go_back();
                    touch_started = false;
                    return;
                }
            }
            if (!touch_started) {
                touch_started = true;
                touch_start_x = data->point.x;
                touch_start_y = data->point.y;
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!touch_started) return;
            touch_started = false;

            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            int thr_y = LV_VER_RES / SWIPE_THRESHOLD_RATIO;
            int thr_x = LV_HOR_RES / SWIPE_THRESHOLD_RATIO;

            if (!menu_container) return;
            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            bool started_in_container = (touch_start_x >= cont_area.x1 && touch_start_x <= cont_area.x2 &&
                                         touch_start_y >= cont_area.y1 && touch_start_y <= cont_area.y2);
            if (!started_in_container) return;

            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int container_h = (int)(cont_area.y2 - cont_area.y1);
                if (container_h > 0) {
                    int y_rel = (int)data->point.y - (int)cont_area.y1;
                    if (y_rel < container_h / 3) {
                        select_item(selected_item_index - 1);
                    } else if (y_rel > (container_h * 2) / 3) {
                        select_item(selected_item_index + 1);
                    } else {
                        lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                        if (sel) {
                            const char *opt = (const char *)lv_obj_get_user_data(sel);
                            if (opt) handle_option(opt);
                        }
                    }
                    return;
                }
            }

            if (abs(dy) > thr_y) {
                lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                return;
            }
            if (abs(dx) > thr_x) return;

            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    select_item(i);
                    const char *opt = (const char *)lv_obj_get_user_data(btn);
                    if (opt) handle_option(opt);
                    return;
                }
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) {
            select_item(selected_item_index - 1);
        } else if (button == 4) {
            select_item(selected_item_index + 1);
        } else if (button == 1) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) handle_option(opt);
            }
        } else if (button == 0) {
            go_back();
        }
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if (keyValue == 'k' || keyValue == 59 || keyValue == ';') {
            select_item(selected_item_index - 1);
        } else if (keyValue == 'j' || keyValue == 46 || keyValue == '.') {
            select_item(selected_item_index + 1);
        } else if (keyValue == 'h' || keyValue == 44 || keyValue == ',') {
            select_item(selected_item_index - 1);
        } else if (keyValue == 'l' || keyValue == 47 || keyValue == '/') {
            select_item(selected_item_index + 1);
        } else if (keyValue == 13) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) handle_option(opt);
            }
        } else if (keyValue == 29 || keyValue == '`') {
            go_back();
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *opt = (const char *)lv_obj_get_user_data(selected_obj);
                if (opt) handle_option(opt);
            }
        } else {
            if (event->data.encoder.direction > 0) {
                select_item(selected_item_index + 1);
            } else {
                select_item(selected_item_index - 1);
            }
        }
        return;
    }

#ifdef CONFIG_USE_ENCODER
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
    }
#endif
}

View badusb_view = {
    .root = NULL,
    .create = badusb_view_create,
    .destroy = badusb_view_destroy,
    .input_callback = badusb_view_input_cb,
    .name = "BadUSB",
    .get_hardwareinput_callback = get_badusb_callback
};

#endif // CONFIG_HAS_BADUSB || CONFIG_HAS_BADUSB_REMOTE
