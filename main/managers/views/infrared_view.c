#include "managers/views/infrared_view.h"
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include <lvgl/lvgl.h>
#include <dirent.h>
#include <string.h>
#include "managers/infrared_manager.h"
#include "managers/rgb_manager.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Infrared view UI
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
static volatile bool universal_transmit_cancel = false;

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
#ifdef CONFIG_USE_ENCODER
static void add_encoder_back_btn(void);
#endif

static const char *TAG = "infrared_view";

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

        num_ir_items = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0);
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

#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif

    // set num_ir_items after all buttons are added (including back button)
    num_ir_items = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0);
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
    // clear previous
    lv_obj_t *prev = lv_obj_get_child(list, selected_ir_index);
    if(prev) {
        lv_obj_set_style_bg_color(prev, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    }
    selected_ir_index = index;
    lv_obj_t *cur = lv_obj_get_child(list, selected_ir_index);
    if(cur) {
        lv_obj_set_style_bg_color(cur, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_scroll_to_view(cur, LV_ANIM_OFF);
    }
}

void infrared_view_input_cb(InputEvent *event) {
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
                            } else {
                                int file_idx = i - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0));
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
                } else {
                    int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0));
                    ESP_LOGI(TAG, "joystick enter pressed, opening selected file at index %d", file_idx);
                    file_event_open(file_idx);
                }
            } else {
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
                if (has_remotes_option && selected_ir_index == 0) {
                    ESP_LOGI(TAG, "Keyboard Enter pressed on Remotes, opening remotes directory");
                    remotes_event_cb(NULL);
                } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                    ESP_LOGI(TAG, "Keyboard Enter pressed on Universals, opening universals directory");
                    universals_event_cb(NULL);
                }
                else {
                    int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0));
                    ESP_LOGI(TAG, "Keyboard Enter pressed, opening selected file at index %d", file_idx);
                    file_event_open(file_idx);
                }
            } else {
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
                if (has_remotes_option && selected_ir_index == 0) {
                    remotes_event_cb(NULL);
                } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                    universals_event_cb(NULL);
                } else {
                    int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0));
                    file_event_open(file_idx);
                }
            } else {
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
#ifdef CONFIG_USE_ENCODER
    add_encoder_back_btn();
#endif
    selected_ir_index = 0;
    num_ir_items = ir_file_count;
#ifdef CONFIG_USE_ENCODER
#endif
    if (ir_file_count > 0) ir_select_item(0);
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
#ifdef CONFIG_ENCODER_INA
    add_encoder_back_btn();
#endif
    selected_ir_index = 0;
    num_ir_items = ir_file_count;
#ifdef CONFIG_USE_ENCODER
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

// provide hardware input callback registration
static void get_infrared_view_callback(void **callback) {
    *callback = infrared_view_input_cb;
}

// Define the view
View infrared_view = {
    .root = NULL,
    .create = infrared_view_create,
    .destroy = infrared_view_destroy,
    .input_callback = infrared_view_input_cb,
    .name = "Infrared",
    .get_hardwareinput_callback = get_infrared_view_callback,
}; 