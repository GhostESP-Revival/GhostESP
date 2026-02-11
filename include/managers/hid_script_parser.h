#ifndef HID_SCRIPT_PARSER_H
#define HID_SCRIPT_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef bool (*hid_send_key_fn)(uint8_t modifiers, uint8_t keycode, void *ctx);
typedef bool (*hid_send_string_fn)(const char *text, size_t len, void *ctx);
typedef void (*hid_delay_fn)(uint32_t ms, void *ctx);
typedef bool (*hid_release_keys_fn)(void *ctx);

typedef struct {
    hid_send_key_fn     send_key;
    hid_send_string_fn  send_string;
    hid_delay_fn        delay;
    hid_release_keys_fn release_keys;
    void               *ctx;
} hid_transport_t;

// Stream-execute from file handle. Reads one line at a time, no full-file load.
int hid_script_execute_file(FILE *f, const hid_transport_t *transport);

// Execute from pre-loaded buffer (for future BadBLE/UART sources).
int hid_script_execute(char *script_buf, const hid_transport_t *transport);

// Compact ASCII->HID conversion (arithmetic ranges, ~30 bytes static)
bool hid_ascii_to_keycode(char c, uint8_t *keycode, uint8_t *modifier);

// Named key lookup ("ENTER", "UPARROW", etc.) -> HID keycode
uint8_t hid_key_name_to_keycode(const char *name);

// Set the active keyboard layout (0=US, 1=DE, 2=FR, 3=UK, 4=ES)
void hid_set_keyboard_layout(uint8_t layout);

#endif
