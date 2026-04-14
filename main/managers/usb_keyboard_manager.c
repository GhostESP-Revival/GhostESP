#include "managers/usb_keyboard_manager.h"

#include "core/esp_comm_manager.h"
#include "managers/display_manager.h"
#include "managers/views/terminal_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#if CONFIG_USE_USB_KEYBOARD && CONFIG_IDF_TARGET_ESP32S3
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#define USB_KBD_EVENT_FLAG_RELEASE 0x01

static const char *TAG = "usb_kbd";

/* Local copy of HID keyboard helper logic to map keycodes to ASCII */

#define KEYBOARD_ENTER_MAIN_CHAR    '\r'
#define KEYBOARD_ENTER_LF_EXTEND    1

static const uint8_t keycode2ascii[57][2] = {
    {0, 0},                          /* HID_KEY_NO_PRESS        */
    {0, 0},                          /* HID_KEY_ROLLOVER        */
    {0, 0},                          /* HID_KEY_POST_FAIL       */
    {0, 0},                          /* HID_KEY_ERROR_UNDEFINED */
    {'a', 'A'},                      /* HID_KEY_A               */
    {'b', 'B'},                      /* HID_KEY_B               */
    {'c', 'C'},                      /* HID_KEY_C               */
    {'d', 'D'},                      /* HID_KEY_D               */
    {'e', 'E'},                      /* HID_KEY_E               */
    {'f', 'F'},                      /* HID_KEY_F               */
    {'g', 'G'},                      /* HID_KEY_G               */
    {'h', 'H'},                      /* HID_KEY_H               */
    {'i', 'I'},                      /* HID_KEY_I               */
    {'j', 'J'},                      /* HID_KEY_J               */
    {'k', 'K'},                      /* HID_KEY_K               */
    {'l', 'L'},                      /* HID_KEY_L               */
    {'m', 'M'},                      /* HID_KEY_M               */
    {'n', 'N'},                      /* HID_KEY_N               */
    {'o', 'O'},                      /* HID_KEY_O               */
    {'p', 'P'},                      /* HID_KEY_P               */
    {'q', 'Q'},                      /* HID_KEY_Q               */
    {'r', 'R'},                      /* HID_KEY_R               */
    {'s', 'S'},                      /* HID_KEY_S               */
    {'t', 'T'},                      /* HID_KEY_T               */
    {'u', 'U'},                      /* HID_KEY_U               */
    {'v', 'V'},                      /* HID_KEY_V               */
    {'w', 'W'},                      /* HID_KEY_W               */
    {'x', 'X'},                      /* HID_KEY_X               */
    {'y', 'Y'},                      /* HID_KEY_Y               */
    {'z', 'Z'},                      /* HID_KEY_Z               */
    {'1', '!'},                      /* HID_KEY_1               */
    {'2', '@'},                      /* HID_KEY_2               */
    {'3', '#'},                      /* HID_KEY_3               */
    {'4', '$'},                      /* HID_KEY_4               */
    {'5', '%'},                      /* HID_KEY_5               */
    {'6', '^'},                      /* HID_KEY_6               */
    {'7', '&'},                      /* HID_KEY_7               */
    {'8', '*'},                      /* HID_KEY_8               */
    {'9', '('},                      /* HID_KEY_9               */
    {'0', ')'},                      /* HID_KEY_0               */
    {KEYBOARD_ENTER_MAIN_CHAR, KEYBOARD_ENTER_MAIN_CHAR}, /* HID_KEY_ENTER */
    {0, 0},                          /* HID_KEY_ESC             */
    {'\b', 0},                      /* HID_KEY_DEL             */
    {0, 0},                          /* HID_KEY_TAB             */
    {' ', ' '},                      /* HID_KEY_SPACE           */
    {'-', '_'},                      /* HID_KEY_MINUS           */
    {'=', '+'},                      /* HID_KEY_EQUAL           */
    {'[', '{'},                      /* HID_KEY_OPEN_BRACKET    */
    {']', '}'},                      /* HID_KEY_CLOSE_BRACKET   */
    {'\\', '|'},                    /* HID_KEY_BACK_SLASH      */
    {'\\', '|'},                    /* HID_KEY_SHARP (Non-US)  */
    {';', ':'},                      /* HID_KEY_COLON           */
    {'\'', '"'},                   /* HID_KEY_QUOTE           */
    {'`', '~'},                      /* HID_KEY_TILDE           */
    {',', '<'},                      /* HID_KEY_LESS            */
    {'.', '>'},                      /* HID_KEY_GREATER         */
    {'/', '?'}                       /* HID_KEY_SLASH           */
};

