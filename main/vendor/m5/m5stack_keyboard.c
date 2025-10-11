#include "vendor/keyboard_handler.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "managers/display_manager.h"
#include "esp_log.h"

#define digitalWrite(pin, level) gpio_set_level((gpio_num_t)pin, level)
#define digitalRead(pin)         gpio_get_level((gpio_num_t)pin)

static const char *TAG = "m5_kbd";
    // forward declare get_key_value so it can be used throughout this file
static uint8_t get_key_value(const Point2D_t* keyCoor, bool shift, bool ctrl, bool is_caps_locked);

#ifndef CONFIG_USE_CARDPUTER_ADV
static const int output_list[] = {8, 9, 11};
static const int input_list[] = {13, 15, 3, 4, 5, 6, 7};
#else
#include "lvgl_helpers.h"
#include "lvgl_i2c/i2c_manager.h"
// minimal TCA8418 support using I2C
#define TCA8418_I2C_ADDR              0x34
#define TCA8418_REG_CFG               0x01
#define TCA8418_REG_INT_STAT          0x02
#define TCA8418_REG_KEY_LCK_EC        0x03
#define TCA8418_REG_KEY_EVENT_A       0x04
#define TCA8418_REG_KP_GPIO1          0x1D
#define TCA8418_REG_KP_GPIO2          0x1E
#define TCA8418_REG_KP_GPIO3          0x1F

static bool tca_ready = false;
// fixed-size active key buffer to avoid malloc/realloc in hot path
#define TCA_ACTIVE_KEYS_MAX 16
static Point2D_t s_active_keys[TCA_ACTIVE_KEYS_MAX];
static size_t s_active_count = 0;
static bool tca_is_shift_like_active(Keyboard_t *kb){
    for (size_t i=0;i<s_active_count;i++){
        uint8_t code = get_key_value(&s_active_keys[i], false, false, kb->is_caps_locked);
        if (code == KEY_LEFT_SHIFT || code == KEY_LEFT_ALT || code == KEY_OPT) return true;
    }
    return false;
}
// ISR -> task notification
static SemaphoreHandle_t s_tca_sem = NULL;
static TaskHandle_t s_tca_task = NULL;
// keyboard event push helper (avoid extra allocations)
static void tca_push_key_event(uint8_t key_value){
    InputEvent ev; ev.type = INPUT_TYPE_KEYBOARD; ev.data.key_value = key_value;
    xQueueSend((QueueHandle_t)input_queue, &ev, 0);
}

static inline esp_err_t tca_write_u8(uint8_t reg, uint8_t val){
    return lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, TCA8418_I2C_ADDR, reg, &val, 1);
}
static inline esp_err_t tca_read_u8(uint8_t reg, uint8_t *out){
    return lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, TCA8418_I2C_ADDR, reg, out, 1);
}
static void tca_add_active(Point2D_t p){
    for (size_t i=0;i<s_active_count;i++){
        if (s_active_keys[i].x==p.x && s_active_keys[i].y==p.y) return;
    }
    if (s_active_count < TCA_ACTIVE_KEYS_MAX) {
        s_active_keys[s_active_count++] = p;
    }
}
static void tca_remove_active(Point2D_t p){
    for (size_t i=0;i<s_active_count;i++){
        if (s_active_keys[i].x==p.x && s_active_keys[i].y==p.y){
            // compact in place
            for (size_t j = i+1; j < s_active_count; ++j) s_active_keys[j-1] = s_active_keys[j];
            s_active_count--;
            return;
        }
    }
}
typedef struct { uint8_t row; uint8_t col; bool pressed; } tca_event_t;
static tca_event_t tca_parse_event(uint8_t raw){
    tca_event_t e;
    e.pressed = (raw & 0x80) != 0; // bit7 indicates press/release
    uint16_t buf = (uint16_t)(raw & 0x7F);
    if (buf>0) buf--; // datasheet encodes 1..n
    e.row = buf / 10;
    e.col = buf % 10;
    return e;
}
static void tca_remap_to_cardputer(Point2D_t *p /* inout */, const tca_event_t *e){
    // replicate vendor remap logic
    uint8_t col = (uint8_t)(e->row * 2);
    if (e->col > 3) col++;
    uint8_t row = (uint8_t)((e->col + 4) % 4);
    p->x = col;
    p->y = row;
}

