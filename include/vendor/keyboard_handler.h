/**
 * @file keyboard.h
 * @brief Keyboard handling for ESP-IDF in C.
 * @version 0.1
 * @date 2023-09-22
 *
 */

#pragma once

#include "driver/gpio.h"
#include "vendor/m5/m5_keyboard_def.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int x;
  int y;
} Point2D_t;

typedef struct {
    uint8_t value;
    uint8_t x_1;
    uint8_t x_2;
} Chart_t;

static const Chart_t X_map_chart[7] = {{1, 0, 1},   {2, 2, 3},  {4, 4, 5},
                                {8, 6, 7},   {16, 8, 9}, {32, 10, 11},
                                {64, 12, 13}};

typedef struct {
    const char value_first;
    const char value_second;
} KeyValue_t;

static const KeyValue_t _key_value_map[4][14] = {{{'`', '~'},
                                           {'1', '!'},
                                           {'2', '@'},
                                           {'3', '#'},
                                           {'4', '$'},
                                           {'5', '%'},
                                           {'6', '^'},
                                           {'7', '&'},
                                           {'8', '*'},
                                           {'9', '('},
                                           {'0', ')'},
                                           {'-', '_'},
                                           {'=', '+'},
                                           {KEY_BACKSPACE, KEY_BACKSPACE}},
                                          {{KEY_TAB, KEY_TAB},
                                           {'q', 'Q'},
                                           {'w', 'W'},
                                           {'e', 'E'},
                                           {'r', 'R'},
                                           {'t', 'T'},
                                           {'y', 'Y'},
                                           {'u', 'U'},
                                           {'i', 'I'},
                                           {'o', 'O'},
                                           {'p', 'P'},
                                           {'[', '{'},
                                           {']', '}'},
                                           {'\\', '|'}},
                                          {{KEY_FN, KEY_FN},
                                           {KEY_LEFT_SHIFT, KEY_LEFT_SHIFT},
                                           {'a', 'A'},
                                           {'s', 'S'},
                                           {'d', 'D'},
                                           {'f', 'F'},
                                           {'g', 'G'},
                                           {'h', 'H'},
                                           {'j', 'J'},
                                           {'k', 'K'},
                                           {'l', 'L'},
                                           {';', ':'},
                                           {'\'', '\"'},
                                           {KEY_ENTER, KEY_ENTER}},
                                          {{KEY_LEFT_CTRL, KEY_LEFT_CTRL},
                                           {KEY_OPT, KEY_OPT},
                                           {KEY_LEFT_ALT, KEY_LEFT_ALT},
                                           {'z', 'Z'},
                                           {'x', 'X'},
                                           {'c', 'C'},
                                           {'v', 'V'},
                                           {'b', 'B'},
                                           {'n', 'N'},
                                           {'m', 'M'},
                                           {',', '<'},
                                           {'.', '>'},
                                           {'/', '?'},
                                           {' ', ' '}}};

typedef struct KeysState_t KeysState_t;

struct KeysState_t {
  bool tab;
  bool fn;
  bool shift;
  bool ctrl;
  bool opt;
  bool alt;
  bool del;
  bool enter;
  bool space;
  uint8_t modifiers;

  char *word;
  size_t word_len;
  uint8_t *hid_keys;
  size_t hid_keys_len;
  uint8_t *modifier_keys;
  size_t modifier_keys_len;

  void (*reset)(struct KeysState_t *self);
};

typedef struct {
  Point2D_t *key_list_buffer;
  size_t key_list_buffer_len;
  Point2D_t *key_pos_print_keys;
  size_t key_pos_print_keys_len;
  Point2D_t *key_pos_hid_keys;
  size_t key_pos_hid_keys_len;
  Point2D_t *key_pos_modifier_keys;
  size_t key_pos_modifier_keys_len;
  KeysState_t keys_state_buffer;
  bool is_caps_locked;
  uint8_t last_key_size;
} Keyboard_t;

// Function declarations
void keyboard_init(Keyboard_t *keyboard);
void keyboard_begin(Keyboard_t *keyboard);
void keyboard_set_output(const int *pinList, size_t pinCount, uint8_t output);
uint8_t keyboard_get_input(const int *pinList, size_t pinCount);
uint8_t keyboard_get_key(const Keyboard_t *keyboard, Point2D_t keyCoor);
void keyboard_update_key_list(Keyboard_t *keyboard);
uint8_t keyboard_is_pressed(const Keyboard_t *keyboard);
bool keyboard_is_change(Keyboard_t *keyboard);
bool keyboard_is_key_pressed(const Keyboard_t *keyboard, char c);
void keyboard_update_keys_state(Keyboard_t *keyboard);

#ifdef __cplusplus
}
#endif