static inline bool hid_keyboard_is_modifier_shift(uint8_t modifier) {
    if (((modifier & HID_LEFT_SHIFT) == HID_LEFT_SHIFT) ||
        ((modifier & HID_RIGHT_SHIFT) == HID_RIGHT_SHIFT)) {
        return true;
    }
    return false;
}

static inline bool hid_keyboard_get_char(uint8_t modifier,
                                         uint8_t key_code,
                                         unsigned char *key_char) {
    uint8_t mod = hid_keyboard_is_modifier_shift(modifier) ? 1 : 0;

    if (key_code >= HID_KEY_A && key_code <= HID_KEY_SLASH) {
        *key_char = keycode2ascii[key_code][mod];
    } else {
        return false;
    }

    return true;
}

typedef struct {
    uint8_t modifier;
    uint8_t key_code;
    bool    pressed;
} usb_kbd_key_event_t;

static TaskHandle_t s_usb_host_task = NULL;
static bool s_host_mode_active = false;
static volatile bool s_usb_host_ready = false;

static int usb_kbd_get_joystick_index(uint8_t key_code) {
    switch (key_code) {
        case HID_KEY_LEFT:  return 0;
        case HID_KEY_ENTER: return 1;
        case HID_KEY_UP:    return 2;
        case HID_KEY_RIGHT: return 3;
        case HID_KEY_DOWN:  return 4;
        case HID_KEY_ESC:   return 5;
        default:            return -1;
    }
}

static void usb_kbd_handle_event(const usb_kbd_key_event_t *ev) {
    if (!ev || !ev->pressed) return;

    uint8_t payload[3];
    payload[0] = 0x00;
    payload[1] = ev->modifier;
    payload[2] = ev->key_code;
    (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_KEYBOARD, payload, sizeof(payload));

    if (!input_queue) return;

    int joy_idx = usb_kbd_get_joystick_index(ev->key_code);
    if (joy_idx >= 0) {
        InputEvent ie = {0};
        ie.type = INPUT_TYPE_JOYSTICK;
        ie.data.joystick_index = joy_idx;
        ie.data.joystick_pressed = true;
        xQueueSend((QueueHandle_t)input_queue, &ie, 0);
        return;
    }

    unsigned char ch = 0;
    if (hid_keyboard_get_char(ev->modifier, ev->key_code, &ch) && ch != 0) {
        InputEvent ie = {0};
        ie.type = INPUT_TYPE_KEYBOARD;
        ie.data.key_value = (uint8_t)ch;
        xQueueSend((QueueHandle_t)input_queue, &ie, 0);
    }
}

static void usb_kbd_keyboard_report_cb(const uint8_t *data, int length) {
    if (!data || length < (int)sizeof(hid_keyboard_input_report_boot_t)) return;

    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = {0};

    const hid_keyboard_input_report_boot_t *kb = (const hid_keyboard_input_report_boot_t *)data;
    usb_kbd_key_event_t ev;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t prev = prev_keys[i];
        if (prev > HID_KEY_ERROR_UNDEFINED) {
            bool still_present = false;
            for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++) {
                if (kb->key[j] == prev) {
                    still_present = true;
                    break;
                }
            }
            if (!still_present) {
                ev.modifier = 0;
                ev.key_code = prev;
                ev.pressed = false;
                usb_kbd_handle_event(&ev);
            }
        }

        uint8_t now = kb->key[i];
        if (now > HID_KEY_ERROR_UNDEFINED) {
            bool was_present = false;
            for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++) {
                if (prev_keys[j] == now) {
                    was_present = true;
                    break;
                }
            }
            if (!was_present) {
                ev.modifier = kb->modifier.val;
                ev.key_code = now;
                ev.pressed = true;
                usb_kbd_handle_event(&ev);
            }
        }
    }

    memcpy(prev_keys, kb->key, HID_KEYBOARD_KEY_MAX);
}

