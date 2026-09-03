#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GHOSTESP_APP_API_VERSION 1u
#define GHOSTESP_APP_FLAG_PERMISSIONS_ENFORCED (1u << 0)
#define GHOSTESP_APP_FLAG_ABSOLUTE_STORAGE_ALLOWED (1u << 1)

typedef enum {
    GHOSTESP_INPUT_NONE = 0,
    GHOSTESP_INPUT_LEFT,
    GHOSTESP_INPUT_RIGHT,
    GHOSTESP_INPUT_UP,
    GHOSTESP_INPUT_DOWN,
    GHOSTESP_INPUT_SELECT,
    GHOSTESP_INPUT_BACK,
    GHOSTESP_INPUT_KEY,
    GHOSTESP_INPUT_TOUCH,
} ghostesp_input_type_t;

typedef struct {
    ghostesp_input_type_t type;
    int32_t value;
    int32_t x;
    int32_t y;
    bool pressed;
    bool is_touch_move;
} ghostesp_input_event_t;

#define GHOSTESP_BUTTON_LEFT   (1u << 0)
#define GHOSTESP_BUTTON_SELECT (1u << 1)
#define GHOSTESP_BUTTON_UP     (1u << 2)
#define GHOSTESP_BUTTON_RIGHT  (1u << 3)
#define GHOSTESP_BUTTON_DOWN   (1u << 4)

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
    uint32_t last_change_ms;
} ghostesp_input_snapshot_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth_mode;
} ghostesp_wifi_ap_info_t;

#define GHOSTESP_STORAGE_NAME_MAX 64

typedef struct {
    char name[GHOSTESP_STORAGE_NAME_MAX];
    bool is_directory;
} ghostesp_storage_entry_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
} ghostesp_ble_device_info_t;

typedef struct {
    uint8_t type;
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
    char subtype[20];
    bool tracking;
} ghostesp_ble_detect_info_t;

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    int8_t rssi;
    uint8_t event_type;
    uint32_t seen_count;
    char name[24];
    bool has_flags;
    uint8_t flags;
    bool has_tx_power;
    int8_t tx_power;
    bool has_manufacturer_id;
    uint16_t manufacturer_id;
    bool is_ibeacon;
    char ibeacon_uuid[37];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t ibeacon_measured_power;
    char adv_type[16];
    char manufacturer[24];
    char oui_vendor[32];
    char services[96];
    char service_data[64];
    bool has_appearance;
    uint16_t appearance;
} ghostesp_ble_adv_info_t;

#define GHOSTESP_ESPNOW_NAME_MAX 24
#define GHOSTESP_ESPNOW_MESSAGE_MAX 160

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t last_seen_ms;
    char name[GHOSTESP_ESPNOW_NAME_MAX];
} ghostesp_espnow_peer_t;

typedef struct {
    uint8_t sender_mac[6];
    uint32_t received_at_ms;
    char sender_name[GHOSTESP_ESPNOW_NAME_MAX];
    char text[GHOSTESP_ESPNOW_MESSAGE_MAX];
} ghostesp_espnow_message_t;

typedef void *ghostesp_ui_obj_t;
typedef void (*ghostesp_ui_button_cb_t)(void *user);

typedef void *ghostesp_options_t;
typedef void *ghostesp_detail_t;
typedef void *ghostesp_popup_t;
typedef void *ghostesp_scan_t;
typedef void *ghostesp_ui_timer_t;
typedef void *ghostesp_paged_menu_t;

typedef enum {
    GHOSTESP_FONT_MICRO = 0,
    GHOSTESP_FONT_CAPTION,
    GHOSTESP_FONT_BODY,
    GHOSTESP_FONT_TITLE,
} ghostesp_font_size_t;

typedef enum {
    GHOSTESP_ALIGN_CENTER = 0,
    GHOSTESP_ALIGN_TOP_LEFT,
    GHOSTESP_ALIGN_TOP_RIGHT,
    GHOSTESP_ALIGN_BOTTOM_LEFT,
    GHOSTESP_ALIGN_BOTTOM_RIGHT,
    GHOSTESP_ALIGN_TOP_MID,
    GHOSTESP_ALIGN_BOTTOM_MID,
    GHOSTESP_ALIGN_LEFT_MID,
    GHOSTESP_ALIGN_RIGHT_MID,
    GHOSTESP_ALIGN_OUT_TOP_LEFT,
    GHOSTESP_ALIGN_OUT_TOP_MID,
    GHOSTESP_ALIGN_OUT_TOP_RIGHT,
    GHOSTESP_ALIGN_OUT_BOTTOM_LEFT,
    GHOSTESP_ALIGN_OUT_BOTTOM_MID,
    GHOSTESP_ALIGN_OUT_BOTTOM_RIGHT,
    GHOSTESP_ALIGN_OUT_LEFT_TOP,
    GHOSTESP_ALIGN_OUT_LEFT_MID,
    GHOSTESP_ALIGN_OUT_LEFT_BOTTOM,
    GHOSTESP_ALIGN_OUT_RIGHT_TOP,
    GHOSTESP_ALIGN_OUT_RIGHT_MID,
    GHOSTESP_ALIGN_OUT_RIGHT_BOTTOM,
} ghostesp_align_t;