// GPIO11 interrupt handler - only signal the task
static void IRAM_ATTR tca_irq_isr(void* arg){
    BaseType_t hp = pdFALSE;
    if (s_tca_sem) xSemaphoreGiveFromISR(s_tca_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

extern Keyboard_t gkeyboard; // provided by display_manager.c
// forward declare get_key_value so ADV task can call it before its definition
static uint8_t get_key_value(const Point2D_t* keyCoor, bool shift, bool ctrl, bool is_caps_locked);
static void tca_keyboard_task(void* arg){
    const TickType_t wait_ticks = pdMS_TO_TICKS(100);
    // modifier and caps latch state (ADV-only path)
    static bool shift_down = false;
    static bool ctrl_down = false;
    static bool alt_down = false;
    static uint8_t shift_count = 0;
    static const uint8_t shift_count_before_caps = 255; // disable hold-to-caps
    static bool caps_latch = false;
    while (1){
        // wait for IRQ or timeout for low-rate poll
        if (s_tca_sem) xSemaphoreTake(s_tca_sem, wait_ticks);
        if (!tca_ready) continue;
        uint8_t ec = 0;
        if (tca_read_u8(TCA8418_REG_KEY_LCK_EC, &ec) != ESP_OK) continue;
        uint8_t count = (uint8_t)(ec & 0x1F);
        bool changed = false;
        // batch all events so modifier updates apply to co-pressed keys regardless of delivery order
        enum { BATCH_CAP = 32 };
        static tca_event_t events[BATCH_CAP];
        static Point2D_t pts[BATCH_CAP];
        static uint8_t base_codes[BATCH_CAP];
        size_t ev_idx = 0;
        while (count-- && ev_idx < BATCH_CAP){
            uint8_t raw = 0;
            if (tca_read_u8(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) break;
            events[ev_idx] = tca_parse_event(raw);
            tca_remap_to_cardputer(&pts[ev_idx], &events[ev_idx]);
            base_codes[ev_idx] = get_key_value(&pts[ev_idx], false, false, gkeyboard.is_caps_locked);
            ev_idx++;
        }

        // first pass: update modifier state for all events
        for (size_t i = 0; i < ev_idx; ++i) {
            tca_event_t *ev = &events[i];
            Point2D_t *p = &pts[i];
            uint8_t base_code = base_codes[i];
            if (ev->pressed) {
                tca_add_active(*p);
                changed = true;
                if (base_code == KEY_LEFT_SHIFT) shift_down = true;
                else if (base_code == KEY_LEFT_CTRL) ctrl_down = true;
                else if (base_code == KEY_LEFT_ALT || base_code == KEY_OPT) alt_down = true;
                if (base_code == KEY_LEFT_SHIFT || base_code == KEY_LEFT_CTRL || base_code == KEY_LEFT_ALT || base_code == KEY_OPT) {
                    ESP_LOGI(TAG, "mod press base=0x%02x s%d c%d a%d cap%d", base_code, shift_down, ctrl_down, alt_down, gkeyboard.is_caps_locked);
                }
            } else {
                tca_remove_active(*p);
                changed = true;
                if (base_code == KEY_LEFT_SHIFT) {
                    shift_down = false;
                    caps_latch = false;
                    shift_count = 0;
                } else if (base_code == KEY_LEFT_CTRL) ctrl_down = false;
                else if (base_code == KEY_LEFT_ALT || base_code == KEY_OPT) alt_down = false;
                if (base_code == KEY_LEFT_SHIFT || base_code == KEY_LEFT_CTRL || base_code == KEY_LEFT_ALT || base_code == KEY_OPT) {
                    ESP_LOGI(TAG, "mod release base=0x%02x s%d c%d a%d cap%d", base_code, shift_down, ctrl_down, alt_down, gkeyboard.is_caps_locked);
                }
            }
        }

        // reflect modifier states so subsequent key reads honor them (also consider active set)
        gkeyboard.keys_state_buffer.shift = (shift_down || alt_down || tca_is_shift_like_active(&gkeyboard));
        gkeyboard.keys_state_buffer.ctrl = ctrl_down;
        gkeyboard.keys_state_buffer.alt  = alt_down;

        // second pass: emit pressed non-modifier keys with updated modifiers applied
        for (size_t i = 0; i < ev_idx; ++i) {
            if (!events[i].pressed) continue;
            uint8_t base_code = base_codes[i];
            if (base_code == KEY_LEFT_SHIFT || base_code == KEY_LEFT_CTRL || base_code == KEY_LEFT_ALT || base_code == KEY_OPT) continue;
            uint8_t key_value = keyboard_get_key(&gkeyboard, pts[i]);
            // force letters to uppercase when any shift-like is active
            if ((gkeyboard.keys_state_buffer.shift || tca_is_shift_like_active(&gkeyboard)) && key_value >= 'a' && key_value <= 'z') {
                key_value = (uint8_t)(key_value - ('a' - 'A'));
            }
            ESP_LOGI(TAG, "press base=0x%02x char=0x%02x s%d c%d a%d cap%d (%d,%d)",
                     base_code, key_value,
                     gkeyboard.keys_state_buffer.shift, gkeyboard.keys_state_buffer.ctrl,
                     gkeyboard.keys_state_buffer.alt, gkeyboard.is_caps_locked,
                     pts[i].x, pts[i].y);
            if (key_value) {
                if (display_manager_notify_user_input()) {
                    // swallowed as wake event
                } else {
                    tca_push_key_event(key_value);
                }
            }
        }
        // clear INT status
        tca_write_u8(TCA8418_REG_INT_STAT, 0x01);
        // handle caps latch when holding SHIFT
        if (shift_down) {
            if (shift_count < 250) shift_count++; // saturate
            // disabled: no auto caps toggle on hold
        }
        (void)changed;
    }
}
#endif

// Forward declarations
static void keys_state_reset(KeysState_t* keys_state);

void keyboard_init(Keyboard_t* keyboard) {
    keyboard->key_list_buffer = NULL;
    keyboard->key_list_buffer_len = 0;
    keyboard->key_pos_print_keys = NULL;
    keyboard->key_pos_print_keys_len = 0;
    keyboard->key_pos_hid_keys = NULL;
    keyboard->key_pos_hid_keys_len = 0;
    keyboard->key_pos_modifier_keys = NULL;
    keyboard->key_pos_modifier_keys_len = 0;
    keyboard->keys_state_buffer.word = NULL;
    keyboard->keys_state_buffer.word_len = 0;
    keyboard->keys_state_buffer.hid_keys = NULL;
    keyboard->keys_state_buffer.hid_keys_len = 0;
    keyboard->keys_state_buffer.modifier_keys = NULL;
    keyboard->keys_state_buffer.modifier_keys_len = 0;
    keyboard->keys_state_buffer.reset = keys_state_reset;
    keyboard->is_caps_locked = false;
    keyboard->last_key_size = 0;
}

void keyboard_begin(Keyboard_t* keyboard) {
#ifndef CONFIG_USE_CARDPUTER_ADV
    for (size_t i = 0; i < sizeof(output_list) / sizeof(output_list[0]); ++i) {
        gpio_reset_pin((gpio_num_t)output_list[i]);
        gpio_set_direction((gpio_num_t)output_list[i], GPIO_MODE_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)output_list[i], GPIO_PULLUP_PULLDOWN);
        digitalWrite(output_list[i], 0);
    }

    for (size_t i = 0; i < sizeof(input_list) / sizeof(input_list[0]); ++i) {
        gpio_reset_pin((gpio_num_t)input_list[i]);
        gpio_set_direction((gpio_num_t)input_list[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)input_list[i], GPIO_PULLUP_ONLY);
    }

    keyboard_set_output(output_list, sizeof(output_list) / sizeof(output_list[0]), 0);
#else
    // init TCA8418 for 7x8 matrix; enable key events
    // configure matrix rows/cols
    // rows: 7 (R0..R6) -> 0x7F, cols: 8 (C0..C7) -> 0xFF, C8..C9 disabled -> 0x00
    if (tca_write_u8(TCA8418_REG_KP_GPIO1, 0x7F) == ESP_OK &&
        tca_write_u8(TCA8418_REG_KP_GPIO2, 0xFF) == ESP_OK &&
        tca_write_u8(TCA8418_REG_KP_GPIO3, 0x00) == ESP_OK) {
        // clear interrupts
        tca_write_u8(TCA8418_REG_INT_STAT, 0x01);
        // enable key event interrupt (KE_IEN) and set INT as open-drain default config
        uint8_t cfg = 0x01; // KE_IEN
        tca_write_u8(TCA8418_REG_CFG, cfg);
        tca_ready = true;
        // setup GPIO11 interrupt to wake task (no I2C in ISR)
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << 11,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE
        };
        gpio_config(&io);
        static bool isr_service_installed = false;
        if (!isr_service_installed){
            gpio_install_isr_service(0);
            isr_service_installed = true;
        }
        gpio_isr_handler_add(11, tca_irq_isr, NULL);
        if (!s_tca_sem) s_tca_sem = xSemaphoreCreateBinary();
        if (!s_tca_task){
            xTaskCreate(tca_keyboard_task, "tca_kbd_task", 2560, NULL, HARDWARE_INPUT_TASK_PRIORITY + 1, &s_tca_task);
        }
    } else {
        tca_ready = false;
    }
#endif
}

void keyboard_set_output(const int* pinList, size_t pinCount, uint8_t output) {
    output &= 0x07;
    for (size_t i = 0; i < pinCount; ++i) {
        digitalWrite(pinList[i], (output >> i) & 0x01);
    }
}

uint8_t keyboard_get_input(const int* pinList, size_t pinCount) {
    uint8_t buffer = 0x00;
    uint8_t pin_value = 0x00;

    for (size_t i = 0; i < pinCount; ++i) {
        pin_value = (digitalRead(pinList[i]) == 1) ? 0x00 : 0x01;
        pin_value <<= i;
        buffer |= pin_value;
    }

    return buffer;
}

uint8_t keyboard_get_key(const Keyboard_t* keyboard, Point2D_t keyCoor) {
    if (keyCoor.x < 0 || keyCoor.y < 0) {
        return 0;
    }
    return get_key_value(&keyCoor, keyboard->keys_state_buffer.shift,
                         keyboard->keys_state_buffer.ctrl,
                         keyboard->is_caps_locked);
}

void keyboard_update_key_list(Keyboard_t* keyboard) {
#ifndef CONFIG_USE_CARDPUTER_ADV
    // Clear current key list
    free(keyboard->key_list_buffer);
    keyboard->key_list_buffer = NULL;
    keyboard->key_list_buffer_len = 0;

    Point2D_t coor;
    uint8_t input_value = 0;

    for (int i = 0; i < 8; i++) {
        keyboard_set_output(output_list, sizeof(output_list) / sizeof(output_list[0]), i);
        input_value = keyboard_get_input(input_list, sizeof(input_list) / sizeof(input_list[0]));

        if (input_value) {
            for (int j = 0; j < 7; j++) {
                if (input_value & (1 << j)) {
                    coor.x  = (i > 3) ? X_map_chart[j].x_1 : X_map_chart[j].x_2;
                    coor.y = (i > 3) ? (i - 4) : i;

                    coor.y = -coor.y + 3;  // Adjust Y coordinate to match picture

                    keyboard->key_list_buffer_len++;
                    keyboard->key_list_buffer = realloc(keyboard->key_list_buffer,
                        keyboard->key_list_buffer_len * sizeof(Point2D_t));
                    keyboard->key_list_buffer[keyboard->key_list_buffer_len - 1] = coor;
                }
            }
        }
    }
#else
    // no-op in ADV mode; events are produced by tca_keyboard_task
    (void)keyboard;
#endif
}

uint8_t keyboard_is_pressed(const Keyboard_t* keyboard) {
    return keyboard->key_list_buffer_len;
}

bool keyboard_is_change(Keyboard_t* keyboard) {
    if (keyboard->last_key_size != keyboard->key_list_buffer_len) {
        keyboard->last_key_size = keyboard->key_list_buffer_len;
        return true;
    }
    return false;
}

bool keyboard_is_key_pressed(const Keyboard_t* keyboard, char c) {
    for (size_t i = 0; i < keyboard->key_list_buffer_len; ++i) {
        if (keyboard_get_key(keyboard, keyboard->key_list_buffer[i]) == c) {
            return true;
        }
    }
    return false;
}

void keyboard_update_keys_state(Keyboard_t* keyboard) {
    keys_state_reset(&keyboard->keys_state_buffer);

    for (size_t i = 0; i < keyboard->key_list_buffer_len; ++i) {
        Point2D_t key_pos = keyboard->key_list_buffer[i];
        uint8_t key_value = keyboard_get_key(keyboard, key_pos);

        switch (key_value) {
            case KEY_FN:
                keyboard->keys_state_buffer.fn = true;
                break;
            case KEY_LEFT_CTRL:
                keyboard->keys_state_buffer.ctrl = true;
                keyboard->keys_state_buffer.modifier_keys_len++;
                keyboard->keys_state_buffer.modifier_keys = realloc(
                    keyboard->keys_state_buffer.modifier_keys,
                    keyboard->keys_state_buffer.modifier_keys_len * sizeof(uint8_t));
                keyboard->keys_state_buffer.modifier_keys[keyboard->keys_state_buffer.modifier_keys_len - 1] = KEY_LEFT_CTRL;
                break;
            case KEY_LEFT_SHIFT:
                keyboard->keys_state_buffer.shift = true;
                keyboard->keys_state_buffer.modifier_keys_len++;
                keyboard->keys_state_buffer.modifier_keys = realloc(
                    keyboard->keys_state_buffer.modifier_keys,
                    keyboard->keys_state_buffer.modifier_keys_len * sizeof(uint8_t));
                keyboard->keys_state_buffer.modifier_keys[keyboard->keys_state_buffer.modifier_keys_len - 1] = KEY_LEFT_SHIFT;
                break;
            case KEY_LEFT_ALT:
                keyboard->keys_state_buffer.alt = true;
                keyboard->keys_state_buffer.modifier_keys_len++;
                keyboard->keys_state_buffer.modifier_keys = realloc(
                    keyboard->keys_state_buffer.modifier_keys,
                    keyboard->keys_state_buffer.modifier_keys_len * sizeof(uint8_t));
                keyboard->keys_state_buffer.modifier_keys[keyboard->keys_state_buffer.modifier_keys_len - 1] = KEY_LEFT_ALT;
                break;
            case KEY_TAB:
                keyboard->keys_state_buffer.tab = true;
                break;
            case KEY_BACKSPACE:
                keyboard->keys_state_buffer.del = true;
                break;
            case KEY_ENTER:
                keyboard->keys_state_buffer.enter = true;
                break;
            case ' ':
                keyboard->keys_state_buffer.space = true;
                break;
            default:
                keyboard->keys_state_buffer.word_len++;
                keyboard->keys_state_buffer.word = realloc(
                    keyboard->keys_state_buffer.word,
                    keyboard->keys_state_buffer.word_len * sizeof(char));
                
                char character = (keyboard->keys_state_buffer.shift || keyboard->is_caps_locked)
                                     ? get_key_value(&key_pos, true, keyboard->keys_state_buffer.ctrl, keyboard->is_caps_locked)
                                     : get_key_value(&key_pos, false, keyboard->keys_state_buffer.ctrl, keyboard->is_caps_locked);
                
                keyboard->keys_state_buffer.word[keyboard->keys_state_buffer.word_len - 1] = character;
                break;
        }
    }


    for (size_t i = 0; i < keyboard->keys_state_buffer.modifier_keys_len; ++i) {
        keyboard->keys_state_buffer.modifiers |= (1 << (keyboard->keys_state_buffer.modifier_keys[i] - 0x80));
    }
}

// Helper function to reset the state
static void keys_state_reset(KeysState_t* keys_state) {
    keys_state->tab = false;
    keys_state->fn = false;
    keys_state->shift = false;
    keys_state->ctrl = false;
    keys_state->opt = false;
    keys_state->alt = false;
    keys_state->del = false;
    keys_state->enter = false;
    keys_state->space = false;
    keys_state->modifiers = 0;
    free(keys_state->word);
    free(keys_state->hid_keys);
    free(keys_state->modifier_keys);
    keys_state->word = NULL;
    keys_state->hid_keys = NULL;
    keys_state->modifier_keys = NULL;
    keys_state->word_len = 0;
    keys_state->hid_keys_len = 0;
    keys_state->modifier_keys_len = 0;
}

static uint8_t get_key_value(const Point2D_t* keyCoor, bool shift, bool ctrl, bool is_caps_locked) {
    uint8_t base_value = _key_value_map[keyCoor->y][keyCoor->x].value_first;
    uint8_t shifted_value = _key_value_map[keyCoor->y][keyCoor->x].value_second;

    // If shift is active or caps lock applies to letters, prefer shifted/upper mapping
    if (shift) {
        // If the base is a lowercase letter but the shifted_value is not the uppercase
        // letter (some layouts keep shifted_value as punctuation), force uppercase for letters.
        if (base_value >= 'a' && base_value <= 'z') {
            return (uint8_t)(base_value - ('a' - 'A'));
        }
        return shifted_value;
    }

    if (is_caps_locked && (base_value >= 'a' && base_value <= 'z')) {
        return (uint8_t)(base_value - ('a' - 'A'));
    }

    return base_value;
}