static void usb_kbd_interface_cb(hid_host_device_handle_t hid_dev, const hid_host_interface_event_t event, void *arg) {
    (void)arg;
    uint8_t buf[64];
    size_t len = 0;
    hid_host_dev_params_t params;

    if (hid_host_device_get_params(hid_dev, &params) != ESP_OK) {
        return;
    }

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(hid_dev, buf, sizeof(buf), &len) == ESP_OK) {
            if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && params.proto == HID_PROTOCOL_KEYBOARD) {
                usb_kbd_keyboard_report_cb(buf, (int)len);
            }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID keyboard disconnected");
        hid_host_device_close(hid_dev);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGE(TAG, "HID transfer error!");
        break;
    default:
        break;
    }
}

static void usb_kbd_device_cb(hid_host_device_handle_t hid_dev, const hid_host_driver_event_t event, void *arg) {
    (void)arg;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(hid_dev, &params) != ESP_OK) {
        return;
    }

    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && params.proto == HID_PROTOCOL_KEYBOARD) {
            const hid_host_device_config_t cfg = {
                .callback = usb_kbd_interface_cb,
                .callback_arg = NULL,
            };
            if (hid_host_device_open(hid_dev, &cfg) == ESP_OK) {
                if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                    hid_class_request_set_protocol(hid_dev, HID_REPORT_PROTOCOL_BOOT);
                    if (params.proto == HID_PROTOCOL_KEYBOARD) {
                        hid_class_request_set_idle(hid_dev, 0, 0);
                    }
                }
                hid_host_device_start(hid_dev);
                ESP_LOGI(TAG, "HID keyboard connected");
            } else {
                ESP_LOGE(TAG, "Failed to open HID device");
            }
        }
    }
}

static void usb_kbd_host_task(void *arg) {
    (void)arg;
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    if (usb_host_install(&cfg) != ESP_OK) {
        TERMINAL_VIEW_ADD_TEXT("usb_host_install failed\n");
        s_usb_host_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_usb_host_ready = true;
    TERMINAL_VIEW_ADD_TEXT("USB host ready\n");

    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        (void)flags;
    }
}

void usb_keyboard_manager_init(void) {
    if (s_host_mode_active) {
        TERMINAL_VIEW_ADD_TEXT("USB Host already active\n");
        return;
    }

    if (!s_usb_host_task) {
        s_usb_host_ready = false;
        if (xTaskCreatePinnedToCore(usb_kbd_host_task, "usb_events", 6144, NULL, 2, &s_usb_host_task, 0) != pdPASS) {
            TERMINAL_VIEW_ADD_TEXT("USB host task failed\n");
            s_usb_host_task = NULL;
            return;
        }
    }

    int wait_count = 0;
    while (!s_usb_host_ready && s_usb_host_task && wait_count < 50) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_count++;
    }
    if (!s_usb_host_ready) {
        TERMINAL_VIEW_ADD_TEXT("USB host init timeout\n");
        return;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 6144,
        .core_id = 0,
        .callback = usb_kbd_device_cb,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        TERMINAL_VIEW_ADD_TEXT("HID install fail: %s\n", esp_err_to_name(err));
    } else {
        s_host_mode_active = true;
        TERMINAL_VIEW_ADD_TEXT("USB Host enabled\n");
    }
}

static void usb_kbd_stop(void) {
    if (!s_host_mode_active) return;
    
    hid_host_uninstall();
    
    if (s_usb_host_task) {
        usb_host_uninstall();
        vTaskDelete(s_usb_host_task);
        s_usb_host_task = NULL;
    }
    
    s_usb_host_ready = false;
    s_host_mode_active = false;
    TERMINAL_VIEW_ADD_TEXT("USB Host disabled\n");
}

static void usb_kbd_stream_rx_cb(uint8_t channel, const uint8_t* data, size_t length, void* user_data) {
    (void)channel;
    (void)user_data;
    if (!data || length < 3 || !input_queue) return;
    if (data[0] & USB_KBD_EVENT_FLAG_RELEASE) return;
    
    uint8_t key_code = data[2];

    int joy_idx = usb_kbd_get_joystick_index(key_code);
    if (joy_idx >= 0) {
        InputEvent ie = {0};
        ie.type = INPUT_TYPE_JOYSTICK;
        ie.data.joystick_index = joy_idx;
        ie.data.joystick_pressed = true;
        xQueueSend((QueueHandle_t)input_queue, &ie, 0);
        return;
    }

    unsigned char ch = 0;
    if (hid_keyboard_get_char(data[1], key_code, &ch) && ch != 0) {
        InputEvent ie = {0};
        ie.type = INPUT_TYPE_KEYBOARD;
        ie.data.key_value = (uint8_t)ch;
        xQueueSend((QueueHandle_t)input_queue, &ie, 0);
    }
}