typedef enum {
    GHOSTESP_FLEX_FLOW_NONE = 0,
    GHOSTESP_FLEX_FLOW_COLUMN,
    GHOSTESP_FLEX_FLOW_ROW,
    GHOSTESP_FLEX_FLOW_COLUMN_WRAP,
    GHOSTESP_FLEX_FLOW_ROW_WRAP,
    GHOSTESP_FLEX_FLOW_COLUMN_REVERSE,
    GHOSTESP_FLEX_FLOW_ROW_REVERSE,
    GHOSTESP_FLEX_FLOW_COLUMN_WRAP_REVERSE,
    GHOSTESP_FLEX_FLOW_ROW_WRAP_REVERSE,
} ghostesp_flex_flow_t;

typedef enum {
    GHOSTESP_FLEX_ALIGN_START = 0,
    GHOSTESP_FLEX_ALIGN_END,
    GHOSTESP_FLEX_ALIGN_CENTER,
    GHOSTESP_FLEX_ALIGN_SPACE_EVENLY,
    GHOSTESP_FLEX_ALIGN_SPACE_AROUND,
    GHOSTESP_FLEX_ALIGN_SPACE_BETWEEN,
} ghostesp_flex_align_t;

typedef struct {
    int32_t x;
    int32_t y;
} ghostesp_point_t;

typedef void (*ghostesp_ui_timer_cb_t)(void *user);
typedef void (*ghostesp_paged_menu_load_fn)(int offset, int page_size, char names[][128], bool *has_more, void *user);
typedef void (*ghostesp_paged_menu_select_fn)(const char *name, void *user);
typedef void (*ghostesp_paged_menu_nav_fn)(void *user);
typedef void (*ghostesp_anim_done_cb_t)(void *user);
typedef void (*ghostesp_input_submit_cb_t)(const char *text, void *user);
typedef void (*ghostesp_gpio_intr_cb_t)(int pin, int level, void *user);
typedef void (*ghostesp_task_fn_t)(void *user);
typedef void (*ghostesp_wifi_packet_cb_t)(const uint8_t *data, size_t len, int8_t rssi, uint8_t channel, void *user);
typedef void (*ghostesp_event_cb_t)(const char *topic, const void *data, size_t len, void *user);
typedef void *ghostesp_task_t;
typedef void *ghostesp_event_sub_t;

typedef struct {
    uint64_t size;
    bool is_directory;
} ghostesp_storage_stat_t;

#define GHOSTESP_NFC_T2_NDEF_MAX 1024

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    char model[24];
    uint16_t user_bytes;
    uint16_t ndef_length;
    bool ndef_present;
    bool read_only;
    bool password_protected;
    bool static_locked;
    bool dynamic_locked;
} ghostesp_nfc_t2_info_t;

