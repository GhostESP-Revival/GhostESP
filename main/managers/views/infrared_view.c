#include "managers/views/infrared_view.h"
#include "esp_log.h"
#include "managers/views/keyboard_screen.h"
#include "managers/settings_manager.h"

void update_learning_popup_selection(void);
void update_easy_learn_popup_selection(void);
void update_easy_learn_instruction_text(void);
void easy_learn_toggle_cb(lv_event_t *e);
void easy_learn_cancel_cb(lv_event_t *e);
void easy_learn_skip_cb(lv_event_t *e);
void create_easy_learn_popup(void);
void cleanup_easy_learn_popup(void *obj);
void easy_learn_signal_name_callback(void);
bool is_button_name_used(const char* remote_path, const char* button_name);
const char* get_next_available_button_name(const char* remote_path);
void generate_unique_remote_filename(const char* base_name, char* output_filename, size_t output_size);

static const char *TAG = "infrared_view";

#ifdef CONFIG_HAS_INFRARED_RX
void cleanup_signal_preview_popup(void *obj);
void signal_preview_save_cb(lv_event_t *e);
void signal_preview_cancel_cb(lv_event_t *e);
void update_signal_preview_selection(void);
void learned_signal_name_callback(const char *name);
void learning_cancel_cb(lv_event_t *e);
static void save_learned_signal(const char *signal_name);
static void create_learning_popup(void);
static void start_ir_learning_task(void);

static void rename_remote_keyboard_callback(const char *name);
static void add_signal_keyboard_callback(const char *name);
#endif

void rename_remote_cb(lv_event_t *e);
void add_signal_cb(lv_event_t *e);
void delete_remote_cb(lv_event_t *e);




#ifndef JOYSTICK_LEFT
#define JOYSTICK_LEFT    (-1)
#endif
#ifndef JOYSTICK_RIGHT
#define JOYSTICK_RIGHT   (1)
#endif
#ifndef JOYSTICK_PRESS
#define JOYSTICK_PRESS   (0)
#endif
#ifndef ENCODER_LEFT
#define ENCODER_LEFT     (-1)
#endif
#ifndef ENCODER_RIGHT
#define ENCODER_RIGHT    (1)
#endif
#ifndef ENCODER_PRESS
#define ENCODER_PRESS    (0)
#endif

#ifdef CONFIG_HAS_INFRARED_RX
static lv_style_t popup_style;
static bool popup_style_initialized = false;
#endif

#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/keyboard_screen.h"
#include <lvgl/lvgl.h>
#include <dirent.h>
#include <string.h>
#include "managers/infrared_manager.h"
#include "managers/infrared_decoder.h"
#include "managers/rgb_manager.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#ifdef CONFIG_HAS_INFRARED_RX
#include "driver/rmt_rx.h"
#include <time.h>
#include "driver/gpio.h"
#endif

static lv_obj_t *root = NULL;
static lv_obj_t *list = NULL;
static int selected_ir_index = 0;
static int num_ir_items = 0;
static char **ir_file_paths = NULL;
static size_t ir_file_count = 0;
static infrared_signal_t *signals = NULL;
static size_t signal_count = 0;
static bool showing_commands = false;
static char current_dir[256] = "/mnt/ghostesp";
static bool has_remotes_option = false;
static bool has_universals_option = false;
static bool in_universals_mode = false;
static char **uni_command_names = NULL;
static size_t uni_command_count = 0;
static char current_universal_file[256] = "";
static lv_obj_t *transmitting_popup = NULL;
static TaskHandle_t universal_task_handle = NULL;

#ifdef CONFIG_HAS_INFRARED_RX

static lv_obj_t *learning_popup = NULL;
static lv_obj_t *learning_cancel_btn = NULL;
static TaskHandle_t ir_learning_task_handle = NULL;
static bool ir_learning_cancel = false;
static rmt_channel_handle_t rx_channel = NULL;
static infrared_signal_t learned_signal = {0};
static char learned_signal_name[64] = {0};
static bool add_signal_mode = false;
static bool preserve_learned_signal = false;

static bool is_easy_mode = false;
static int existing_remote_button_index = 0;

static const char *common_button_names[] = {
    "Power", "Vol_up", "Vol_dn", "Mute", "Ch_up",
    "Ch_dn", "Ok", "Up", "Down", "Left",
    "Right", "Menu", "Back", "Play", "Pause",
    "Stop", "Next", "Prev", "FF", "Rew",
    "Input", "Exit", "Eject", "Subtitle"
};
static const int num_common_buttons = sizeof(common_button_names) / sizeof(common_button_names[0]);

static lv_obj_t *easy_learn_popup = NULL;
static lv_obj_t *easy_learn_cancel_btn = NULL;
static lv_obj_t *easy_learn_skip_btn = NULL;
static lv_obj_t *easy_learn_instruction_label = NULL;
static int easy_learn_selected_option = 0;

static lv_obj_t *signal_preview_popup = NULL;
static lv_obj_t *protocol_label = NULL;
static lv_obj_t *address_label = NULL;
static lv_obj_t *command_label = NULL;
static lv_obj_t *save_btn = NULL;
static lv_obj_t *cancel_btn = NULL;
static int preview_selected_option = 0;
static bool signal_decoded = false;
static InfraredDecodedMessage *decoded_message = NULL;
static InfraredDecoderContext *decoder_context = NULL;
static QueueHandle_t ir_rx_queue = NULL;

static bool ir_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx);
#endif
static void command_event_cb(lv_event_t *e);
static volatile bool universal_transmit_cancel = false;

static char current_remote_path[256] = "";
static char current_remote_name[64] = "";