bool usb_keyboard_manager_is_host_mode(void) {
    return s_host_mode_active;
}

void usb_keyboard_manager_set_host_mode(bool enable) {
    if (enable && !s_host_mode_active) {
        usb_keyboard_manager_init();
    } else if (!enable && s_host_mode_active) {
        usb_kbd_stop();
    }
}

void usb_keyboard_manager_register_stream_handler(void) {
    bool ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_KEYBOARD, usb_kbd_stream_rx_cb, NULL);
    TERMINAL_VIEW_ADD_TEXT("KBD stream handler(S3): %s\n", ok ? "OK" : "FAIL");
}

#else

void usb_keyboard_manager_init(void) {
}

bool usb_keyboard_manager_is_host_mode(void) {
    return false;
}

void usb_keyboard_manager_set_host_mode(bool enable) {
    (void)enable;
}

#define HID_KEY_A     0x04
#define HID_KEY_SLASH 0x38
#define HID_KEY_LEFT  0x50
#define HID_KEY_UP    0x52
#define HID_KEY_RIGHT 0x4F
#define HID_KEY_DOWN  0x51
#define HID_KEY_ENTER 0x28
#define HID_KEY_ESC   0x29
#define HID_LEFT_SHIFT  (1 << 1)
#define HID_RIGHT_SHIFT (1 << 5)

static const uint8_t keycode2ascii_simple[57][2] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'}, {'f', 'F'},
    {'g', 'G'}, {'h', 'H'}, {'i', 'I'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'},
    {'m', 'M'}, {'n', 'N'}, {'o', 'O'}, {'p', 'P'}, {'q', 'Q'}, {'r', 'R'},
    {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'}, {'x', 'X'},
    {'y', 'Y'}, {'z', 'Z'}, {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'},
    {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    {'\r', '\r'}, {0, 0}, {'\b', 0}, {0, 0}, {' ', ' '}, {'-', '_'},
    {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'}, {'\\', '|'},
    {';', ':'}, {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'}, {'/', '?'}
};

static void usb_kbd_stream_rx_cb_simple(uint8_t channel, const uint8_t* data, size_t length, void* user_data) {
    (void)channel;
    (void)user_data;
    if (!data || length < 3 || !input_queue) return;
    if (data[0] & 0x01) return;
    
    uint8_t modifier = data[1];
    uint8_t key_code = data[2];
    
    int joy_idx = -1;
    switch (key_code) {
        case HID_KEY_LEFT:  joy_idx = 0; break;
        case HID_KEY_ENTER: joy_idx = 1; break;
        case HID_KEY_UP:    joy_idx = 2; break;
        case HID_KEY_RIGHT: joy_idx = 3; break;
        case HID_KEY_DOWN:  joy_idx = 4; break;
        case HID_KEY_ESC:   joy_idx = 5; break;
    }
    
    if (joy_idx >= 0) {
        InputEvent ie = {0};
        ie.type = INPUT_TYPE_JOYSTICK;
        ie.data.joystick_index = joy_idx;
        ie.data.joystick_pressed = true;
        xQueueSend((QueueHandle_t)input_queue, &ie, 0);
        return;
    }
    
    if (key_code >= HID_KEY_A && key_code <= HID_KEY_SLASH) {
        uint8_t shift = ((modifier & HID_LEFT_SHIFT) || (modifier & HID_RIGHT_SHIFT)) ? 1 : 0;
        uint8_t ch = keycode2ascii_simple[key_code][shift];
        if (ch != 0) {
            InputEvent ie = {0};
            ie.type = INPUT_TYPE_KEYBOARD;
            ie.data.key_value = ch;
            xQueueSend((QueueHandle_t)input_queue, &ie, 0);
        }
    }
}

void usb_keyboard_manager_register_stream_handler(void) {
    bool ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_KEYBOARD, usb_kbd_stream_rx_cb_simple, NULL);
    TERMINAL_VIEW_ADD_TEXT("KBD stream handler: %s\n", ok ? "OK" : "FAIL");
}

#endif