typedef struct ghostesp_api {
    uint32_t api_version;
    size_t struct_size;
    uint32_t flags;
    const char *target;
    void (*log)(const char *message);
    void (*ui_set_title)(const char *title);
    void (*ui_print)(const char *text);
    void (*ui_clear)(void);
    void (*toast)(const char *message);
    void (*ui_show_text)(const char *title, const char *text);
    bool (*command_exec)(const char *command);
    bool (*storage_exists)(const char *path);
    int (*storage_read)(const char *path, void *buffer, size_t buffer_len);
    bool (*storage_write)(const char *path, const void *data, size_t len);
    bool (*storage_append)(const char *path, const void *data, size_t len);
    bool (*storage_delete)(const char *path);
    bool (*storage_mkdir)(const char *path);
    int (*storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);
    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
    void *(*app_malloc)(size_t size);
    void *(*app_calloc)(size_t count, size_t size);
    void (*app_free)(void *ptr);
    size_t (*app_memory_used)(void);
    size_t (*app_memory_limit)(void);
    void (*delay_ms)(uint32_t ms);
    size_t (*system_free_heap)(void);
    size_t (*system_free_internal_heap)(void);
    uint32_t (*system_uptime_ms)(void);
    const char *(*system_firmware_version)(void);
    const char *(*system_target)(void);
    bool (*wifi_start_scan)(void);
    bool (*wifi_stop_scan)(void);
    uint16_t (*wifi_ap_count)(void);
    bool (*wifi_scan_get_ap)(uint16_t index, ghostesp_wifi_ap_info_t *out);
    bool (*wifi_start_scan_async)(void);
    bool (*wifi_scan_check_done)(void);
    void (*wifi_finish_scan)(void);
    bool (*rgb_set_all)(uint8_t red, uint8_t green, uint8_t blue);
    void *(*lv_scr_act)(void);
    void *(*display_get_current_view)(void);
    void *(*raw_symbol)(const char *name);
    const char *(*app_id)(void);
    const char *(*app_data_path)(void);
    bool (*app_storage_exists)(const char *path);
    int (*app_storage_read)(const char *path, void *buffer, size_t buffer_len);
    bool (*app_storage_write)(const char *path, const void *data, size_t len);
    bool (*app_storage_append)(const char *path, const void *data, size_t len);
    bool (*app_storage_delete)(const char *path);
    bool (*app_storage_mkdir)(const char *path);
    int (*app_storage_list)(const char *path, ghostesp_storage_entry_t *out, int max_entries);
    bool (*badusb_run_script)(const char *app_relative_path);
    bool (*badusb_stop)(void);
    bool (*ir_send_file)(const char *app_relative_path);
    bool (*ir_stop)(void);
    bool (*nfc_is_available)(void);
    bool (*nfc_read_start)(void);
    bool (*nfc_stop)(void);
    bool (*ble_start_scan)(void);
    bool (*ble_stop_scan)(void);
    int (*ble_device_count)(void);
    bool (*ble_get_device)(int index, ghostesp_ble_device_info_t *out);
    bool (*subghz_is_available)(void);
    bool (*subghz_load_snapshot)(const char *app_relative_path);
    bool (*subghz_transmit_loaded)(void);
    bool (*subghz_stop)(void);
    ghostesp_ui_obj_t (*ui_screen_create)(const char *title);
    ghostesp_ui_obj_t (*ui_card_create)(ghostesp_ui_obj_t parent);
    ghostesp_ui_obj_t (*ui_label_create)(ghostesp_ui_obj_t parent, const char *text);
    ghostesp_ui_obj_t (*ui_button_create)(ghostesp_ui_obj_t parent, const char *text, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_label_set_text)(ghostesp_ui_obj_t label, const char *text);
    void (*ui_button_set_text)(ghostesp_ui_obj_t button, const char *text);
    void (*ui_obj_set_visible)(ghostesp_ui_obj_t obj, bool visible);
    void (*ui_obj_delete)(ghostesp_ui_obj_t obj);
    void (*ui_set_status)(const char *text);
    void (*ui_show_popup)(const char *title, const char *text);
    uint32_t (*ui_theme_get_background)(void);
    uint32_t (*ui_theme_get_surface)(void);
    uint32_t (*ui_theme_get_surface_alt)(void);
    uint32_t (*ui_theme_get_text)(void);
    uint32_t (*ui_theme_get_text_muted)(void);
    uint32_t (*ui_theme_get_accent)(void);
    bool (*ui_theme_is_bright)(void);
    void (*ui_obj_set_bg_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_text_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_border_color)(ghostesp_ui_obj_t obj, uint32_t hex_color);
    void (*ui_obj_set_border_width)(ghostesp_ui_obj_t obj, int32_t width);
    void (*ui_obj_set_radius)(ghostesp_ui_obj_t obj, int32_t radius);
    void (*ui_obj_set_pad)(ghostesp_ui_obj_t obj, int32_t left, int32_t right, int32_t top, int32_t bottom);
    void (*ui_obj_set_font)(ghostesp_ui_obj_t obj, ghostesp_font_size_t size);
    void (*ui_obj_set_opa)(ghostesp_ui_obj_t obj, uint8_t opa);
    void (*ui_obj_set_pos)(ghostesp_ui_obj_t obj, int32_t x, int32_t y);
    void (*ui_obj_set_size)(ghostesp_ui_obj_t obj, int32_t w, int32_t h);
    void (*ui_obj_set_width)(ghostesp_ui_obj_t obj, int32_t w);
    void (*ui_obj_set_height)(ghostesp_ui_obj_t obj, int32_t h);
    void (*ui_obj_align)(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs);
    int32_t (*ui_obj_get_width)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_height)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_x)(ghostesp_ui_obj_t obj);
    int32_t (*ui_obj_get_y)(ghostesp_ui_obj_t obj);
    void (*ui_obj_set_flex_flow)(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow);
    void (*ui_obj_set_flex_align)(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track);
    void (*ui_obj_set_flex_grow)(ghostesp_ui_obj_t obj, uint8_t grow);
    void (*ui_obj_set_pad_row)(ghostesp_ui_obj_t obj, int32_t pad);
    void (*ui_obj_set_pad_column)(ghostesp_ui_obj_t obj, int32_t pad);
    ghostesp_ui_timer_t (*ui_timer_create)(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user);
    void (*ui_timer_delete)(ghostesp_ui_timer_t timer);
    void (*ui_timer_set_interval)(ghostesp_ui_timer_t timer, uint32_t interval_ms);
    ghostesp_options_t (*ui_options_create)(const char *title);
    ghostesp_ui_obj_t (*ui_options_add_item)(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    ghostesp_ui_obj_t (*ui_options_add_back)(ghostesp_options_t opts, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_options_set_selected)(ghostesp_options_t opts, int index);
    void (*ui_options_move_selection)(ghostesp_options_t opts, int delta);
    int (*ui_options_get_selected)(ghostesp_options_t opts);
    void (*ui_options_clear)(ghostesp_options_t opts);
    void (*ui_options_destroy)(ghostesp_options_t opts);
    ghostesp_detail_t (*ui_detail_create)(const char *title);
    void (*ui_detail_add_info)(ghostesp_detail_t dv, const char *label, const char *value);
    void (*ui_detail_add_action)(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_detail_add_header)(ghostesp_detail_t dv, const char *text);
    void (*ui_detail_add_divider)(ghostesp_detail_t dv);
    ghostesp_ui_obj_t (*ui_detail_add_back)(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_detail_set_selected)(ghostesp_detail_t dv, int index);
    void (*ui_detail_move_selection)(ghostesp_detail_t dv, int delta);
    int (*ui_detail_get_selected)(ghostesp_detail_t dv);
    int (*ui_detail_get_count)(ghostesp_detail_t dv);
    void (*ui_detail_clear)(ghostesp_detail_t dv);
    void (*ui_detail_destroy)(ghostesp_detail_t dv);
    ghostesp_popup_t (*ui_popup_create)(int32_t width, int32_t height);
    void (*ui_popup_set_title)(ghostesp_popup_t p, const char *title);
    void (*ui_popup_set_body)(ghostesp_popup_t p, const char *body);
    ghostesp_ui_obj_t (*ui_popup_add_button)(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_popup_show)(ghostesp_popup_t p);
    void (*ui_popup_hide)(ghostesp_popup_t p);
    void (*ui_popup_destroy)(ghostesp_popup_t p);
    ghostesp_scan_t (*ui_scan_status_create)(const char *message);
    void (*ui_scan_status_update)(ghostesp_scan_t ss, const char *message);
    void (*ui_scan_status_set_progress)(ghostesp_scan_t ss, int current, int total);
    void (*ui_scan_status_close)(ghostesp_scan_t ss);
    ghostesp_ui_obj_t (*ui_canvas_create)(ghostesp_ui_obj_t parent, int32_t width, int32_t height);
    void (*ui_canvas_draw_rect)(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color);
    void (*ui_canvas_fill)(ghostesp_ui_obj_t canvas, uint32_t hex_color);
    void (*ui_canvas_draw_line)(ghostesp_ui_obj_t canvas, const ghostesp_point_t *points, int count, uint32_t hex_color, int32_t width);
    void (*ui_canvas_draw_arc)(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width);
    void (*ui_anim_slide_in)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms);
    void (*ui_anim_slide_out)(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user);
    void (*ui_anim_pop_in)(ghostesp_ui_obj_t obj);
    void (*ui_anim_press_pulse)(ghostesp_ui_obj_t obj);
    ghostesp_ui_obj_t (*ui_arc_create)(ghostesp_ui_obj_t parent);
    void (*ui_arc_set_value)(ghostesp_ui_obj_t arc, int32_t value);
    void (*ui_arc_set_range)(ghostesp_ui_obj_t arc, int32_t min, int32_t max);
    void (*ui_arc_set_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
    void (*ui_arc_set_bg_angles)(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
    void (*ui_arc_set_bg_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);
    void (*ui_arc_set_indicator_color)(ghostesp_ui_obj_t arc, uint32_t hex_color);
    ghostesp_ui_obj_t (*ui_line_create)(ghostesp_ui_obj_t parent);
    void (*ui_line_set_points)(ghostesp_ui_obj_t line, const ghostesp_point_t *points, int count);
    void (*ui_line_set_color)(ghostesp_ui_obj_t line, uint32_t hex_color);
    void (*ui_line_set_width)(ghostesp_ui_obj_t line, int32_t width);
    ghostesp_ui_obj_t (*ui_image_create)(ghostesp_ui_obj_t parent);
    bool (*ui_image_set_src)(ghostesp_ui_obj_t img, const char *app_relative_path);
    ghostesp_paged_menu_t (*ui_paged_menu_create)(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user);
    void (*ui_paged_menu_set_callbacks)(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select_fn, ghostesp_paged_menu_nav_fn prev_fn, ghostesp_paged_menu_nav_fn next_fn, void *user);
    void (*ui_paged_menu_reset)(ghostesp_paged_menu_t pm);
    void (*ui_paged_menu_destroy)(ghostesp_paged_menu_t pm);
    bool (*ui_paged_menu_has_prev)(ghostesp_paged_menu_t pm);
    bool (*ui_paged_menu_has_next)(ghostesp_paged_menu_t pm);
    bool (*gps_is_available)(void);
    bool (*gps_has_fix)(void);
    double (*gps_get_latitude)(void);
    double (*gps_get_longitude)(void);
    double (*gps_get_altitude)(void);
    int (*gps_get_satellites)(void);
    float (*gps_get_speed)(void);
    float (*gps_get_heading)(void);
    uint8_t (*settings_get_theme)(void);
    const char *(*settings_get_device_name)(void);
    void (*ui_input_dialog)(const char *title, const char *default_text, ghostesp_input_submit_cb_t on_submit, void *user);
    int32_t (*ui_screen_get_width)(void);
    int32_t (*ui_screen_get_height)(void);

    void (*ble_detect_start)(void);
    void (*ble_detect_stop)(void);
    bool (*ble_detect_is_active)(void);
    int (*ble_detect_count)(void);
    bool (*ble_detect_get_device)(int index, ghostesp_ble_detect_info_t *out);
    const char *(*ble_detect_type_name)(uint8_t type);
    bool (*ble_detect_start_tracking)(int index);
    bool (*ble_detect_start_airtag_spoof)(int index);
    bool (*ui_detail_step_up)(ghostesp_detail_t dv);
    bool (*ui_detail_step_down)(ghostesp_detail_t dv);
    void (*ui_detail_activate_selected)(ghostesp_detail_t dv);
    void (*app_exit)(void);

    bool (*gpio_set_mode)(int pin, uint32_t mode);
    bool (*gpio_write)(int pin, int level);
    int (*gpio_read)(int pin);
    bool (*gpio_set_pull)(int pin, bool pullup, bool pulldown);
    bool (*gpio_set_drive_strength)(int pin, int strength);
    bool (*gpio_set_intr)(int pin, int edge, ghostesp_gpio_intr_cb_t cb, void *user);
    bool (*gpio_clear_intr)(int pin);

    bool (*uart_open)(int uart_num, int tx_pin, int rx_pin, uint32_t baud);
    int (*uart_write)(int uart_num, const void *data, size_t len);
    int (*uart_read)(int uart_num, void *buffer, size_t len, uint32_t timeout_ms);
    bool (*uart_close)(int uart_num);

    bool (*i2c_probe)(uint8_t addr, uint32_t timeout_ms);
    bool (*i2c_write)(uint8_t addr, const void *data, size_t len, uint32_t timeout_ms);
    int (*i2c_read)(uint8_t addr, void *buffer, size_t len, uint32_t timeout_ms);
    bool (*i2c_write_read)(uint8_t addr, const void *tx, size_t tx_len, void *rx, size_t rx_len, uint32_t timeout_ms);

    int (*spi_open)(int host, int sclk, int miso, int mosi, int cs, uint32_t hz, int mode);
    int (*spi_transfer)(int handle, const void *tx, void *rx, size_t len);
    bool (*spi_close)(int handle);

    int (*adc_read_raw)(int channel);
    int (*adc_read_mv)(int channel);

    bool (*pwm_attach)(int pin, uint32_t freq_hz, uint8_t resolution_bits);
    bool (*pwm_write)(int pin, uint32_t duty);
    bool (*pwm_detach)(int pin);

    uint64_t (*system_uptime_us)(void);
    void (*delay_us)(uint32_t us);
    uint32_t (*random_u32)(void);
    bool (*random_bytes)(void *buffer, size_t len);

    bool (*storage_stat)(const char *path, ghostesp_storage_stat_t *out);
    int64_t (*storage_size)(const char *path);
    bool (*storage_rename)(const char *from, const char *to);
    bool (*storage_mkdir_recursive)(const char *path);
    bool (*app_storage_stat)(const char *path, ghostesp_storage_stat_t *out);
    int64_t (*app_storage_size)(const char *path);
    bool (*app_storage_rename)(const char *from, const char *to);
    bool (*app_storage_mkdir_recursive)(const char *path);

    int (*battery_percent)(void);
    int (*battery_voltage_mv)(void);
    bool (*battery_is_charging)(void);
    uint8_t (*display_get_brightness)(void);
    bool (*display_set_brightness)(uint8_t percent);
    uint32_t (*input_buttons_state)(void);

    bool (*wifi_connect)(const char *ssid, const char *password, uint32_t timeout_ms);
    bool (*wifi_disconnect)(void);
    bool (*wifi_is_connected)(void);
    int (*wifi_rssi)(void);
    bool (*wifi_ip)(char *out, size_t out_len);
    int (*http_get)(const char *url, void *buffer, size_t buffer_len, uint32_t timeout_ms);
    int (*http_post)(const char *url, const void *body, size_t body_len, void *buffer, size_t buffer_len, uint32_t timeout_ms);

    ghostesp_task_t (*task_create)(const char *name, ghostesp_task_fn_t fn, void *user, uint32_t stack_size, int priority);
    bool (*task_delete)(ghostesp_task_t task);
    void (*task_yield)(void);

    int (*tcp_connect)(const char *host, uint16_t port, uint32_t timeout_ms);
    int (*socket_send)(int sock, const void *data, size_t len);
    int (*socket_recv)(int sock, void *buffer, size_t len, uint32_t timeout_ms);
    bool (*socket_close)(int sock);
    int (*udp_open)(uint16_t local_port);
    int (*udp_send_to)(int sock, const char *host, uint16_t port, const void *data, size_t len);
    int (*udp_recv_from)(int sock, void *buffer, size_t len, char *host_out, size_t host_out_len, uint16_t *port_out, uint32_t timeout_ms);

    int64_t (*time_unix)(void);
    bool (*time_set_unix)(int64_t unix_time);
    void (*system_reboot)(void);

    bool (*wifi_set_channel)(uint8_t channel);
    uint8_t (*wifi_get_channel)(void);
    bool (*wifi_monitor_start)(ghostesp_wifi_packet_cb_t cb, void *user);
    bool (*wifi_monitor_stop)(void);
    bool (*wifi_raw_tx)(const void *data, size_t len);

    bool (*nfc_get_last_uid)(uint8_t *uid, size_t *uid_len);
    bool (*nfc_write_file)(const char *app_relative_path);
    bool (*ir_send_raw)(uint32_t carrier_hz, const uint16_t *durations, size_t count);
    bool (*ir_receive_start)(void);
    bool (*ir_receive_stop)(void);
    int (*ir_receive_read)(uint16_t *durations, size_t max_count);
    bool (*subghz_transmit_raw)(uint32_t frequency_hz, const uint16_t *durations, size_t count);
    bool (*ble_adv_start)(const uint8_t *data, size_t len);
    bool (*ble_adv_stop)(void);

    bool (*nrf24_start)(bool stream_to_peer);
    void (*nrf24_stop)(void);
    bool (*nrf24_is_running)(void);
    bool (*nrf24_is_paused)(void);
    void (*nrf24_set_paused)(bool paused);

    bool (*ble_gatt_connect)(const uint8_t mac[6]);
    bool (*ble_gatt_disconnect)(void);
    int (*ble_gatt_read)(uint16_t service_uuid, uint16_t char_uuid, void *buffer, size_t buffer_len);
    bool (*ble_gatt_write)(uint16_t service_uuid, uint16_t char_uuid, const void *data, size_t len);
    bool (*ble_gatt_server_start)(const char *name);
    bool (*ble_gatt_server_stop)(void);

    bool (*wifi_deauth)(const uint8_t bssid[6], const uint8_t sta[6], uint8_t reason);
    bool (*wifi_send_beacon)(const char *ssid, const uint8_t bssid[6], uint8_t channel);
    bool (*wifi_pcap_start)(const char *app_relative_path);
    bool (*wifi_pcap_stop)(void);

    bool (*ethernet_is_connected)(void);
    bool (*ethernet_ip)(char *out, size_t out_len);

    int (*camera_capture_jpeg)(void *buffer, size_t buffer_len);
    bool (*camera_capture_jpeg_file)(const char *app_relative_path);

    bool (*usb_hid_keyboard_send)(const char *text);
    bool (*usb_hid_mouse_move)(int dx, int dy, uint8_t buttons);

    bool (*audio_mic_is_available)(void);
    int (*audio_mic_read)(int32_t *samples, size_t max_samples, uint32_t timeout_ms);
    float (*audio_mic_rms)(const int32_t *samples, size_t count);

    bool (*zigbee_capture_start)(uint8_t channel);
    bool (*zigbee_capture_stop)(void);
    bool (*zigbee_is_capturing)(void);
    int (*zigbee_device_count)(void);

    bool (*settings_get_u8)(const char *key, uint8_t *out);
    bool (*settings_set_u8)(const char *key, uint8_t value);
    bool (*settings_get_string)(const char *key, char *out, size_t out_len);
    bool (*settings_set_string)(const char *key, const char *value);
    bool (*settings_save)(void);

    bool (*nvs_get_u32)(const char *key, uint32_t *out);
    bool (*nvs_set_u32)(const char *key, uint32_t value);
    int (*nvs_get_blob)(const char *key, void *buffer, size_t buffer_len);
    bool (*nvs_set_blob)(const char *key, const void *data, size_t len);
    bool (*nvs_delete)(const char *key);

    ghostesp_event_sub_t (*event_subscribe)(const char *topic, ghostesp_event_cb_t cb, void *user);
    bool (*event_unsubscribe)(ghostesp_event_sub_t sub);
    bool (*event_publish)(const char *topic, const void *data, size_t len);

    bool (*parser_nfc_summary)(const char *app_relative_path, char *out, size_t out_len);
    bool (*parser_ir_summary)(const char *app_relative_path, char *out, size_t out_len);
    bool (*parser_subghz_summary)(const char *app_relative_path, char *out, size_t out_len);
    ghostesp_ui_obj_t (*ui_detail_finish)(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_back, void *user);
    ghostesp_ui_obj_t (*ui_touch_bar_create)(ghostesp_ui_obj_t parent);
    ghostesp_ui_obj_t (*ui_touch_bar_add_back)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
    ghostesp_ui_obj_t (*ui_touch_bar_add_up)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
    ghostesp_ui_obj_t (*ui_touch_bar_add_down)(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
    void (*ui_obj_set_scrollable)(ghostesp_ui_obj_t obj, bool scrollable);
    void (*ui_obj_scroll_by)(ghostesp_ui_obj_t obj, int32_t dx, int32_t dy, bool animated);
    void (*ui_obj_set_scrollbar)(ghostesp_ui_obj_t obj, bool enabled);
    void (*ui_button_set_selected)(ghostesp_ui_obj_t button, bool selected);
    int32_t (*ui_screen_get_content_width)(void);
    int32_t (*ui_screen_get_content_height)(void);
    bool (*ui_screen_is_compact)(void);
    bool (*ui_has_touchscreen)(void);

    bool (*wifi_live_scan_start)(void);
    void (*wifi_live_scan_stop)(void);
    bool (*wifi_live_scan_active)(void);

    bool (*has_permission)(const char *permission);
    bool (*has_feature)(const char *feature);
    bool (*subghz_transmit_file)(const char *app_relative_path);
    void (*ble_adv_scan_start)(void);
    void (*ble_adv_scan_stop)(void);
    bool (*ble_adv_scan_active)(void);
    int (*ble_adv_scan_count)(void);
    bool (*ble_adv_scan_get)(int index, ghostesp_ble_adv_info_t *out);
    bool (*ble_adv_scan_track)(int index);
    void (*ble_adv_scan_stop_tracking)(void);
    bool (*ble_adv_scan_save_to_sd)(int index);

    bool (*nfc_t2_scan_start)(void);
    bool (*nfc_t2_scan_stop)(void);
    bool (*nfc_t2_scan_active)(void);
    bool (*nfc_t2_read)(ghostesp_nfc_t2_info_t *out_info, uint8_t *ndef_out,
                        size_t max_ndef_bytes, size_t *ndef_bytes_out);
    bool (*nfc_t2_write_ndef)(const uint8_t *ndef, size_t ndef_len);
    bool (*ui_image_set_builtin)(ghostesp_ui_obj_t img, const char *image_name);
    /* Copy a RGB565 framebuffer into a canvas, scaling with nearest-neighbor sampling. */
    bool (*ui_canvas_blit_rgb565)(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                  int32_t src_width, int32_t src_height, int32_t src_stride,
                                  int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height);
    /* True when host canvas buffers already match the screen's native RGB565 byte order. When true,
       callers can populate pixels directly in screen byte order and skip the read back/byte swap. */
    bool (*ui_canvas_is_rgb565_native_byte_order)(void);
    int (*storage_read_at)(const char *path, uint32_t offset, void *buffer, size_t buffer_len);
    int (*app_storage_read_at)(const char *path, uint32_t offset, void *buffer, size_t buffer_len);
    int (*asset_storage_read_at)(const char *path, uint32_t offset, void *buffer, size_t buffer_len);
    bool (*asset_path)(const char *path, char *out, size_t out_len);
    bool (*asset_session_begin)(void);
    void (*asset_session_end)(void);
    void (*request_exit)(void);
    bool (*input_snapshot)(ghostesp_input_snapshot_t *out);
    /* Size in bytes of a named asset, or -1 if unknown. Works whether the asset
       was extracted to the on-SD app cache or is served directly out of the
       installed .gapp archive (see asset_storage_read_at) — callers that need
       an asset's length (e.g. to discover how many numbered parts a split
       file has) should use this instead of assuming a real file exists on
       disk at asset_path()'s returned path. */
    int64_t (*asset_storage_size)(const char *path);
    /* Non-blocking variant of ui_canvas_blit_rgb565: queues the blit on the
       UI task and returns immediately, so a rendering app can start its next
       frame while the previous one is still being pushed to the display
       (blitting synchronously serializes the two and roughly halves the
       achievable frame rate).
         - `pixels` MUST stay valid and unmodified until the blit completes,
           so render into a second buffer, not the one just handed over.
         - Returns false if a queued blit is still outstanding, so wait via
           ui_canvas_blit_async_wait() before reusing the pixel buffer. */
    bool (*ui_canvas_blit_rgb565_async)(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                        int32_t src_width, int32_t src_height, int32_t src_stride,
                                        int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height);
    /* Blocks until any queued async blit has finished (or the timeout
       elapses). Returns true when no blit is outstanding — i.e. when the
       buffer passed to the last async blit is safe to overwrite. */
    bool (*ui_canvas_blit_async_wait)(uint32_t timeout_ms);
    bool (*espnow_start)(uint8_t channel);
    void (*espnow_stop)(void);
    bool (*espnow_is_active)(void);
    uint8_t (*espnow_channel)(void);
    const char *(*espnow_name)(void);
    const char *(*espnow_last_error)(void);
    bool (*espnow_announce)(void);
    int (*espnow_peer_count)(void);
    bool (*espnow_get_peer)(int index, ghostesp_espnow_peer_t *out);
    bool (*espnow_send)(const uint8_t mac[6], const char *text);
    int (*espnow_message_count)(void);
    bool (*espnow_receive)(ghostesp_espnow_message_t *out);
    /* Optional strict PSRAM allocation for large app buffers. Unlike malloc,
       this never falls back to internal RAM. */
    void *(*psram_malloc)(size_t size);
    void (*psram_free)(void *ptr);
} ghostesp_api_t;

#define GHOSTESP_API_STRUCT_SIZE_V1 sizeof(ghostesp_api_t)

typedef struct ghostesp_app {
    uint32_t api_version;
    size_t struct_size;
    const char *id;
    const char *name;
    void (*on_start)(void);
    void (*on_stop)(void);
    void (*on_input)(const ghostesp_input_event_t *event);
    void (*on_tick)(uint32_t elapsed_ms);
    void (*on_pause)(void);
    void (*on_resume)(void);
} ghostesp_app_t;

#define GHOSTESP_APP_STRUCT_SIZE_V1 sizeof(ghostesp_app_t)

#define GHOSTESP_APP_DEFINE(_id, _name, _start, _stop, _input, _tick) \
    { \
        .api_version = GHOSTESP_APP_API_VERSION, \
        .struct_size = GHOSTESP_APP_STRUCT_SIZE_V1, \
        .id = (_id), \
        .name = (_name), \
        .on_start = (_start), \
        .on_stop = (_stop), \
        .on_input = (_input), \
        .on_tick = (_tick), \
    }