static void rename_remote_keyboard_callback(const char *name) {
    if (!name || strlen(name) == 0) {
        display_manager_switch_view(&infrared_view);
        return;
    }
    
    char new_path[256];
    char old_path[256];
    char dir_path[256];

    strncpy(dir_path, current_remote_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    size_t dir_len = strlen(dir_path);
    size_t max_name_len = sizeof(new_path) - dir_len - 16;
    
    if (max_name_len <= 0 || max_name_len > 200) {
        max_name_len = 200;
    }
    
    char truncated_name[max_name_len + 1];
    strncpy(truncated_name, name, max_name_len);
    truncated_name[max_name_len] = '\0';
    
    char safe_dir_path[128];
    char filename_part[64];
    
    strncpy(safe_dir_path, dir_path, sizeof(safe_dir_path) - 1);
    strncpy(safe_dir_path, dir_path, sizeof(safe_dir_path) - 1);
    safe_dir_path[sizeof(safe_dir_path) - 1] = '\0';
    
    strncpy(filename_part, truncated_name, sizeof(filename_part) - 1);
    filename_part[sizeof(filename_part) - 1] = '\0';
    
    if (strlen(safe_dir_path) + strlen(filename_part) + 4 >= sizeof(new_path)) {
        ESP_LOGE(TAG, "Path would be too long");
        display_manager_switch_view(&infrared_view);
        return;
    }
    
    snprintf(new_path, sizeof(new_path), "%s/%s.ir", safe_dir_path, filename_part);
    strncpy(old_path, current_remote_path, sizeof(old_path) - 1);
    old_path[sizeof(old_path) - 1] = '\0';
    
    if (rename(old_path, new_path) == 0) {
        ESP_LOGI(TAG, "Renamed remote from %s to %s", old_path, new_path);
        strncpy(current_remote_path, new_path, sizeof(current_remote_path) - 1);
        current_remote_path[sizeof(current_remote_path) - 1] = '\0';
        strncpy(current_remote_name, name, sizeof(current_remote_name) - 1);
        current_remote_name[sizeof(current_remote_name) - 1] = '\0';
    } else {
        ESP_LOGE(TAG, "Failed to rename remote from %s to %s", old_path, new_path);
    }
    
    display_manager_switch_view(&infrared_view);
}

static void add_signal_keyboard_callback(const char *name) {
    if (!name || strlen(name) == 0) {
        display_manager_switch_view(&infrared_view);
        return;
    }
    
    ESP_LOGI(TAG, "Adding new signal with name: %s", name);
    display_manager_switch_view(&infrared_view);
}

#ifdef CONFIG_HAS_INFRARED_RX

static void ir_learning_task(void *arg);
static void add_signal_to_remote_callback(const char *name);
static void append_signal_to_remote(const char *signal_name);
#endif




#ifdef CONFIG_HAS_INFRARED_RX
static void add_signal_to_remote_callback(const char *name)
{
    if (name) {
        strncpy(learned_signal_name, name, sizeof(learned_signal_name) - 1);
        learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
        append_signal_to_remote(learned_signal_name);
    }
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    // Return to infrared view
    display_manager_switch_view(&infrared_view);
}

void learned_signal_name_callback(const char *name)
{
    if (name) {
        strncpy(learned_signal_name, name, sizeof(learned_signal_name) - 1);
        learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
        
        // Store whether we were in add signal mode before saving
        bool was_adding_to_existing = add_signal_mode && strlen(current_remote_path) > 0;
        
        if (add_signal_mode) {
            // Adding signal to existing remote
            append_signal_to_remote(learned_signal_name);
        } else {
            // Learning new remote
            save_learned_signal(learned_signal_name);
        }
        
        // Reset the add signal mode flag
        add_signal_mode = false;
        
        // If we were adding to an existing remote, reload the remote's signal list to stay on it
        if (was_adding_to_existing) {
            // Reload the current remote's signals to refresh the view
            if (signals) {
                infrared_manager_free_list(signals, signal_count);
                signals = NULL;
                signal_count = 0;
            }
            if (infrared_manager_read_list(current_remote_path, &signals, &signal_count)) {
                // Rebuild the signal list UI with proper styling
                lv_obj_clean(list);
                num_ir_items = signal_count;
                selected_ir_index = 0;
                
                for (size_t i = 0; i < signal_count; i++) {
                    const char *cmd_name = signals[i].name;
                    lv_obj_t *btn = lv_list_add_btn(list, NULL, cmd_name);
                    lv_obj_set_width(btn, LV_HOR_RES);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) {
                        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                    }
                    lv_obj_add_event_cb(btn, command_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                    lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                }
                
                // Add remote management options with proper styling
                lv_obj_t *rename_btn = lv_list_add_btn(list, NULL, "Rename Remote");
                lv_obj_set_width(rename_btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(rename_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(rename_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(rename_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(rename_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *rename_label = lv_obj_get_child(rename_btn, 0);
                if (rename_label) {
                    lv_obj_set_style_text_font(rename_label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(rename_label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(rename_btn, rename_remote_cb, LV_EVENT_CLICKED, NULL);
                lv_obj_set_user_data(rename_btn, (void *)(intptr_t)signal_count);
                
                lv_obj_t *add_signal_btn = lv_list_add_btn(list, NULL, "Add Signal");
                lv_obj_set_width(add_signal_btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(add_signal_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(add_signal_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(add_signal_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(add_signal_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *add_signal_label = lv_obj_get_child(add_signal_btn, 0);
                if (add_signal_label) {
                    lv_obj_set_style_text_font(add_signal_label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(add_signal_label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(add_signal_btn, add_signal_cb, LV_EVENT_CLICKED, NULL);
                lv_obj_set_user_data(add_signal_btn, (void *)(intptr_t)(signal_count + 1));
                
                lv_obj_t *delete_btn = lv_list_add_btn(list, NULL, "Delete Remote");
                lv_obj_set_width(delete_btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(delete_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(delete_btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(delete_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *delete_label = lv_obj_get_child(delete_btn, 0);
                if (delete_label) {
                    lv_obj_set_style_text_font(delete_label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(delete_label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(delete_btn, delete_remote_cb, LV_EVENT_CLICKED, NULL);
                lv_obj_set_user_data(delete_btn, (void *)(intptr_t)(signal_count + 2));
                
                num_ir_items = signal_count + 3; // signals + 3 management options
                
                ESP_LOGI(TAG, "Reloaded %zu signals for remote after adding new signal (regular mode)", signal_count);
                return; // Stay in current view, don't switch
            }
        }
    }
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    // Return to infrared view
    display_manager_switch_view(&infrared_view);
}
#endif

#ifdef CONFIG_HAS_INFRARED_RX
// Function to append a learned signal to an existing remote file
static void append_signal_to_remote(const char *signal_name) {
    ESP_LOGI(TAG, "append_signal_to_remote called with name: %s", signal_name ? signal_name : "NULL");
    ESP_LOGI(TAG, "Current remote path: %s", current_remote_path);
    ESP_LOGI(TAG, "Signal data: timings=%p, size=%d, is_raw=%d", 
             learned_signal.payload.raw.timings, 
             learned_signal.payload.raw.timings_size,
             learned_signal.is_raw);
    
    if (!signal_name || strlen(signal_name) == 0) {
        ESP_LOGE(TAG, "Invalid signal name provided");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    if (strlen(current_remote_path) == 0) {
        ESP_LOGE(TAG, "No current remote file selected");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    // Check if we have valid signal data
    if (!learned_signal.is_raw) {
        ESP_LOGE(TAG, "No valid signal data to save");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    if (!learned_signal.payload.raw.timings || learned_signal.payload.raw.timings_size == 0) {
        ESP_LOGE(TAG, "No timing data in signal");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    // Additional safety check: validate timing data pointer
    if ((uintptr_t)learned_signal.payload.raw.timings < 0x3C000000 || 
        (uintptr_t)learned_signal.payload.raw.timings > 0x3FFFFFFF) {
        ESP_LOGE(TAG, "Invalid timing data pointer: %p", learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
        return;
    }
    
    // Add a small delay to ensure any pending operations are complete
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "Appending signal to remote file: %s", current_remote_path);
    
    // Open the existing remote file in append mode
    FILE *f = fopen(current_remote_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open remote file for appending: %s, errno: %d", current_remote_path, errno);
        // Free timing data to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    ESP_LOGI(TAG, "File opened successfully, appending signal...");
    
    // Append the new signal to the file
    fprintf(f, "#\n");
    fprintf(f, "name: %s\n", signal_name);
    
    // Check if we have a decoded signal to save in proper format
    if (signal_decoded && decoded_message) {
        ESP_LOGI(TAG, "Appending decoded signal: %s, addr=0x%08lX, cmd=0x%08lX", 
                infrared_protocol_to_string(decoded_message->protocol),
                decoded_message->address, decoded_message->command);
        
        // Save as decoded signal
        fprintf(f, "type: parsed\n");
        fprintf(f, "protocol: %s\n", infrared_protocol_to_string(decoded_message->protocol));
        fprintf(f, "address: %02lX %02lX %02lX %02lX\n", 
                (decoded_message->address >> 0) & 0xFF,
                (decoded_message->address >> 8) & 0xFF, 
                (decoded_message->address >> 16) & 0xFF,
                (decoded_message->address >> 24) & 0xFF);
        fprintf(f, "command: %02lX %02lX %02lX %02lX\n",
                (decoded_message->command >> 0) & 0xFF,
                (decoded_message->command >> 8) & 0xFF,
                (decoded_message->command >> 16) & 0xFF, 
                (decoded_message->command >> 24) & 0xFF);
    } else {
        ESP_LOGI(TAG, "Appending as raw signal (could not decode)");
        
        // Save as raw signal
        fprintf(f, "type: raw\n");
        fprintf(f, "frequency: %lu\n", learned_signal.payload.raw.frequency);
        fprintf(f, "duty_cycle: %.6f\n", learned_signal.payload.raw.duty_cycle);
        fprintf(f, "data: ");
        
        // Write timing data
        for (size_t i = 0; i < learned_signal.payload.raw.timings_size; i++) {
            fprintf(f, "%lu ", learned_signal.payload.raw.timings[i]);
        }
        fprintf(f, "\n");
    }
    
    fclose(f);
    
    ESP_LOGI(TAG, "Signal '%s' appended to remote file %s", signal_name, current_remote_path);
    
    // Free timing data to prevent memory leaks
    if (learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
    }
}
#endif

#ifdef CONFIG_USE_ENCODER
static const char *IR_BACK_OPTION_MAGIC_STR = "__IR_BACK_OPTION__"; // Unique string for the back button
#endif

typedef struct {
    char path[256];
    char command[32];
} UniversalTransmitArgs_t;
static void ir_select_item(int index);

// touchscreen controls
#ifdef CONFIG_USE_TOUCHSCREEN
#define IR_SCROLL_BTN_SIZE 40
#define IR_SCROLL_BTN_PADDING 5
static lv_obj_t *ir_scroll_up_btn = NULL;
static lv_obj_t *ir_scroll_down_btn = NULL;
static lv_obj_t *ir_back_btn = NULL;
// scroll callbacks
static void file_scroll_up_cb(lv_event_t *e) { ir_select_item(selected_ir_index - 1); }
static void file_scroll_down_cb(lv_event_t *e) { ir_select_item(selected_ir_index + 1); }
#endif

// add job struct and queue/task for universals
typedef struct {
    char path[256];
    char command[32];
} UniversalJob_t;
static QueueHandle_t universals_queue = NULL;
static TaskHandle_t universals_task_handle = NULL;

// forward declarations
static void back_event_cb(lv_event_t *e);
static void file_event_open(int idx);
static void command_event_execute(int idx);
static void file_event_cb(lv_event_t *e);
static void command_event_cb(lv_event_t *e);
static void remotes_event_cb(lv_event_t *e);
static void universals_event_cb(lv_event_t *e);
#ifdef CONFIG_HAS_INFRARED_RX
static void learn_remote_event_cb(lv_event_t *e);
#endif
#ifdef CONFIG_USE_ENCODER
static void add_encoder_back_btn(void);
#endif

static void cleanup_transmit_popup(void *obj);
static void cleanup_learning_popup(void *obj);
static void universal_transmit_task(void *arg);

static void cleanup_transmit_popup(void *obj) {
    if (transmitting_popup) {
        lv_obj_del(transmitting_popup);
        transmitting_popup = NULL;
    }
}

static void universal_transmit_task(void *arg) {
    UniversalTransmitArgs_t *args = (UniversalTransmitArgs_t *)arg;
    char path[256];
    char command[32];
    strncpy(path, args->path, sizeof(path) -1);
    path[sizeof(path) - 1] = '\0';
    strncpy(command, args->command, sizeof(command) -1);
    command[sizeof(command) - 1] = '\0';
    free(args);

    printf("universal_transmit_task: start %s -> %s\n", path, command);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("universal_transmit_task: fopen failed for %s\n", path);
        lv_async_call(cleanup_transmit_popup, NULL);
        universal_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    char buf[256];
    infrared_signal_t sig;
    bool in_block = false, block_valid = false;
    universal_transmit_cancel = false;

    while (fgets(buf, sizeof(buf), f)) {
        if (universal_transmit_cancel) break;
        char *s = buf; while (*s && isspace((unsigned char)*s)) s++;
        if (*s=='#'||*s=='\0') continue;
        if (strncmp(s, "name:", 5)==0) {
            if (in_block&&block_valid) {
                infrared_manager_transmit(&sig);
                infrared_manager_free_signal(&sig);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            char *v = s+5; while (*v && isspace((unsigned char)*v)) v++;
            char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
            if (strcmp(v, command)==0) {
                in_block=true; block_valid=false; memset(&sig,0,sizeof(sig));
                strncpy(sig.name, v, sizeof(sig.name)-1);
            } else {
                in_block=false;
            }
        } else if (in_block) {
            if (strncmp(s, "type:",5)==0) {
                char *v=s+5; while(*v&&isspace((unsigned char)*v))v++;
                char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
                sig.is_raw = (strncmp(v,"raw",3)==0);
                block_valid = true;
            } else if (sig.is_raw) {
                if (strncmp(s, "frequency:",10)==0) sig.payload.raw.frequency = strtoul(s+10,NULL,10);
                else if (strncmp(s, "duty_cycle:",11)==0) sig.payload.raw.duty_cycle = strtof(s+11,NULL);
                else if (strncmp(s, "data:",5)==0) {
                    char *p=s+5; size_t cnt=0; char *t=p;
                    while(*t){while(*t&&isspace((unsigned char)*t))t++;if(!*t)break;cnt++;while(*t&&!isspace((unsigned char)*t))t++;}
                    uint32_t *arr=malloc(cnt*sizeof(uint32_t)); size_t ii=0; char *endp;
                    while(*p){while(*p&&isspace((unsigned char)*p))p++;if(!*p)break;arr[ii++]=strtoul(p,&endp,10);p=endp;}
                    sig.payload.raw.timings=arr; sig.payload.raw.timings_size=cnt;
                }
            } else {
                if (strncmp(s, "protocol:",9)==0) {
                    char *v=s+9; 
                    while(*v && isspace((unsigned char)*v)) v++;
                    char *e = v + strlen(v) - 1;
                    while(e > v && isspace((unsigned char)*e)) *e-- = '\0';
                    strncpy(sig.payload.message.protocol, v, sizeof(sig.payload.message.protocol)-1);
                    sig.payload.message.protocol[sizeof(sig.payload.message.protocol)-1] = '\0';
                    block_valid=true;
                } else if (strncmp(s, "address:",8)==0) {
                    char* p = s + 8;
                    uint32_t addr = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        addr |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.address = addr;
                } else if (strncmp(s, "command:",8)==0) {
                    char* p = s + 8;
                    uint32_t cmd = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        cmd |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.command = cmd;
                }
            }
        }
    }
    if (!universal_transmit_cancel && in_block&&block_valid) {
        infrared_manager_transmit(&sig);
        infrared_manager_free_signal(&sig);
    }
    fclose(f);
    printf("universal_transmit_task: finished processing %s\n", path);
    lv_async_call(cleanup_transmit_popup, NULL);
    universal_task_handle = NULL;
    vTaskDelete(NULL);
}

static void back_event_cb(lv_event_t *e) {
    if (showing_commands) {
        // if currently showing commands, return to file list
        showing_commands = false;

        // free command-related resources
        if (!in_universals_mode && signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if (in_universals_mode && uni_command_names) {
            for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
            free(uni_command_names);
            uni_command_names = NULL;
            uni_command_count = 0;
        }

        // rebuild file list
        lv_obj_clean(list);
        num_ir_items = ir_file_count;
        selected_ir_index = 0;
        for (size_t i = 0; i < ir_file_count; i++) {
            lv_obj_t *btn = lv_list_add_btn(list, NULL, ir_file_paths[i]);
            lv_obj_set_width(btn, LV_HOR_RES);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *label = lv_obj_get_child(btn, 0);
            if (label) {
                lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_add_event_cb(btn, file_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        }
#ifdef CONFIG_USE_ENCODER
        add_encoder_back_btn();
#endif
        if (num_ir_items > 0) ir_select_item(0);
        return;
    }

    // if we are in a file list (remotes or universals) but not at top-level, go back to top-level menu
    if (!has_remotes_option || !has_universals_option) {
        // cancel any ongoing universal transmission
        if (universal_task_handle) {
            universal_transmit_cancel = true;
        }

        // free resources from file list level
        if (signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if (ir_file_paths) {
            for (size_t i = 0; i < ir_file_count; i++) free(ir_file_paths[i]);
            free(ir_file_paths);
            ir_file_paths = NULL;
            ir_file_count = 0;
        }
        if (uni_command_names) {
            for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
            free(uni_command_names);
            uni_command_names = NULL;
            uni_command_count = 0;
        }

        in_universals_mode = false;
        has_remotes_option = true;
        has_universals_option = true;
        strcpy(current_dir, "/mnt/ghostesp");

        // rebuild the top-level list
        lv_obj_clean(list);
        lv_obj_t *remotes_btn = lv_list_add_btn(list, NULL, "Remotes");
        lv_obj_set_width(remotes_btn, LV_HOR_RES);
        lv_obj_set_style_bg_color(remotes_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(remotes_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(remotes_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(remotes_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *rem_label = lv_obj_get_child(remotes_btn, 0);
        if (rem_label) {
            lv_obj_set_style_text_font(rem_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(rem_label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(remotes_btn, remotes_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *universals_btn = lv_list_add_btn(list, NULL, "Universals");
        lv_obj_set_width(universals_btn, LV_HOR_RES);
        lv_obj_set_style_bg_color(universals_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(universals_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(universals_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(universals_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *uni_label = lv_obj_get_child(universals_btn, 0);
        if (uni_label) {
            lv_obj_set_style_text_font(uni_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(uni_label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(universals_btn, universals_event_cb, LV_EVENT_CLICKED, NULL);

#ifdef CONFIG_HAS_INFRARED_RX
        // add learn remote option
        lv_obj_t *learn_btn = lv_list_add_btn(list, NULL, "Learn Remote");
        lv_obj_set_width(learn_btn, LV_HOR_RES);
        lv_obj_set_style_bg_color(learn_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(learn_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(learn_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(learn_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *learn_label = lv_obj_get_child(learn_btn, 0);
        if (learn_label) {
            lv_obj_set_style_text_font(learn_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(learn_label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(learn_btn, learn_remote_event_cb, LV_EVENT_CLICKED, NULL);
        
        // Add Easy Learn toggle option
        is_easy_mode = settings_get_infrared_easy_mode(&G_Settings);
        lv_obj_t *easy_learn_btn = lv_list_add_btn(list, NULL, is_easy_mode ? "Easy Learn [X]" : "Easy Learn [ ]");
        lv_obj_set_width(easy_learn_btn, LV_HOR_RES);
        lv_obj_set_style_bg_color(easy_learn_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(easy_learn_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(easy_learn_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(easy_learn_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *easy_learn_label = lv_obj_get_child(easy_learn_btn, 0);
        if (easy_learn_label) {
            lv_obj_set_style_text_font(easy_learn_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(easy_learn_label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(easy_learn_btn, easy_learn_toggle_cb, LV_EVENT_CLICKED, NULL);
#endif

        num_ir_items = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0);
#ifdef CONFIG_HAS_INFRARED_RX
        num_ir_items++; // account for learn remote button
        num_ir_items++; // account for easy learn button
#endif
#ifdef CONFIG_USE_ENCODER
        add_encoder_back_btn();
#endif
        selected_ir_index = 0;
        if (num_ir_items > 0) ir_select_item(0);
        return;
    }

    // default: leave view
    display_manager_switch_view(&main_menu_view);
}

void infrared_view_create(void) {
    // Initialize infrared settings
#ifdef CONFIG_HAS_INFRARED_RX
    is_easy_mode = settings_get_infrared_easy_mode(&G_Settings);
#endif
    
    root = lv_obj_create(lv_scr_act());
    lv_obj_set_style_pad_all(root, 0, 0);
    infrared_view.root = root;
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);

    display_manager_add_status_bar("Infrared");

    const int STATUS_BAR_HEIGHT = 20;
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = IR_SCROLL_BTN_SIZE + IR_SCROLL_BTN_PADDING * 2;
#else
    const int BUTTON_AREA_HEIGHT = 0;
#endif
    int list_h = LV_VER_RES - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    list = lv_list_create(root);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 0, LV_PART_MAIN);
    lv_obj_set_size(list, LV_HOR_RES, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);

    // add remotes option
    has_remotes_option = true;
    lv_obj_t *remotes_btn = lv_list_add_btn(list, NULL, "Remotes");
    lv_obj_set_width(remotes_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(remotes_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(remotes_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(remotes_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(remotes_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *rem_label = lv_obj_get_child(remotes_btn, 0);
    if (rem_label) {
        lv_obj_set_style_text_font(rem_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rem_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(remotes_btn, remotes_event_cb, LV_EVENT_CLICKED, NULL);

    // add universals option
    has_universals_option = true;
    lv_obj_t *universals_btn = lv_list_add_btn(list, NULL, "Universals");
    lv_obj_set_width(universals_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(universals_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(universals_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(universals_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(universals_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *uni_label = lv_obj_get_child(universals_btn, 0);
    if (uni_label) {
        lv_obj_set_style_text_font(uni_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(uni_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(universals_btn, universals_event_cb, LV_EVENT_CLICKED, NULL);

#ifdef CONFIG_HAS_INFRARED_RX
    // Initialize RMT RX channel for IR learning (do this once per view)
    ESP_LOGI(TAG, "Initializing RMT RX channel for infrared learning");
    
    // Initialize GPIO for IR RX
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << CONFIG_INFRARED_RX_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    
    // Initialize RMT RX channel
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = CONFIG_INFRARED_RX_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, // 1MHz resolution
        .mem_block_symbols = 128, // Larger buffer
        .intr_priority = 0, // Let driver choose priority
        .flags = {
            .invert_in = 0,
            .with_dma = 0,
            .io_loop_back = 0,
            .allow_pd = 0,
        },
    };
    
    // Ensure any existing channel is cleaned up first
    if (rx_channel != NULL) {
        rmt_disable(rx_channel);
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
    }
    
    if (rmt_new_rx_channel(&rx_config, &rx_channel) == ESP_OK) {
        // Create queue for RX data communication
        if (ir_rx_queue) {
            vQueueDelete(ir_rx_queue);
        }
        ir_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
        
        if (ir_rx_queue) {
            // Register RX callback
            rmt_rx_event_callbacks_t cbs = {
                .on_recv_done = ir_rx_done_callback,
            };
            
            if (rmt_rx_register_event_callbacks(rx_channel, &cbs, ir_rx_queue) == ESP_OK) {
                // Enable the RMT channel
                if (rmt_enable(rx_channel) == ESP_OK) {
                    ESP_LOGI(TAG, "RMT RX channel initialized successfully");
                } else {
                    ESP_LOGE(TAG, "Failed to enable RMT RX channel");
                    rmt_del_channel(rx_channel);
                    rx_channel = NULL;
                    vQueueDelete(ir_rx_queue);
                    ir_rx_queue = NULL;
                }
            } else {
                ESP_LOGE(TAG, "Failed to register RMT RX callbacks");
                rmt_del_channel(rx_channel);
                rx_channel = NULL;
                vQueueDelete(ir_rx_queue);
                ir_rx_queue = NULL;
            }
        } else {
            ESP_LOGE(TAG, "Failed to create RX queue");
            rmt_del_channel(rx_channel);
            rx_channel = NULL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to create RMT RX channel");
        rx_channel = NULL;
    }
    
    // add learn remote option
    lv_obj_t *learn_btn = lv_list_add_btn(list, NULL, "Learn Remote");
    lv_obj_set_width(learn_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(learn_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(learn_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(learn_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(learn_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *learn_label = lv_obj_get_child(learn_btn, 0);
    if (learn_label) {
        lv_obj_set_style_text_font(learn_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(learn_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(learn_btn, learn_remote_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Add Easy Learn toggle option
    lv_obj_t *easy_learn_btn = lv_list_add_btn(list, NULL, is_easy_mode ? "Easy Learn [X]" : "Easy Learn [ ]");
    lv_obj_set_width(easy_learn_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(easy_learn_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(easy_learn_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(easy_learn_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(easy_learn_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *easy_learn_label = lv_obj_get_child(easy_learn_btn, 0);
    if (easy_learn_label) {
        lv_obj_set_style_text_font(easy_learn_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(easy_learn_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(easy_learn_btn, easy_learn_toggle_cb, LV_EVENT_CLICKED, NULL);
#endif

#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif

    // set num_ir_items after all buttons are added (including back button)
    num_ir_items = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0);
#ifdef CONFIG_HAS_INFRARED_RX
    num_ir_items++; // account for learn remote button
    num_ir_items++; // account for easy learn button
#endif
#ifdef CONFIG_USE_ENCODER
    num_ir_items++; // account for encoder back button
#endif
    selected_ir_index = 0;
    if (num_ir_items > 0) ir_select_item(0);

    // Back button
    // touchscreen-only controls
    #ifdef CONFIG_USE_TOUCHSCREEN
    ir_scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(ir_scroll_up_btn, IR_SCROLL_BTN_SIZE, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, IR_SCROLL_BTN_PADDING, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_scroll_up_btn, file_scroll_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(ir_scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);

    ir_scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(ir_scroll_down_btn, IR_SCROLL_BTN_SIZE, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -IR_SCROLL_BTN_PADDING, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_scroll_down_btn, file_scroll_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(ir_scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);

    ir_back_btn = lv_btn_create(root);
    lv_obj_set_size(ir_back_btn, IR_SCROLL_BTN_SIZE + 20, IR_SCROLL_BTN_SIZE);
    lv_obj_align(ir_back_btn, LV_ALIGN_BOTTOM_MID, 0, -IR_SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(ir_back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(ir_back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ir_back_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(ir_back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(ir_back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
    #endif
}

void infrared_view_destroy(void) {
    if (universal_task_handle) {
        universal_transmit_cancel = true;
        vTaskDelete(universal_task_handle);
        universal_task_handle = NULL;
    }
    cleanup_transmit_popup(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
    // Cleanup IR learning resources
    if (ir_learning_task_handle) {
        ir_learning_cancel = true;
        // Just set the flag and let the task clean itself up
        // vTaskDelete should be avoided if possible
        ir_learning_task_handle = NULL;
    }
    cleanup_learning_popup(NULL);
    cleanup_signal_preview_popup(NULL);
    
    // Clean up decoder context
    if (decoder_context) {
        infrared_decoder_free(decoder_context);
        decoder_context = NULL;
    }
    signal_decoded = false;
    decoded_message = NULL;
    
    // Only free learned signal data if it's not being preserved for the callback
    // If preserve_learned_signal is true, it means we're switching to keyboard view
    // and the timing data should be preserved for the callback
    if (!preserve_learned_signal && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
    }
    
    // Clean up RMT RX resources
    if (rx_channel) {
        rmt_disable(rx_channel);
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
        ESP_LOGI(TAG, "RMT RX channel cleaned up");
    }
    
    // Clean up RX queue
    if (ir_rx_queue) {
        vQueueDelete(ir_rx_queue);
        ir_rx_queue = NULL;
        ESP_LOGI(TAG, "RX queue cleaned up");
    }
#endif
    if(root) {
        if(signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if(ir_file_paths) {
            for(size_t i = 0; i < ir_file_count; i++) {
                free(ir_file_paths[i]);
            }
            free(ir_file_paths);
            ir_file_paths = NULL;
            ir_file_count = 0;
        }
        showing_commands = false;
        lv_obj_del(root);
        root = NULL;
        list = NULL;
        infrared_view.root = NULL;
        selected_ir_index = 0;
        num_ir_items = 0;
    }
}

static void ir_select_item(int index) {
    if(num_ir_items == 0) return;
    if(index < 0) index = num_ir_items - 1;
    if(index >= num_ir_items) index = 0;
    
    // clear previous selection
    lv_obj_t *prev = lv_obj_get_child(list, selected_ir_index);
    if(prev) {
        // Check if this is one of the management buttons that have special styling
        if (showing_commands && selected_ir_index >= signal_count) {
            // This is a management button, restore its special styling
            if (selected_ir_index == signal_count) {
                // Rename button
                lv_obj_set_style_bg_color(prev, lv_color_hex(0x2E2E2E), LV_PART_MAIN);
            } else if (selected_ir_index == signal_count + 1) {
                // Add Signal button
                lv_obj_set_style_bg_color(prev, lv_color_hex(0x2E2E2E), LV_PART_MAIN);
            } else if (selected_ir_index == signal_count + 2) {
                // Delete button - keep the red color
                lv_obj_set_style_bg_color(prev, lv_color_hex(0x8B0000), LV_PART_MAIN);
            } else {
                // Regular command button
                lv_obj_set_style_bg_color(prev, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
            }
        } else {
            // Regular command button
            lv_obj_set_style_bg_color(prev, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
        }
    }
    
    selected_ir_index = index;
    lv_obj_t *cur = lv_obj_get_child(list, selected_ir_index);
    if(cur) {
        // If the currently selected item is the Delete Remote management option,
        // highlight it with a lighter red instead of the default gray.
        if (showing_commands && selected_ir_index >= signal_count && selected_ir_index == signal_count + 2) {
            lv_obj_set_style_bg_color(cur, lv_color_hex(0xB22222), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(cur, lv_color_hex(0x555555), LV_PART_MAIN);
        }
        lv_obj_scroll_to_view(cur, LV_ANIM_OFF);
    }
}

void infrared_view_input_cb(InputEvent *event) {
#ifdef CONFIG_HAS_INFRARED_RX
    // Handle learn remote popup input
    if (learning_popup && lv_obj_is_valid(learning_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on cancel button
                if (learning_cancel_btn && lv_obj_is_valid(learning_cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(learning_cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        learning_cancel_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            // Only one button: treat left/right/tab as select
            if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_PRESS) ||
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                learning_cancel_cb(NULL);
                return;
            } else if (
                (event->type == INPUT_TYPE_JOYSTICK && (event->data.joystick_index == JOYSTICK_LEFT || event->data.joystick_index == JOYSTICK_RIGHT)) ||
                (event->type == INPUT_TYPE_ENCODER && (event->data.encoder.direction == ENCODER_LEFT || event->data.encoder.direction == ENCODER_RIGHT)) ||
                (event->type == INPUT_TYPE_KEYBOARD && event->data.key_value == 9)) // Tab
            {
                // Toggle selection state (only one button, so stays selected)
                preview_selected_option = 1;
                update_learning_popup_selection();
            } else if (event->type == INPUT_TYPE_KEYBOARD) {
                // Handle Cardputer keyboard input for cancel
                if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                    event->data.key_value == 27 || // ESC key
                    event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                    learning_cancel_cb(NULL);
                    return;
                }
            }
        }
        return; // Don't process other input when learning popup is active
    }
    
    // Handle Easy Learn popup input
    if (easy_learn_popup && lv_obj_is_valid(easy_learn_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on cancel button
                if (easy_learn_cancel_btn && lv_obj_is_valid(easy_learn_cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(easy_learn_cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        easy_learn_cancel_cb(NULL);
                        return;
                    }
                }
                // Handle touch on skip button
                if (easy_learn_skip_btn && lv_obj_is_valid(easy_learn_skip_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(easy_learn_skip_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        easy_learn_skip_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            // Handle joystick/encoder navigation
            if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_LEFT) || 
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_LEFT)) {
                easy_learn_selected_option = 0; // Cancel
                update_easy_learn_popup_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_RIGHT) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_RIGHT)) {
                easy_learn_selected_option = 1; // Skip
                update_easy_learn_popup_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_PRESS) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                if (easy_learn_selected_option == 0) {
                    easy_learn_cancel_cb(NULL);
                } else {
                    easy_learn_skip_cb(NULL);
                }
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            // Handle Cardputer keyboard input
            if (event->data.key_value == 's' || event->data.key_value == 'S') {
                easy_learn_skip_cb(NULL);
            } else if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                      event->data.key_value == 27) { // ESC key
                easy_learn_cancel_cb(NULL);
            } else if (event->data.key_value == 9) { // Tab key
                easy_learn_selected_option = (easy_learn_selected_option + 1) % 2;
                update_easy_learn_popup_selection();
            } else if (event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                if (easy_learn_selected_option == 0) {
                    easy_learn_cancel_cb(NULL);
                } else {
                    easy_learn_skip_cb(NULL);
                }
            }
        }
        return; // Don't process other input when easy learn popup is active
    }
    
    // Handle signal preview popup input
    if (signal_preview_popup && lv_obj_is_valid(signal_preview_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on save/cancel buttons
                if (save_btn && lv_obj_is_valid(save_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(save_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        signal_preview_save_cb(NULL);
                        return;
                    }
                }
                if (cancel_btn && lv_obj_is_valid(cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        signal_preview_cancel_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            // Handle joystick/encoder navigation
            if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_LEFT) || 
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_LEFT)) {
                preview_selected_option = 0; // Save
                update_signal_preview_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_RIGHT) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_RIGHT)) {
                preview_selected_option = 1; // Cancel
                update_signal_preview_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == JOYSTICK_PRESS) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                if (preview_selected_option == 0) {
                    signal_preview_save_cb(NULL);
                } else {
                    signal_preview_cancel_cb(NULL);
                }
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            // Handle Cardputer keyboard input
            if (event->data.key_value == 's' || event->data.key_value == 'S') {
                signal_preview_save_cb(NULL);
            } else if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                      event->data.key_value == 27) { // ESC key
                signal_preview_cancel_cb(NULL);
            } else if (event->data.key_value == 9) { // Tab key
                preview_selected_option = (preview_selected_option + 1) % 2;
                update_signal_preview_selection();
            } else if (event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                if (preview_selected_option == 0) {
                    signal_preview_save_cb(NULL);
                } else {
                    signal_preview_cancel_cb(NULL);
                }
            }
        }
        return; // Don't process other input when preview is active
    }
#endif
    
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        
        if (data->state == LV_INDEV_STATE_PR) {
            #ifdef CONFIG_USE_TOUCHSCREEN
            if (ir_scroll_up_btn && lv_obj_is_valid(ir_scroll_up_btn)) {
                lv_area_t area;
                lv_obj_get_coords(ir_scroll_up_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    ir_select_item(selected_ir_index - 1);
                    return;
                }
            }
            
            if (ir_scroll_down_btn && lv_obj_is_valid(ir_scroll_down_btn)) {
                lv_area_t area;
                lv_obj_get_coords(ir_scroll_down_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    ir_select_item(selected_ir_index + 1);
                    return;
                }
            }
            
            if (ir_back_btn && lv_obj_is_valid(ir_back_btn)) {
                lv_area_t area;
                lv_obj_get_coords(ir_back_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    back_event_cb(NULL);
                    return;
                }
            }
            #endif
            
            for (int i = 0; i < num_ir_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(list, i);
                if (btn) {
                    lv_area_t btn_area;
                    lv_obj_get_coords(btn, &btn_area);
                    if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                        data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                        ir_select_item(i);
                        
                        if (!showing_commands) {
                            if (has_remotes_option && i == 0) {
                                remotes_event_cb(NULL);
                            } else if (has_universals_option && i == (has_remotes_option ? 1 : 0)) {
                                universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                            } else if (i == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                                learn_remote_event_cb(NULL);
#endif
                            } else {
                                int file_idx = i - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                                    + 1
#endif
                                );
                                file_event_open(file_idx);
                            }
                        } else {
                            command_event_execute(i);
                        }
                        return;
                    }
                }
            }
        }
    } else if(event->type == INPUT_TYPE_JOYSTICK) {
        uint8_t idx = event->data.joystick_index;
        if(idx == 0) {
            ESP_LOGI(TAG, "joystick left pressed, going back");
            back_event_cb(NULL);
        } else if(idx == 2) {
            ESP_LOGI(TAG, "joystick up pressed, selecting previous item");
            ir_select_item(selected_ir_index - 1);
        } else if(idx == 4) {
            ESP_LOGI(TAG, "joystick down pressed, selecting next item");
            ir_select_item(selected_ir_index + 1);
        } else if(idx == 1) {
#ifdef CONFIG_USE_ENCODER
            // Check if the selected item is the Back option
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            if (selected_obj && lv_obj_get_user_data(selected_obj) == IR_BACK_OPTION_MAGIC_STR) {
                ESP_LOGI(TAG, "Joystick Enter pressed on Back option");
                back_event_cb(NULL);
                return;
            }
#endif
            if (!showing_commands) {
                if (has_remotes_option && selected_ir_index == 0) {
                    ESP_LOGI(TAG, "joystick enter pressed on Remotes, opening remotes directory");
                    remotes_event_cb(NULL);
                } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                    ESP_LOGI(TAG, "joystick enter pressed on Universals, opening universals directory");
                    universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                    ESP_LOGI(TAG, "joystick enter pressed on Learn Remote, starting IR learning");
                    learn_remote_event_cb(NULL);
#endif
                } else {
                    int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                        + 1
#endif
                    );
                    ESP_LOGI(TAG, "joystick enter pressed, opening selected file at index %d", file_idx);
                    file_event_open(file_idx);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                ESP_LOGI(TAG, "joystick enter pressed, executing selected command at index %d", selected_ir_index);
                command_event_execute(selected_ir_index);
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) {
            ESP_LOGI(TAG, "Keyboard Left/Up button pressed\n");
            ir_select_item(selected_ir_index - 1);
        } else if ((keyValue == 47 || keyValue == '/') || (keyValue == 46 || keyValue == '.')) {
            ESP_LOGI(TAG, "Keyboard Right/Down button pressed\n");
            ir_select_item(selected_ir_index + 1);
        } else if (keyValue == 13) {
            ESP_LOGI(TAG, "Keyboard Enter button pressed\n");
#ifdef CONFIG_USE_ENCODER
            // Check if the selected item is the Back option
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            if (selected_obj && lv_obj_get_user_data(selected_obj) == IR_BACK_OPTION_MAGIC_STR) {
                ESP_LOGI(TAG, "Keyboard Enter pressed on Back option");
                back_event_cb(NULL);
                return;
            }
#endif
            if (!showing_commands) {
                // Check if we're in the top-level menu or in a file list
                if (has_remotes_option || has_universals_option) {
                    // Top-level menu logic
                    if (has_remotes_option && selected_ir_index == 0) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Remotes, opening remotes directory");
                        remotes_event_cb(NULL);
                    } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Universals, opening universals directory");
                        universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Learn Remote");
                        learn_remote_event_cb(NULL);
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Easy Learn");
                        easy_learn_toggle_cb(NULL);
#endif
                    } else {
                        int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                            + 2  // Account for both Learn Remote and Easy Learn buttons
#endif
                        );
                        ESP_LOGI(TAG, "Keyboard Enter pressed, opening selected file at index %d", file_idx);
                        file_event_open(file_idx);
                    }
                } else {
                    // File list mode - direct file selection
                    ESP_LOGI(TAG, "Keyboard Enter pressed, opening selected file at index %d", selected_ir_index);
                    file_event_open(selected_ir_index);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                ESP_LOGI(TAG, "Keyboard Enter pressed, executing selected command at index %d", selected_ir_index);
                command_event_execute(selected_ir_index);
            }
        } else if (keyValue == 29 || keyValue == '`') {
            ESP_LOGI(TAG, "Keyboard Esc button pressed\n");
            back_event_cb(NULL);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            // Check if the selected item is the Back option
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            if (selected_obj && lv_obj_get_user_data(selected_obj) == IR_BACK_OPTION_MAGIC_STR) {
                ESP_LOGI(TAG, "Encoder button pressed on Back option");
                back_event_cb(NULL);
                return;
            }
            if (!showing_commands) {
                // Check if we're in the top-level menu or in a file list
                if (has_remotes_option || has_universals_option) {
                    // Top-level menu logic
                    if (has_remotes_option && selected_ir_index == 0) {
                        remotes_event_cb(NULL);
                    } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                        universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        learn_remote_event_cb(NULL);
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                        easy_learn_toggle_cb(NULL);
#endif
                    } else {
                        int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                            + 2  // Account for both Learn Remote and Easy Learn buttons
#endif
                        );
                        file_event_open(file_idx);
                    }
                } else {
                    // File list mode - direct file selection
                    file_event_open(selected_ir_index);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                command_event_execute(selected_ir_index);
            }
        } else {
            if (event->data.encoder.direction > 0) {
                ir_select_item(selected_ir_index + 1);
            } else {
                ir_select_item(selected_ir_index - 1);
            }
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

// open selected IR file and list commands
static void file_event_open(int idx) {
    if (in_universals_mode) {
        // clear previous unique names
        for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
        free(uni_command_names);
        uni_command_names = NULL;
        uni_command_count = 0;
        // build full file path
        char path[256];
        size_t base_len = strlen(current_dir);
        if (base_len >= sizeof(path) - 1) base_len = sizeof(path) - 1;
        memcpy(path, current_dir, base_len);
        path[base_len] = '\0';
        if (base_len + 1 < sizeof(path)) strcat(path, "/");
        strcat(path, ir_file_paths[idx]);
        // remember for transmit
        strncpy(current_universal_file, path, sizeof(current_universal_file) - 1);
        current_universal_file[sizeof(current_universal_file) - 1] = '\0';
        printf("scanning universal file: %s\n", current_universal_file);
        // scan file for unique command names
        FILE *f = fopen(path, "r"); if (!f) return;
        char buf[256], last[256] = "";
        while (fgets(buf, sizeof(buf), f)) {
            char *s = buf;
            while (*s && isspace((unsigned char)*s)) s++;
            if (*s == '#' || *s == '\0') continue;
            if (strncmp(s, "name:", 5) == 0) {
                char *v = s + 5; while (*v && isspace((unsigned char)*v)) v++;
                char *e = v + strlen(v) - 1; while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
                if (strcmp(v, last) != 0) {
                    bool dup = false;
                    for (size_t j = 0; j < uni_command_count; j++) {
                        if (strcmp(uni_command_names[j], v) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        uni_command_names = realloc(uni_command_names, (uni_command_count + 1) * sizeof(*uni_command_names));
                        uni_command_names[uni_command_count] = strdup(v);
                        uni_command_count++;
                    }
                    strcpy(last, v);
                }
            }
        }
        fclose(f);
        printf("found %zu unique commands\n", uni_command_count);
        // show unique commands
        lv_obj_clean(list);
        showing_commands = true;
        num_ir_items = uni_command_count;
        selected_ir_index = 0;
        for (size_t i = 0; i < uni_command_count; i++) {
            lv_obj_t *btn = lv_list_add_btn(list, NULL, uni_command_names[i]);
            lv_obj_set_width(btn, LV_HOR_RES);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *label = lv_obj_get_child(btn, 0);
            if (label) {
                lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_add_event_cb(btn, command_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        }
#ifdef CONFIG_USE_ENCODER
        add_encoder_back_btn();
#endif
        ir_select_item(0);
        return;
    }
    if (idx < 0 || idx >= ir_file_count) return;
    char path[256];
    size_t base_len = strlen(current_dir);
    if (base_len >= sizeof(path) - 1) base_len = sizeof(path) - 1;
    memcpy(path, current_dir, base_len);
    path[base_len] = '\0';
    if (base_len + 1 < sizeof(path)) {
        strncat(path, "/", sizeof(path) - strlen(path) - 1);
    }
    strncat(path, ir_file_paths[idx], sizeof(path) - strlen(path) - 1);

    ESP_LOGI(TAG, "opening IR file: %s", path);

    // Save current remote info for management functions
    strncpy(current_remote_path, path, sizeof(current_remote_path) - 1);
    current_remote_path[sizeof(current_remote_path) - 1] = '\0';
    
    // Extract remote name (without .ir extension)
    strncpy(current_remote_name, ir_file_paths[idx], sizeof(current_remote_name) - 1);
    current_remote_name[sizeof(current_remote_name) - 1] = '\0';
    char *dot = strrchr(current_remote_name, '.');
    if (dot) *dot = '\0';

    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    if (!infrared_manager_read_list(path, &signals, &signal_count)) {
        ESP_LOGE(TAG, "failed to read IR list from file: %s", path);
        return;
    }
    lv_obj_clean(list);
    showing_commands = true;
    num_ir_items = signal_count;
    selected_ir_index = 0;

    ESP_LOGI(TAG, "listing %zu commands for %s", signal_count, ir_file_paths[idx]);

    for (size_t i = 0; i < signal_count; i++) {
        const char *cmd_name = signals[i].name;
        lv_obj_t *btn = lv_list_add_btn(list, NULL, cmd_name);
        lv_obj_set_width(btn, LV_HOR_RES);
        // style button
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_add_event_cb(btn, command_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    
    // Add remote management options at the bottom
    lv_obj_t *rename_btn = lv_list_add_btn(list, NULL, "Rename Remote");
    lv_obj_set_width(rename_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(rename_btn, lv_color_hex(0x2E2E2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(rename_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(rename_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rename_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *rename_label = lv_obj_get_child(rename_btn, 0);
    if (rename_label) {
        lv_obj_set_style_text_font(rename_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rename_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(rename_btn, rename_remote_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *add_signal_btn = lv_list_add_btn(list, NULL, "Add New Signal");
    lv_obj_set_width(add_signal_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(add_signal_btn, lv_color_hex(0x2E2E2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(add_signal_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(add_signal_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(add_signal_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *add_signal_label = lv_obj_get_child(add_signal_btn, 0);
    if (add_signal_label) {
        lv_obj_set_style_text_font(add_signal_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(add_signal_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(add_signal_btn, add_signal_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *delete_btn = lv_list_add_btn(list, NULL, "Delete Remote");
    lv_obj_set_width(delete_btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0x8B0000), LV_PART_MAIN | LV_STATE_DEFAULT);  // Dark red
    lv_obj_set_style_border_width(delete_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(delete_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(delete_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *delete_label = lv_obj_get_child(delete_btn, 0);
    if (delete_label) {
        lv_obj_set_style_text_font(delete_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(delete_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(delete_btn, delete_remote_cb, LV_EVENT_CLICKED, NULL);
    
    // Update item count to include management options
    num_ir_items = signal_count + 3;  // 3 management options
    
#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif
    ir_select_item(0);
}

// execute selected IR command
static void command_event_execute(int idx) {
    if (in_universals_mode) {
        if (universal_task_handle) {
            printf("Universal transmission cancel requested.\n");
            universal_transmit_cancel = true;
            cleanup_transmit_popup(NULL);
            return;
        }
        if (idx < 0 || idx >= uni_command_count) return;

        transmitting_popup = lv_obj_create(lv_scr_act());
        lv_obj_set_size(transmitting_popup, 200, 60);
        lv_obj_center(transmitting_popup);
        lv_obj_set_style_bg_color(transmitting_popup, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(transmitting_popup, 2, 0);
        lv_obj_set_style_border_color(transmitting_popup, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(transmitting_popup, 5, 0);
        lv_obj_clear_flag(transmitting_popup, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(transmitting_popup);
        lv_label_set_text(label, "Transmitting...");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);

        UniversalTransmitArgs_t *args = malloc(sizeof(UniversalTransmitArgs_t));
        if (!args) {
            printf("Failed to allocate args for universal transmit task\n");
            cleanup_transmit_popup(NULL);
            return;
        }
        strncpy(args->path, current_universal_file, sizeof(args->path)-1);
        args->path[sizeof(args->path)-1] = '\0';
        strncpy(args->command, uni_command_names[idx], sizeof(args->command)-1);
        args->command[sizeof(args->command)-1] = '\0';
        xTaskCreate(universal_transmit_task, "uni_tx_task", 4096, args, tskIDLE_PRIORITY + 1, &universal_task_handle);
        printf("universals job task created\n");
        return;
    }
    if (idx < 0 || idx >= signal_count) return;
    ESP_LOGI(TAG, "transmitting command: %s", signals[idx].name);
    infrared_manager_transmit(&signals[idx]);
}

// LVGL event wrappers
static void file_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    file_event_open(idx);
}

static void command_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    
    // Check if this is one of our remote management options
    // These are added after all the IR commands, so they have indices:
    // signal_count = Rename Remote
    // signal_count + 1 = Add New Signal
    // signal_count + 2 = Delete Remote
    
    if (showing_commands && !in_universals_mode) {
        if (idx == signal_count) {
            // Rename Remote option
            rename_remote_cb(e);
            return;
        } else if (idx == signal_count + 1) {
            // Add New Signal option
            add_signal_cb(e);
            return;
        } else if (idx == signal_count + 2) {
            // Delete Remote option
            delete_remote_cb(e);
            return;
        }
    }
    
    // Otherwise, it's a regular command
    command_event_execute(idx);
}

static void remotes_event_cb(lv_event_t *e) {
    // exit any universals mode
    in_universals_mode = false;
    has_remotes_option = false;
    has_universals_option = false;
    strcpy(current_dir, "/mnt/ghostesp/infrared/remotes");
    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    if (ir_file_paths) {
        for (size_t i = 0; i < ir_file_count; i++) {
            free(ir_file_paths[i]);
        }
        free(ir_file_paths);
        ir_file_paths = NULL;
        ir_file_count = 0;
    }
    showing_commands = false;
    lv_obj_clean(list);
    DIR *d = opendir(current_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const char *name = entry->d_name;
            if (strstr(name, ".ir") || strstr(name, ".IR")) {
                ir_file_paths = realloc(ir_file_paths, (ir_file_count + 1) * sizeof(*ir_file_paths));
                ir_file_paths[ir_file_count] = strdup(name);
                lv_obj_t *btn = lv_list_add_btn(list, NULL, name);
                lv_obj_set_width(btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(btn, file_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)ir_file_count);
                ir_file_count++;
            }
        }
        closedir(d);
    }
    selected_ir_index = 0;
    num_ir_items = ir_file_count;
#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif
    if (ir_file_count > 0) ir_select_item(0);
}

// Remote management callback functions
void rename_remote_cb(lv_event_t *e) {
    // Switch to keyboard view to get new name
    keyboard_view_set_submit_callback(rename_remote_keyboard_callback);

    // Use current remote name as placeholder
    if (strlen(current_remote_name) > 0) {
        keyboard_view_set_placeholder(current_remote_name);
    } else {
        keyboard_view_set_placeholder("Enter remote name");
    }
    display_manager_switch_view(&keyboard_view);
}

#ifdef CONFIG_HAS_INFRARED_RX
typedef enum {
    LEARNING_POPUP_STANDARD,
    LEARNING_POPUP_EASY_LEARN
} learning_popup_type_t;

typedef struct {
    const char *title;
    const char *instruction;
    int width;
    int height;
    bool has_skip_button;
    lv_event_cb_t cancel_cb;
    lv_event_cb_t skip_cb;
} learning_popup_config_t;

static void create_unified_learning_popup(learning_popup_type_t type, learning_popup_config_t *config) {
    lv_obj_t *popup;
    lv_obj_t *cancel_btn;
    lv_obj_t *skip_btn = NULL;
    lv_obj_t *instruction_label;
    
    if (type == LEARNING_POPUP_STANDARD) {
        learning_popup = lv_obj_create(lv_scr_act());
        popup = learning_popup;
    } else {
        easy_learn_popup = lv_obj_create(lv_scr_act());
        popup = easy_learn_popup;
    }
    
    lv_obj_set_size(popup, config->width, config->height);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_radius(popup, 10, 0);
    
    if (config->has_skip_button) {
        cancel_btn = lv_btn_create(popup);
        lv_obj_set_size(cancel_btn, 80, 30);
        lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 20, -10);
        
        skip_btn = lv_btn_create(popup);
        lv_obj_set_size(skip_btn, 80, 30);
        lv_obj_align(skip_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
        lv_obj_set_style_bg_color(skip_btn, lv_color_hex(0x555555), 0);
        lv_obj_t *skip_label = lv_label_create(skip_btn);
        lv_label_set_text(skip_label, "Skip");
        lv_obj_center(skip_label);
        lv_obj_add_event_cb(skip_btn, config->skip_cb, LV_EVENT_CLICKED, NULL);
        
        if (type == LEARNING_POPUP_EASY_LEARN) {
            easy_learn_cancel_btn = cancel_btn;
            easy_learn_skip_btn = skip_btn;
        }
    } else {
        cancel_btn = lv_btn_create(popup);
        lv_obj_set_size(cancel_btn, 80, 30);
        lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
        
        if (type == LEARNING_POPUP_STANDARD) {
            learning_cancel_btn = cancel_btn;
        }
    }
    
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel_btn, config->cancel_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *title_label = lv_label_create(popup);
    lv_label_set_text(title_label, config->title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);
    
    instruction_label = lv_label_create(popup);
    lv_obj_set_style_text_font(instruction_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(instruction_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(instruction_label, LV_ALIGN_CENTER, 0, 0);
    
    if (type == LEARNING_POPUP_EASY_LEARN) {
        easy_learn_instruction_label = instruction_label;
    } else {
        lv_label_set_text(instruction_label, config->instruction);
    }
}

static void create_learning_popup(void) {
    learning_popup_config_t config = {
        .title = "Learning IR Signal",
        .instruction = "Press a button on your remote...",
        .width = 300,
        .height = 150,
        .has_skip_button = false,
        .cancel_cb = learning_cancel_cb,
        .skip_cb = NULL
    };
    
    create_unified_learning_popup(LEARNING_POPUP_STANDARD, &config);
}

// Function to start the IR learning task
static void start_ir_learning_task(void) {
    // Start IR learning task
    ir_learning_cancel = false;
    xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
}

void add_signal_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Add Signal button pressed");
    
    // Check if we have a remote selected
    if (current_remote_path[0] == '\0') {
        ESP_LOGW(TAG, "No remote selected for adding signal");
        // Show error popup
        lv_obj_t *error_popup = lv_msgbox_create(NULL, "Error", "Please select a remote first", NULL, true);
        lv_obj_center(error_popup);
        return;
    }
    
    // Set flag to indicate we're adding to existing remote
    add_signal_mode = true;
    
    // Reset button index for easy learn mode
    existing_remote_button_index = 0;
    
    // Create appropriate popup based on easy learn mode
    if (is_easy_mode) {
        // Create easy learn popup
        create_easy_learn_popup();
    } else {
        // Create learning popup
        create_learning_popup();
        
        // Start IR learning task
        start_ir_learning_task();
    }
}
#else
void add_signal_cb(lv_event_t *e) {
    ESP_LOGW(TAG, "IR RX not configured - cannot add signals");
}
#endif

void delete_remote_cb(lv_event_t *e) {
    // Confirm deletion
    ESP_LOGI(TAG, "Deleting remote: %s", current_remote_path);
    
    // Delete the file
    if (remove(current_remote_path) == 0) {
        ESP_LOGI(TAG, "Successfully deleted remote: %s", current_remote_path);
        // Go back to the remotes list
        display_manager_switch_view(&infrared_view);
    } else {
        ESP_LOGE(TAG, "Failed to delete remote: %s", current_remote_path);
    }
}

// implement universals option callback
static void universals_event_cb(lv_event_t *e) {
    has_remotes_option = false;
    has_universals_option = false;
    in_universals_mode = true;
    strcpy(current_dir, "/mnt/ghostesp/infrared/universals");
    ESP_LOGI(TAG, "entering universals mode, dir=%s", current_dir);
    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    if (ir_file_paths) {
        for (size_t i = 0; i < ir_file_count; i++) free(ir_file_paths[i]);
        free(ir_file_paths);
        ir_file_paths = NULL;
        ir_file_count = 0;
    }
    showing_commands = false;
    lv_obj_clean(list);
    DIR *d = opendir(current_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            const char *name = entry->d_name;
            if (strstr(name, ".ir") || strstr(name, ".IR")) {
                ir_file_paths = realloc(ir_file_paths, (ir_file_count + 1) * sizeof(*ir_file_paths));
                ir_file_paths[ir_file_count] = strdup(name);
                lv_obj_t *btn = lv_list_add_btn(list, NULL, name);
                lv_obj_set_width(btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(btn, file_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)ir_file_count);
                ir_file_count++;
            }
        }
        closedir(d);
    }
    selected_ir_index = 0;
    num_ir_items = ir_file_count;
#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif
    if (ir_file_count > 0) ir_select_item(0);
}

#ifdef CONFIG_USE_ENCODER
static void add_encoder_back_btn(void)
{
    lv_obj_t *btn = lv_list_add_btn(list, NULL, LV_SYMBOL_LEFT " Back");
    lv_obj_set_width(btn, LV_HOR_RES);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(btn, back_event_cb, LV_EVENT_CLICKED,
                        (void *)IR_BACK_OPTION_MAGIC_STR);
    lv_obj_set_user_data(btn, (void *)IR_BACK_OPTION_MAGIC_STR);
    num_ir_items++;
}
#endif

#ifdef CONFIG_HAS_INFRARED_RX
// IR learning functionality
static void cleanup_unified_learning_popup(learning_popup_type_t type)
{
    if (type == LEARNING_POPUP_STANDARD) {
        if (learning_popup) {
            lv_obj_del(learning_popup);
            learning_popup = NULL;
        }
    } else {
        if (easy_learn_popup) {
            lv_obj_del(easy_learn_popup);
            easy_learn_popup = NULL;
            easy_learn_instruction_label = NULL;
        }
    }
}

static void cleanup_learning_popup(void *obj)
{
    cleanup_unified_learning_popup(LEARNING_POPUP_STANDARD);
}

void cleanup_easy_learn_popup(void *obj)
{
    cleanup_unified_learning_popup(LEARNING_POPUP_EASY_LEARN);
}

// Signal preview UI functions
void cleanup_signal_preview_popup(void *obj)
{
    if (signal_preview_popup) {
        lv_obj_del(signal_preview_popup);
        signal_preview_popup = NULL;
        protocol_label = NULL;
        address_label = NULL;
        command_label = NULL;
        save_btn = NULL;
        cancel_btn = NULL;
    }
}



void signal_preview_save_cb(lv_event_t *e)
{
    // Transition to keyboard view for naming
    lv_async_call(cleanup_signal_preview_popup, NULL);
    keyboard_view_set_placeholder("Enter signal name");
    
    // Use different callbacks based on whether we're adding to existing remote, easy learn mode, or creating new
    if (is_easy_mode) {
        // Easy learn mode - use automatic naming (takes priority over add_signal_mode)
        easy_learn_signal_name_callback();
        return; // Don't switch to keyboard view
    } else if (add_signal_mode) {
        // Adding signal to existing remote (non-easy mode)
        keyboard_view_set_submit_callback(add_signal_to_remote_callback);
    } else {
        // Learning new remote (original behavior)
        keyboard_view_set_submit_callback(learned_signal_name_callback);
    }
    
    display_manager_switch_view(&keyboard_view);
}

void easy_learn_signal_name_callback(void)
{
    // Get the signal name based on current button index (which gets updated by skip)
    const char *signal_name = "Unknown";
    if (existing_remote_button_index < num_common_buttons) {
        // Always use the current button index - this reflects what the user sees in the UI
        signal_name = common_button_names[existing_remote_button_index];
    }
    
    strncpy(learned_signal_name, signal_name, sizeof(learned_signal_name) - 1);
    learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
    
    // Save the learned signal based on mode
    if (add_signal_mode) {
        // Adding signal to existing remote
        append_signal_to_remote(learned_signal_name);
    } else {
        // Learning new remote
        save_learned_signal(learned_signal_name);
    }
    
    // Store whether we were in add signal mode before resetting the flag
    bool was_adding_to_existing = add_signal_mode && strlen(current_remote_path) > 0;
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    // Move to the next button index for the next signal
    existing_remote_button_index++;
    
    // If we were adding to an existing remote, reload the remote's signal list to stay on it
    if (was_adding_to_existing) {
        // Reload the current remote's signals to refresh the view
        if (signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if (infrared_manager_read_list(current_remote_path, &signals, &signal_count)) {
            // Rebuild the signal list UI
            lv_obj_clean(list);
            num_ir_items = signal_count;
            selected_ir_index = 0;
            
            for (size_t i = 0; i < signal_count; i++) {
                const char *cmd_name = signals[i].name;
                lv_obj_t *btn = lv_list_add_btn(list, NULL, cmd_name);
                lv_obj_set_width(btn, LV_HOR_RES);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                }
                lv_obj_add_event_cb(btn, command_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            }
            
            // Add remote management options
            lv_obj_t *rename_btn = lv_list_add_btn(list, NULL, "Rename Remote");
            lv_obj_set_width(rename_btn, LV_HOR_RES);
            lv_obj_set_style_bg_color(rename_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(rename_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(rename_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(rename_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *rename_label = lv_obj_get_child(rename_btn, 0);
            if (rename_label) {
                lv_obj_set_style_text_font(rename_label, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(rename_label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_add_event_cb(rename_btn, rename_remote_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_user_data(rename_btn, (void *)(intptr_t)signal_count);
            
            lv_obj_t *add_signal_btn = lv_list_add_btn(list, NULL, "Add Signal");
            lv_obj_set_width(add_signal_btn, LV_HOR_RES);
            lv_obj_set_style_bg_color(add_signal_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(add_signal_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(add_signal_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(add_signal_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *add_signal_label = lv_obj_get_child(add_signal_btn, 0);
            if (add_signal_label) {
                lv_obj_set_style_text_font(add_signal_label, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(add_signal_label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_add_event_cb(add_signal_btn, add_signal_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_user_data(add_signal_btn, (void *)(intptr_t)(signal_count + 1));
            
            lv_obj_t *delete_btn = lv_list_add_btn(list, NULL, "Delete Remote");
            lv_obj_set_width(delete_btn, LV_HOR_RES);
            lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(delete_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(delete_btn, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(delete_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_t *delete_label = lv_obj_get_child(delete_btn, 0);
            if (delete_label) {
                lv_obj_set_style_text_font(delete_label, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(delete_label, lv_color_hex(0xFFFFFF), 0);
            }
            lv_obj_add_event_cb(delete_btn, delete_remote_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_user_data(delete_btn, (void *)(intptr_t)(signal_count + 2));
            
            num_ir_items = signal_count + 3; // signals + 3 management options
            
#ifdef CONFIG_USE_ENCODER
            add_encoder_back_btn();
#endif
            
            ESP_LOGI(TAG, "Reloaded %zu signals for remote after adding new signal", signal_count);
        }
    }
    
    // We're already in the infrared view and have reloaded the signal list
    // Just close the popup - no need to switch views
    if (easy_learn_popup) {
        cleanup_easy_learn_popup(NULL);
    }
}

void signal_preview_cancel_cb(lv_event_t *e)
{
    // Clean up learned signal data
    if (learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
    }
    signal_decoded = false;
    decoded_message = NULL;
    
    // Reset add signal mode flag when cancelling
    add_signal_mode = false;
    
    // Clean up popup immediately (not async) to prevent UI corruption
    cleanup_signal_preview_popup(NULL);
    
    // Return to infrared view after cleanup is complete
    display_manager_switch_view(&infrared_view);
}

void easy_learn_cancel_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Easy Learn cancel button pressed");
    ir_learning_cancel = true;
    
    // Reset the add signal mode flag when cancelling
    add_signal_mode = false;
    
    if (ir_learning_task_handle) {
        // Task will clean up and close popup
    } else {
        cleanup_easy_learn_popup(NULL);
    }
}

void easy_learn_skip_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Easy Learn skip button pressed");
    
    // Skip to next available button name (auto-skip already used ones)
    if (add_signal_mode && strlen(current_remote_path) > 0) {
        // When adding to existing remote, find next unused button
        int original_index = existing_remote_button_index;
        do {
            existing_remote_button_index++;
            if (existing_remote_button_index >= num_common_buttons) {
                existing_remote_button_index = 0; // Wrap around
            }
            
            // Prevent infinite loop by checking if we've wrapped around to the original index
            if (existing_remote_button_index == original_index) {
                break;
            }
            
            const char *button_name = common_button_names[existing_remote_button_index];
            if (!is_button_name_used(current_remote_path, button_name)) {
                break; // Found next unused button
            }
        } while (true);
    } else {
        // For new remotes, just increment normally
        existing_remote_button_index++;
        if (existing_remote_button_index >= num_common_buttons) {
            existing_remote_button_index = 0; // Wrap around
        }
    }
    
    // Update the instruction text to show the next button to learn
    update_easy_learn_instruction_text();
}

void update_signal_preview_selection(void)
{
    if (!save_btn || !cancel_btn) return;
    
    // Update button styles based on selection
    if (preview_selected_option == 0) {
        // Save selected - white background, black text
        lv_obj_set_style_bg_color(save_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(save_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *save_label = lv_obj_get_child(save_btn, 0);
        if (save_label) lv_obj_set_style_text_color(save_label, lv_color_hex(0x000000), 0);
        
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    } else {
        // Cancel selected - white background, black text
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
        
        // Save unselected - dark background, white text
        lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(save_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *save_label = lv_obj_get_child(save_btn, 0);
        if (save_label) lv_obj_set_style_text_color(save_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- New: Update learning popup cancel button highlight ---
void update_learning_popup_selection(void)
{
    if (!learning_cancel_btn) return;
    if (preview_selected_option == 1) {
        // Cancel selected - white background, black text
        lv_obj_set_style_bg_color(learning_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(learning_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(learning_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
    } else {
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(learning_cancel_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(learning_cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(learning_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- Update Easy Learn popup button highlighting ---
void update_easy_learn_popup_selection(void)
{
    if (!easy_learn_cancel_btn || !easy_learn_skip_btn) return;
    
    // Update Cancel button
    if (easy_learn_selected_option == 0) {
        // Cancel selected - white background, black text
        lv_obj_set_style_bg_color(easy_learn_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_cancel_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(easy_learn_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
    } else {
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(easy_learn_cancel_btn, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(easy_learn_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    }
    
    // Update Skip button
    if (easy_learn_selected_option == 1) {
        // Skip selected - white background, black text
        lv_obj_set_style_bg_color(easy_learn_skip_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_skip_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *skip_label = lv_obj_get_child(easy_learn_skip_btn, 0);
        if (skip_label) lv_obj_set_style_text_color(skip_label, lv_color_hex(0x000000), 0);
    } else {
        // Skip unselected - dark background, white text
        lv_obj_set_style_bg_color(easy_learn_skip_btn, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_skip_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *skip_label = lv_obj_get_child(easy_learn_skip_btn, 0);
        if (skip_label) lv_obj_set_style_text_color(skip_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- Update Easy Learn instruction text with current suggested button name ---
void update_easy_learn_instruction_text(void)
{
    if (!easy_learn_instruction_label) return;
    
    const char *button_name = "Unknown";
    
    // Always use the current button index to show what button we're currently suggesting
    if (existing_remote_button_index < num_common_buttons) {
        button_name = common_button_names[existing_remote_button_index];
    }
    
    lv_label_set_text_fmt(easy_learn_instruction_label, "Press '%s' button...", button_name);
}

// --- Check if a button name is already used in a remote file ---
bool is_button_name_used(const char* remote_path, const char* button_name)
{
    if (!remote_path || strlen(remote_path) == 0 || !button_name) {
        return false;
    }
    
    FILE *f = fopen(remote_path, "r");
    if (!f) {
        return false;
    }
    
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        char *s = buf;
        // Skip whitespace
        while (*s && isspace((unsigned char)*s)) s++;
        // Skip comments and empty lines
        if (*s == '#' || *s == '\0') continue;
        
        // Look for "name:" lines
        if (strncmp(s, "name:", 5) == 0) {
            char *v = s + 5;
            // Skip whitespace after "name:"
            while (*v && isspace((unsigned char)*v)) v++;
            // Remove trailing whitespace
            char *e = v + strlen(v) - 1;
            while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
            
            // Check if this name matches the button name we're looking for
            if (strcmp(v, button_name) == 0) {
                fclose(f);
                return true;
            }
        }
    }
    fclose(f);
    return false;
}

// --- Get next available common button name by reading existing signals ---
const char* get_next_available_button_name(const char* remote_path)
{
    if (!remote_path || strlen(remote_path) == 0) {
        // No remote path, return first common button
        return common_button_names[0];
    }
    
    // Read existing signal names from the remote file
    FILE *f = fopen(remote_path, "r");
    if (!f) {
        // File doesn't exist or can't be opened, return first common button
        return common_button_names[0];
    }
    
    // Track which common button names are already used
    bool used_buttons[num_common_buttons];
    memset(used_buttons, false, sizeof(used_buttons));
    
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        char *s = buf;
        // Skip whitespace
        while (*s && isspace((unsigned char)*s)) s++;
        // Skip comments and empty lines
        if (*s == '#' || *s == '\0') continue;
        
        // Look for "name:" lines
        if (strncmp(s, "name:", 5) == 0) {
            char *v = s + 5;
            // Skip whitespace after "name:"
            while (*v && isspace((unsigned char)*v)) v++;
            // Remove trailing whitespace
            char *e = v + strlen(v) - 1;
            while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
            
            // Check if this name matches any common button name
            for (int i = 0; i < num_common_buttons; i++) {
                if (strcmp(v, common_button_names[i]) == 0) {
                    used_buttons[i] = true;
                    break;
                }
            }
        }
    }
    fclose(f);
    
    // Find the first unused common button name
    for (int i = 0; i < num_common_buttons; i++) {
        if (!used_buttons[i]) {
            return common_button_names[i];
        }
    }
    
    // All common buttons are used, return the first one (user can override)
    return common_button_names[0];
}

// --- Generate unique remote filename by auto-incrementing if file exists ---
void generate_unique_remote_filename(const char* base_name, char* output_filename, size_t output_size)
{
    if (!base_name || !output_filename || output_size == 0) {
        return;
    }
    
    // Start with the base filename
    snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s.ir", base_name);
    
    // Check if file exists
    FILE *test_file = fopen(output_filename, "r");
    if (!test_file) {
        // File doesn't exist, we can use the base name
        return;
    }
    fclose(test_file);
    
    // File exists, try incrementing numbers
    int counter = 2;
    while (counter <= 999) { // Reasonable limit to prevent infinite loop
        snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s_%d.ir", base_name, counter);
        
        test_file = fopen(output_filename, "r");
        if (!test_file) {
            // This filename is available
            return;
        }
        fclose(test_file);
        counter++;
    }
    
    // If we get here, we couldn't find a unique name (very unlikely)
    // Fall back to using timestamp
    time_t now = time(NULL);
    snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s_%ld.ir", base_name, (long)now);
}

void create_easy_learn_popup(void)
{
    learning_popup_config_t config = {
        .title = "Easy Learn Mode",
        .instruction = "", // Will be set by update_easy_learn_instruction_text()
        .width = LV_HOR_RES - 30,
        .height = 160,
        .has_skip_button = true,
        .cancel_cb = easy_learn_cancel_cb,
        .skip_cb = easy_learn_skip_cb
    };
    
    create_unified_learning_popup(LEARNING_POPUP_EASY_LEARN, &config);
    
    // Initialize button index to first unused button when adding to existing remote
    if (add_signal_mode && strlen(current_remote_path) > 0) {
        // Find first unused button name
        existing_remote_button_index = 0;
        for (int i = 0; i < num_common_buttons; i++) {
            if (!is_button_name_used(current_remote_path, common_button_names[i])) {
                existing_remote_button_index = i;
                break;
            }
        }
    }
    
    // Update instruction text with current suggested button name
    update_easy_learn_instruction_text();
    
    // Initialize selection to Cancel button and update highlighting
    easy_learn_selected_option = 0;
    update_easy_learn_popup_selection();
    
    // Start IR learning task
    ir_learning_cancel = false;
    xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
}

void create_signal_preview_popup(void)
{
    // Initialize popup style if not already done
    if (!popup_style_initialized) {
        lv_style_init(&popup_style);
        lv_style_set_bg_color(&popup_style, lv_color_hex(0x222222));
        lv_style_set_border_color(&popup_style, lv_color_hex(0xFFFFFF));
        lv_style_set_border_width(&popup_style, 1);
        lv_style_set_radius(&popup_style, 8);
        popup_style_initialized = true;
    }
    
    // Create popup container with compact sizing for landscape displays
    signal_preview_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(signal_preview_popup, LV_HOR_RES - 30, 160);  // Reduced height for landscape
    lv_obj_center(signal_preview_popup);
    lv_obj_add_style(signal_preview_popup, &popup_style, 0);
    
    // Remove scrollbars and ensure content fits
    lv_obj_set_scrollbar_mode(signal_preview_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(signal_preview_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(signal_preview_popup, 8, 0);
    
    // Create buttons first to ensure proper z-order
    save_btn = lv_btn_create(signal_preview_popup);
    lv_obj_set_size(save_btn, 80, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 35, -5);
    lv_obj_add_event_cb(save_btn, signal_preview_save_cb, LV_EVENT_CLICKED, NULL);
    
    // Remove default button styling
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(save_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(save_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(save_label);
    
    cancel_btn = lv_btn_create(signal_preview_popup);
    lv_obj_set_size(cancel_btn, 80, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -35, -5);
    lv_obj_add_event_cb(cancel_btn, signal_preview_cancel_cb, LV_EVENT_CLICKED, NULL);
    
    // Remove default button styling
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cancel_label);
    
    // Title
    lv_obj_t *title = lv_label_create(signal_preview_popup);
    lv_label_set_text(title, "IR Signal Decoded");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Protocol info
    protocol_label = lv_label_create(signal_preview_popup);
    address_label = lv_label_create(signal_preview_popup);
    command_label = lv_label_create(signal_preview_popup);
    
    // Set concise text
    if (signal_decoded && decoded_message) {
        lv_label_set_text_fmt(protocol_label, "%s", infrared_protocol_to_string(decoded_message->protocol));
        lv_label_set_text_fmt(address_label, "Addr: 0x%lX", decoded_message->address);
        lv_label_set_text_fmt(command_label, "Cmd: 0x%lX", decoded_message->command);
    } else {
        lv_label_set_text(protocol_label, "Raw Signal");
        lv_label_set_text(address_label, "Unknown Protocol");
        lv_label_set_text(command_label, "");
    }
    
    // Style and position labels
    lv_obj_set_style_text_color(protocol_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(address_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(command_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(protocol_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(address_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(command_label, &lv_font_montserrat_14, 0);
    
    lv_obj_align(protocol_label, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_align(address_label, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_align(command_label, LV_ALIGN_TOP_MID, 0, 45);
    
    // Raw signal info
    lv_obj_t *raw_info = lv_label_create(signal_preview_popup);
    lv_label_set_text_fmt(raw_info, "%d timings", 
                         learned_signal.payload.raw.timings_size);
    lv_obj_set_style_text_color(raw_info, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(raw_info, &lv_font_montserrat_14, 0);
    lv_obj_align(raw_info, LV_ALIGN_TOP_MID, 0, 65);
    
    // Set initial selection
    preview_selected_option = 0;
    update_signal_preview_selection();
}

static void save_learned_signal(const char *signal_name) {
    ESP_LOGI(TAG, "save_learned_signal called with name: %s", signal_name ? signal_name : "NULL");
    ESP_LOGI(TAG, "Signal data: timings=%p, size=%d, is_raw=%d", 
             learned_signal.payload.raw.timings, 
             learned_signal.payload.raw.timings_size,
             learned_signal.is_raw);
    
    if (!signal_name || strlen(signal_name) == 0) {
        ESP_LOGE(TAG, "Invalid signal name provided");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    // Check if we have valid signal data
    if (!learned_signal.is_raw) {
        ESP_LOGE(TAG, "No valid signal data to save");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    if (!learned_signal.payload.raw.timings || learned_signal.payload.raw.timings_size == 0) {
        ESP_LOGE(TAG, "No timing data in signal");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    // Additional safety check: validate timing data pointer
    if ((uintptr_t)learned_signal.payload.raw.timings < 0x3C000000 || 
        (uintptr_t)learned_signal.payload.raw.timings > 0x3FFFFFFF) {
        ESP_LOGE(TAG, "Invalid timing data pointer: %p", learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
        return;
    }
    
    // Add a small delay to ensure any pending operations are complete
    vTaskDelay(pdMS_TO_TICKS(10));
    
    char filename[256];
    // Ensure the signal name doesn't cause buffer overflow
    // Use a conservative approach to avoid compiler warnings
    size_t max_signal_name_len = sizeof(filename) - 64; // Reserve 64 chars for path prefix and safety margin
    
    // Ensure max_signal_name_len is reasonable
    if (max_signal_name_len <= 0 || max_signal_name_len > 190) {
        max_signal_name_len = 190; // Set a reasonable limit
    }
    
    // Truncate signal name if too long
    char truncated_signal_name[max_signal_name_len + 1];
    strncpy(truncated_signal_name, signal_name, max_signal_name_len);
    truncated_signal_name[max_signal_name_len] = '\0';
    
    // Generate unique filename to prevent overwriting existing remotes
    generate_unique_remote_filename(truncated_signal_name, filename, sizeof(filename));
    
    // Final safety check for filename length
    if (strlen(filename) >= sizeof(filename) - 1) {
        ESP_LOGE(TAG, "Generated filename would be too long");
        // Free timing data if it exists to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    ESP_LOGI(TAG, "Creating IR file: %s", filename);
    
    ESP_LOGI(TAG, "Opening file for writing...");

    // Save signal to file
    FILE *f = fopen(filename, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s, errno: %d", filename, errno);
        // Free timing data to prevent memory leaks
        if (learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }

    ESP_LOGI(TAG, "File opened successfully, writing content...");

    // Write header directly to the file
    fprintf(f, "Filetype: IR signals file\n");
    fprintf(f, "Version: 1\n");
    fprintf(f, "#\n");
    fprintf(f, "# Generated by Ghost ESP\n");
    fprintf(f, "# Signal: %s\n", signal_name);
    fprintf(f, "#\n");
    fprintf(f, "name: %s\n", signal_name);
    
    // Check if we have a decoded signal to save in proper format
    if (signal_decoded && decoded_message) {
        ESP_LOGI(TAG, "Saving decoded signal: %s, addr=0x%08lX, cmd=0x%08lX", 
                infrared_protocol_to_string(decoded_message->protocol),
                decoded_message->address, decoded_message->command);
        
        // Save as decoded signal
        fprintf(f, "type: parsed\n");
        fprintf(f, "protocol: %s\n", infrared_protocol_to_string(decoded_message->protocol));
        fprintf(f, "address: %02lX %02lX %02lX %02lX\n", 
                (decoded_message->address >> 0) & 0xFF,
                (decoded_message->address >> 8) & 0xFF, 
                (decoded_message->address >> 16) & 0xFF,
                (decoded_message->address >> 24) & 0xFF);
        fprintf(f, "command: %02lX %02lX %02lX %02lX\n",
                (decoded_message->command >> 0) & 0xFF,
                (decoded_message->command >> 8) & 0xFF,
                (decoded_message->command >> 16) & 0xFF, 
                (decoded_message->command >> 24) & 0xFF);
    } else {
        ESP_LOGI(TAG, "Saving as raw signal (could not decode)");
        
        // Save as raw signal
        fprintf(f, "type: raw\n");
        fprintf(f, "frequency: %lu\n", learned_signal.payload.raw.frequency);
        fprintf(f, "duty_cycle: %.6f\n", learned_signal.payload.raw.duty_cycle);
        fprintf(f, "data: ");

        // Write timing data
        for (size_t i = 0; i < learned_signal.payload.raw.timings_size; i++) {
            fprintf(f, "%lu ", learned_signal.payload.raw.timings[i]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    
    ESP_LOGI(TAG, "IR signal saved to %s", filename);
    
    // Free timing data to prevent memory leaks
    if (learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
    }
}



// RMT RX callback function
static bool ir_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t)user_ctx;
    
    // Send the received RMT symbols to the parser task
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    
    return high_task_wakeup == pdTRUE;
}

static void ir_learning_task(void *arg) {
    ESP_LOGI(TAG, "IR learning task started");
    
    // Reset learning cancel flag
    ir_learning_cancel = false;
    
    // Check if RMT channel is available (should be initialized by view_create)
    if (!rx_channel) {
        ESP_LOGE(TAG, "RMT RX channel not initialized");
        lv_async_call(cleanup_learning_popup, NULL);
        ir_learning_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Check if RX queue is available
    if (!ir_rx_queue) {
        ESP_LOGE(TAG, "RX queue not initialized");
        lv_async_call(cleanup_learning_popup, NULL);
        ir_learning_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Initialize decoder context
    decoder_context = infrared_decoder_alloc();
    if (!decoder_context) {
        ESP_LOGE(TAG, "Failed to allocate decoder context");
        lv_async_call(cleanup_learning_popup, NULL);
        ir_learning_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Reset decoder state
    signal_decoded = false;
    decoded_message = NULL;
    
    // Receive buffer
    rmt_symbol_word_t raw_symbols[64];
    
    // Configure receive parameters based on ESP-IDF documentation
    // These values are suitable for typical IR remote protocols
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,     // Minimum pulse width (smaller than typical IR pulse ~560µs)
        .signal_range_max_ns = 12000000, // Maximum pulse width (larger than typical IR gap ~9ms)
    };
    
    while (!ir_learning_cancel) {
        ESP_LOGI(TAG, "Starting IR receive operation...");
        
        // Check if channel is still valid before trying to receive
        if (!rx_channel) {
            ESP_LOGE(TAG, "RMT RX channel is NULL");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Start a single receive operation (non-blocking)
        esp_err_t ret = rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start RMT receive: %d", ret);
            // Add a delay and try again
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Wait for IR signal with timeout (inspired by Arduino IRremote timeout handling)
        rmt_rx_done_event_data_t rx_data;
        TickType_t timeout_ticks = pdMS_TO_TICKS(1000);  // 1 second timeout per attempt
        
        if (xQueueReceive(ir_rx_queue, &rx_data, timeout_ticks) == pdTRUE) {
            // Data received, process it
            ESP_LOGI(TAG, "IR signal received: %d symbols", rx_data.num_symbols);
            
            // Log first few symbols for debugging
            for (size_t i = 0; i < rx_data.num_symbols && i < 5; i++) {
                ESP_LOGD(TAG, "Symbol %d: duration0=%u, duration1=%u", i, 
                         rx_data.received_symbols[i].duration0, rx_data.received_symbols[i].duration1);
            }
            
            // Enhanced signal validation inspired by Arduino IRremote
            bool is_valid_signal = false;
            bool has_overflow = false; // TODO: Implement proper overflow detection
            uint32_t total_duration = 0;
            uint32_t max_pulse_duration = 0;
            uint32_t min_pulse_duration = UINT32_MAX;
            uint32_t pulse_count = 0;
            uint32_t gap_count = 0;
            
            if (has_overflow) {
                ESP_LOGW(TAG, "Buffer overflow detected - signal may be truncated");
            }
            
            // More robust signal validation
            if (rx_data.num_symbols >= 6 && rx_data.num_symbols <= 200) {  // Allow wider range but still filter noise
                
                for (size_t i = 0; i < rx_data.num_symbols; i++) {
                    uint32_t duration_us = rx_data.received_symbols[i].duration0 + rx_data.received_symbols[i].duration1;
                    total_duration += duration_us;
                    
                    // Analyze pulse and gap durations separately
                    uint32_t pulse_duration = (rx_data.received_symbols[i].level0 == 1) ? rx_data.received_symbols[i].duration0 : rx_data.received_symbols[i].duration1;
                    uint32_t gap_duration = (rx_data.received_symbols[i].level0 == 0) ? rx_data.received_symbols[i].duration0 : rx_data.received_symbols[i].duration1;
                    
                    if (pulse_duration > 0) {
                        pulse_count++;
                        if (pulse_duration > max_pulse_duration) max_pulse_duration = pulse_duration;
                        if (pulse_duration < min_pulse_duration) min_pulse_duration = pulse_duration;
                    }
                    if (gap_duration > 0) {
                        gap_count++;
                    }
                }
                
                // Enhanced validation criteria based on typical IR remote characteristics
                bool duration_valid = (total_duration >= 5000 && total_duration <= 200000);  // 5-200ms total
                bool pulse_valid = (max_pulse_duration >= 200 && max_pulse_duration <= 20000);  // 0.2-20ms max pulse
                bool min_pulse_valid = (min_pulse_duration >= 100 && min_pulse_duration <= 5000);  // 0.1-5ms min pulse
                bool structure_valid = (pulse_count >= 3 && gap_count >= 2);  // Must have pulses and gaps
                
                if (duration_valid && pulse_valid && min_pulse_valid && structure_valid) {
                    is_valid_signal = true;
                    ESP_LOGI(TAG, "Valid IR signal: %lu symbols, %lu us total, pulse range %lu-%lu us", 
                            rx_data.num_symbols, total_duration, min_pulse_duration, max_pulse_duration);
                } else {
                    ESP_LOGW(TAG, "Invalid signal: dur=%s, pulse=%s, min_pulse=%s, struct=%s",
                            duration_valid ? "OK" : "FAIL",
                            pulse_valid ? "OK" : "FAIL", 
                            min_pulse_valid ? "OK" : "FAIL",
                            structure_valid ? "OK" : "FAIL");
                }
            }
            
            if (is_valid_signal) {
                // Convert received data to our format
                memset(&learned_signal, 0, sizeof(learned_signal));
                learned_signal.is_raw = true;
                learned_signal.payload.raw.frequency = 38000; // Default 38kHz
                learned_signal.payload.raw.duty_cycle = 0.33f; // Default 33% duty cycle
                learned_signal.payload.raw.timings_size = rx_data.num_symbols * 2;
                
                // Validate that we have symbols to process
                if (rx_data.num_symbols == 0) {
                    ESP_LOGW(TAG, "Received valid signal flag but zero symbols, ignoring");
                    continue;
                }
                
                learned_signal.payload.raw.timings = malloc(learned_signal.payload.raw.timings_size * sizeof(uint32_t));
                
                if (learned_signal.payload.raw.timings) {
                    // Copy timing data from RMT symbols
                    for (size_t i = 0; i < rx_data.num_symbols; i++) {
                        learned_signal.payload.raw.timings[i * 2] = rx_data.received_symbols[i].duration0;
                        learned_signal.payload.raw.timings[i * 2 + 1] = rx_data.received_symbols[i].duration1;
                    }
                    
                    // Additional validation: ensure we actually copied data
                    if (learned_signal.payload.raw.timings_size > 0) {
                        ESP_LOGI(TAG, "IR signal data prepared: %d timings, %d bytes", 
                                learned_signal.payload.raw.timings_size, 
                                learned_signal.payload.raw.timings_size * sizeof(uint32_t));
                        
                        // Try to decode the signal using the decoder
                        infrared_decoder_reset(decoder_context);
                        signal_decoded = false;
                        decoded_message = NULL;
                        
                        // Process RMT symbols as a continuous timing stream
                        ESP_LOGI(TAG, "Processing %d RMT symbols for decoding:", rx_data.num_symbols);
                        
                        // Convert RMT symbols to a continuous stream of level/timing pairs
                        // Each RMT symbol contains two timing periods with their respective levels
                        for (size_t i = 0; i < rx_data.num_symbols && !signal_decoded; i++) {
                            rmt_symbol_word_t symbol = rx_data.received_symbols[i];
                            
                            ESP_LOGD(TAG, "Symbol %d: dur0=%u(lvl%d), dur1=%u(lvl%d)", 
                                     i, symbol.duration0, symbol.level0, symbol.duration1, symbol.level1);
                            
                            // Feed both timing periods from this symbol to the decoder
                            // This maintains the continuous timing relationship
                            
                            // Process first timing period (duration0 with level0)
                            if (symbol.duration0 > 0) {
                                // Invert level since IR receivers typically output inverted signals
                                // (LOW when IR detected, HIGH when no IR)
                                bool inverted_level0 = !symbol.level0;
                                ESP_LOGD(TAG, "Feeding decoder: level=%d, timing=%uµs (raw_level=%d)", inverted_level0, symbol.duration0, symbol.level0);
                                InfraredDecodedMessage* result = infrared_decoder_decode(decoder_context, inverted_level0, symbol.duration0);
                                if (result) {
                                    decoded_message = result;
                                    signal_decoded = true;
                                    ESP_LOGI(TAG, "Signal decoded: %s, addr=0x%08lX, cmd=0x%08lX", 
                                            infrared_protocol_to_string(result->protocol),
                                            result->address, result->command);
                                    break;
                                }
                            }
                            
                            // Process second timing period (duration1 with level1) if not already decoded
                            if (!signal_decoded && symbol.duration1 > 0) {
                                // Invert level since IR receivers typically output inverted signals
                                bool inverted_level1 = !symbol.level1;
                                ESP_LOGD(TAG, "Feeding decoder: level=%d, timing=%uµs (raw_level=%d)", inverted_level1, symbol.duration1, symbol.level1);
                                InfraredDecodedMessage* result = infrared_decoder_decode(decoder_context, inverted_level1, symbol.duration1);
                                if (result) {
                                    decoded_message = result;
                                    signal_decoded = true;
                                    ESP_LOGI(TAG, "Signal decoded: %s, addr=0x%08lX, cmd=0x%08lX", 
                                            infrared_protocol_to_string(result->protocol),
                                            result->address, result->command);
                                    break;
                                }
                            }
                        }
                        
                        // Send end-of-signal indication to decoder if not already decoded
                        // This is crucial for protocols like SIRC that need end-of-signal detection
                        if (!signal_decoded) {
                            ESP_LOGD(TAG, "Sending end-of-signal indication to decoder (timing=0)");
                            InfraredDecodedMessage* result = infrared_decoder_decode(decoder_context, false, 0);
                            if (result) {
                                decoded_message = result;
                                signal_decoded = true;
                                ESP_LOGI(TAG, "Signal decoded after end-of-signal: %s, addr=0x%08lX, cmd=0x%08lX", 
                                        infrared_protocol_to_string(result->protocol),
                                        result->address, result->command);
                            }
                        }
                        
                        if (!signal_decoded) {
                            ESP_LOGI(TAG, "Signal could not be decoded - will save as raw");
                        }
                        
                        // Set flag to preserve timing data during view transition
                        preserve_learned_signal = true;
                        
                        // Signal received successfully, show preview popup
                        if (is_easy_mode) {
                            lv_async_call(cleanup_easy_learn_popup, NULL);
                        } else {
                            lv_async_call(cleanup_learning_popup, NULL);
                        }
                        
                        // Create and show signal preview popup
                        lv_async_call((lv_async_cb_t)create_signal_preview_popup, NULL);
                        
                        // Don't clean up timing data here - let the callback handle it
                        // RMT channel remains active for future learning sessions
                        
                        ir_learning_task_handle = NULL;
                        vTaskDelete(NULL);
                        return;
                    } else {
                        ESP_LOGW(TAG, "Timing data size is zero after allocation");
                        free(learned_signal.payload.raw.timings);
                        learned_signal.payload.raw.timings = NULL;
                        learned_signal.payload.raw.timings_size = 0;
                        continue;
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for timing data");
                    // Ensure we don't have a dangling pointer
                    learned_signal.payload.raw.timings = NULL;
                    learned_signal.payload.raw.timings_size = 0;
                    // Continue listening for another signal
                    continue;
                }
            } else {
                ESP_LOGW(TAG, "Invalid IR signal: %d symbols, %uµs total, %uµs max - likely noise", 
                         (int)rx_data.num_symbols, total_duration, max_pulse_duration);
                // Continue listening for another signal
            }
        } else {
            // Timeout - no signal received within 1 second
            ESP_LOGD(TAG, "No IR signal received, continuing to listen...");
        }
    }
    
    // Cleanup - only reached if learning was cancelled or failed
    // RMT channel is managed by view lifecycle, don't clean it up here
    
    // Clean up decoder context
    if (decoder_context) {
        infrared_decoder_free(decoder_context);
        decoder_context = NULL;
    }
    signal_decoded = false;
    decoded_message = NULL;
    
    // Only clean up timing data if learning was cancelled
    if (ir_learning_cancel && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
    }
    
    if (ir_learning_cancel) {
        lv_async_call(cleanup_learning_popup, NULL);
    }
    
    ir_learning_task_handle = NULL;
    vTaskDelete(NULL);
}

#endif

void learning_cancel_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Learn Remote cancel button pressed");
#ifdef CONFIG_HAS_INFRARED_RX
    ir_learning_cancel = true;
    
    // Reset the add signal mode flag when cancelling
    add_signal_mode = false;
    
    if (ir_learning_task_handle) {
        // Task will clean up and close popup
    } else {
        cleanup_learning_popup(NULL);
    }
#else
    cleanup_learning_popup(NULL);
#endif
}

void easy_learn_toggle_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Easy Learn toggle pressed");
    
#ifdef CONFIG_HAS_INFRARED_RX
    // Toggle the easy mode state
    is_easy_mode = !is_easy_mode;
    
    // Update the settings
    settings_set_infrared_easy_mode(&G_Settings, is_easy_mode);
    settings_save(&G_Settings);
    
    // Update the button text to reflect the new state
    lv_obj_t *btn = NULL;
    if (e != NULL) {
        // Called from LVGL event (touch)
        btn = lv_event_get_target(e);
    } else {
        // Called from encoder/keyboard input - find the Easy Learn button
        // Easy Learn button is at index: (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1
        int easy_learn_index = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1;
        btn = lv_obj_get_child(list, easy_learn_index);
    }
    
    if (btn) {
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_label_set_text(label, is_easy_mode ? "Easy Learn [X]" : "Easy Learn [ ]");
        }
    }
    
    ESP_LOGI(TAG, "Easy Learn mode %s", is_easy_mode ? "enabled" : "disabled");
#else
    // Do nothing if IR RX is not configured
    ESP_LOGI(TAG, "Easy Learn mode not available (IR RX not configured)");
#endif
}

#ifdef CONFIG_HAS_INFRARED_RX

static void learn_remote_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Learn Remote button pressed");
    
    // Reset flag to indicate we're learning a new remote (not adding to existing)
    add_signal_mode = false;
    
    // Reset button index for easy learn mode
    existing_remote_button_index = 0;
    
    // Create appropriate popup based on easy learn mode
    if (is_easy_mode) {
        // Create easy learn popup
        create_easy_learn_popup();
    } else {
        // Create standard learning popup
        learning_popup = lv_obj_create(lv_scr_act());
        lv_obj_set_size(learning_popup, 300, 150);
        lv_obj_center(learning_popup);
        lv_obj_set_style_bg_color(learning_popup, lv_color_hex(0x2E2E2E), 0);
        lv_obj_set_style_border_color(learning_popup, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(learning_popup, 2, 0);
    lv_obj_set_style_radius(learning_popup, 10, 0);
    
    // Create cancel button first to ensure proper z-order
    learning_cancel_btn = lv_btn_create(learning_popup);
    lv_obj_set_size(learning_cancel_btn, 80, 30);
    lv_obj_align(learning_cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(learning_cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_t *cancel_label = lv_label_create(learning_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    
    // Add cancel button callback
    lv_obj_add_event_cb(learning_cancel_btn, learning_cancel_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *title_label = lv_label_create(learning_popup);
    lv_label_set_text(title_label, "Learning IR Signal");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);
    
    lv_obj_t *instruction_label = lv_label_create(learning_popup);
    lv_label_set_text(instruction_label, "Press a button on your remote...");
    lv_obj_set_style_text_font(instruction_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(instruction_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(instruction_label, LV_ALIGN_CENTER, 0, 0);
    
    // Start IR learning task
    ir_learning_cancel = false;
    xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
    }
}
#endif

View infrared_view = {
    .root = NULL,
    .create = infrared_view_create,
    .destroy = infrared_view_destroy,
    .name = "Infrared View",
    .get_hardwareinput_callback = NULL,
    .input_callback = infrared_view_input_cb
};
