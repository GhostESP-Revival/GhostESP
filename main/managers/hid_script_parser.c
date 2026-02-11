#include "sdkconfig.h"

#ifdef CONFIG_HAS_BADUSB

#include "managers/hid_script_parser.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define HID_KEY_NONE        0x00
#define HID_KEY_A           0x04
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_2           0x1F
#define HID_KEY_3           0x20
#define HID_KEY_4           0x21
#define HID_KEY_5           0x22
#define HID_KEY_6           0x23
#define HID_KEY_7           0x24
#define HID_KEY_8           0x25
#define HID_KEY_9           0x26
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESCAPE      0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_MINUS       0x2D
#define HID_KEY_EQUAL       0x2E
#define HID_KEY_LBRACKET    0x2F
#define HID_KEY_RBRACKET    0x30
#define HID_KEY_BACKSLASH   0x31
#define HID_KEY_SEMICOLON   0x33
#define HID_KEY_APOSTROPHE  0x34
#define HID_KEY_GRAVE       0x35
#define HID_KEY_COMMA       0x36
#define HID_KEY_PERIOD      0x37
#define HID_KEY_SLASH       0x38
#define HID_KEY_CAPSLOCK    0x39
#define HID_KEY_F1          0x3A
#define HID_KEY_F2          0x3B
#define HID_KEY_F3          0x3C
#define HID_KEY_F4          0x3D
#define HID_KEY_F5          0x3E
#define HID_KEY_F6          0x3F
#define HID_KEY_F7          0x40
#define HID_KEY_F8          0x41
#define HID_KEY_F9          0x42
#define HID_KEY_F10         0x43
#define HID_KEY_F11         0x44
#define HID_KEY_F12         0x45
#define HID_KEY_PRINTSCREEN 0x46
#define HID_KEY_SCROLLLOCK  0x47
#define HID_KEY_PAUSE       0x48
#define HID_KEY_INSERT      0x49
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_DELETE      0x4C
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52
#define HID_KEY_NUMLOCK     0x53
#define HID_KEY_MENU        0x65

#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_LGUI    0x08
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_RALT    0x40
#define HID_MOD_RGUI    0x80

static uint8_t s_active_layout = 0;

void hid_set_keyboard_layout(uint8_t layout) {
    s_active_layout = layout;
}

typedef struct {
    char ch;
    uint8_t keycode;
    uint8_t modifier;
} special_char_map_t;

static const special_char_map_t special_chars_us[] = {
    {' ',  HID_KEY_SPACE,      0},
    {'!',  HID_KEY_1,          HID_MOD_LSHIFT},
    {'@',  HID_KEY_2,          HID_MOD_LSHIFT},
    {'#',  HID_KEY_3,          HID_MOD_LSHIFT},
    {'$',  HID_KEY_4,          HID_MOD_LSHIFT},
    {'%',  HID_KEY_5,          HID_MOD_LSHIFT},
    {'^',  HID_KEY_6,          HID_MOD_LSHIFT},
    {'&',  HID_KEY_7,          HID_MOD_LSHIFT},
    {'*',  HID_KEY_8,          HID_MOD_LSHIFT},
    {'(',  HID_KEY_9,          HID_MOD_LSHIFT},
    {')',  HID_KEY_0,          HID_MOD_LSHIFT},
    {'-',  HID_KEY_MINUS,      0},
    {'_',  HID_KEY_MINUS,      HID_MOD_LSHIFT},
    {'=',  HID_KEY_EQUAL,      0},
    {'+',  HID_KEY_EQUAL,      HID_MOD_LSHIFT},
    {'[',  HID_KEY_LBRACKET,   0},
    {'{',  HID_KEY_LBRACKET,   HID_MOD_LSHIFT},
    {']',  HID_KEY_RBRACKET,   0},
    {'}',  HID_KEY_RBRACKET,   HID_MOD_LSHIFT},
    {'\\', HID_KEY_BACKSLASH,  0},
    {'|',  HID_KEY_BACKSLASH,  HID_MOD_LSHIFT},
    {';',  HID_KEY_SEMICOLON,  0},
    {':',  HID_KEY_SEMICOLON,  HID_MOD_LSHIFT},
    {'\'', HID_KEY_APOSTROPHE, 0},
    {'"',  HID_KEY_APOSTROPHE, HID_MOD_LSHIFT},
    {'`',  HID_KEY_GRAVE,      0},
    {'~',  HID_KEY_GRAVE,      HID_MOD_LSHIFT},
    {',',  HID_KEY_COMMA,      0},
    {'<',  HID_KEY_COMMA,      HID_MOD_LSHIFT},
    {'.',  HID_KEY_PERIOD,     0},
    {'>',  HID_KEY_PERIOD,     HID_MOD_LSHIFT},
    {'/',  HID_KEY_SLASH,      0},
    {'?',  HID_KEY_SLASH,      HID_MOD_LSHIFT},
    {'\n', HID_KEY_ENTER,      0},
    {'\t', HID_KEY_TAB,        0},
};
#define NUM_SPECIAL_US (sizeof(special_chars_us) / sizeof(special_chars_us[0]))

static const special_char_map_t special_chars_de[] = {
    {' ',  HID_KEY_SPACE,      0},
    {'!',  HID_KEY_1,          HID_MOD_LSHIFT},
    {'"',  HID_KEY_2,          HID_MOD_LSHIFT},
    {'#',  HID_KEY_BACKSLASH,  0},            // # is on its own key (key to left of Enter)
    {'$',  HID_KEY_4,          HID_MOD_LSHIFT},
    {'%',  HID_KEY_5,          HID_MOD_LSHIFT},
    {'&',  HID_KEY_6,          HID_MOD_LSHIFT},
    {'/',  HID_KEY_7,          HID_MOD_LSHIFT},
    {'(',  HID_KEY_8,          HID_MOD_LSHIFT},
    {')',  HID_KEY_9,          HID_MOD_LSHIFT},
    {'=',  HID_KEY_0,          HID_MOD_LSHIFT},
    {'?',  HID_KEY_MINUS,      HID_MOD_LSHIFT}, // ß? key
    {'\'', HID_KEY_BACKSLASH,  HID_MOD_LSHIFT},
    {'+',  HID_KEY_RBRACKET,   0},             // +* key
    {'*',  HID_KEY_RBRACKET,   HID_MOD_LSHIFT},
    {'-',  HID_KEY_SLASH,      0},             // -_ key (right of 0)
    {'_',  HID_KEY_SLASH,      HID_MOD_LSHIFT},
    {'.',  HID_KEY_PERIOD,     0},
    {':',  HID_KEY_PERIOD,     HID_MOD_LSHIFT},
    {',',  HID_KEY_COMMA,      0},
    {';',  HID_KEY_COMMA,      HID_MOD_LSHIFT},
    {'<',  0x64,               0},             // < > key (key 102, non-US backslash)
    {'>',  0x64,               HID_MOD_LSHIFT},
    {'@',  HID_KEY_2,          HID_MOD_RALT},  // AltGr+2
    {'[',  HID_KEY_8,          HID_MOD_RALT},  // AltGr+8
    {']',  HID_KEY_9,          HID_MOD_RALT},  // AltGr+9
    {'{',  HID_KEY_7,          HID_MOD_RALT},  // AltGr+7
    {'}',  HID_KEY_0,          HID_MOD_RALT},  // AltGr+0
    {'\\', HID_KEY_MINUS,      HID_MOD_RALT},  // AltGr+ß
    {'~',  HID_KEY_RBRACKET,   HID_MOD_RALT},  // AltGr++
    {'|',  0x64,               HID_MOD_RALT},  // AltGr+<
    {'`',  HID_KEY_EQUAL,      HID_MOD_LSHIFT}, // ´` key (accent)
    {'^',  HID_KEY_GRAVE,      0},             // ^° key
    {'\n', HID_KEY_ENTER,      0},
    {'\t', HID_KEY_TAB,        0},
};
#define NUM_SPECIAL_DE (sizeof(special_chars_de) / sizeof(special_chars_de[0]))

static const special_char_map_t special_chars_fr[] = {
    {' ',  HID_KEY_SPACE,      0},
    {'&',  HID_KEY_1,          0},             // unshifted 1 = &
    {'~',  HID_KEY_2,          HID_MOD_RALT},  // AltGr+2 = ~
    {'"',  HID_KEY_3,          0},             // unshifted 3 = "
    {'\'', HID_KEY_4,          0},             // unshifted 4 = '
    {'(',  HID_KEY_5,          0},             // unshifted 5 = (
    {'-',  HID_KEY_6,          0},             // unshifted 6 = -
    {'_',  HID_KEY_8,          0},             // unshifted 8 = _
    {')',  HID_KEY_MINUS,      0},             // ) key
    {'=',  HID_KEY_SLASH,      0},             // = key
    {'!',  HID_KEY_SLASH,      HID_MOD_LSHIFT},
    {'$',  HID_KEY_RBRACKET,   0},
    {'*',  HID_KEY_BACKSLASH,  0},
    {',',  0x10,               0},             // M key position in AZERTY = comma
    {';',  HID_KEY_COMMA,      0},             // semicolon is unshifted comma pos
    {':',  HID_KEY_PERIOD,     0},             // colon is unshifted period pos
    {'.',  HID_KEY_COMMA,      HID_MOD_LSHIFT}, // period is Shift+comma pos
    {'/',  HID_KEY_PERIOD,     HID_MOD_LSHIFT}, // slash is Shift+period pos
    {'?',  0x10,               HID_MOD_LSHIFT}, // ? is Shift+M pos
    {'#',  HID_KEY_3,          HID_MOD_RALT},  // AltGr+3 = #
    {'@',  HID_KEY_0,          HID_MOD_RALT},  // AltGr+0 = @
    {'[',  HID_KEY_5,          HID_MOD_RALT},  // AltGr+5 = [
    {']',  HID_KEY_MINUS,      HID_MOD_RALT},  // AltGr+) = ]
    {'{',  HID_KEY_4,          HID_MOD_RALT},  // AltGr+' = {
    {'}',  HID_KEY_EQUAL,      HID_MOD_RALT},  // AltGr+= = }
    {'\\', HID_KEY_8,          HID_MOD_RALT},  // AltGr+8 = backslash
    {'|',  HID_KEY_6,          HID_MOD_RALT},  // AltGr+6 = |
    {'^',  HID_KEY_LBRACKET,   0},             // ^ key (dead key used as direct)
    {'<',  0x64,               0},             // non-US key
    {'>',  0x64,               HID_MOD_LSHIFT},
    {'+',  HID_KEY_EQUAL,      HID_MOD_LSHIFT},
    {'`',  HID_KEY_7,          HID_MOD_RALT},  // AltGr+7 = `
    {'\n', HID_KEY_ENTER,      0},
    {'\t', HID_KEY_TAB,        0},
};
#define NUM_SPECIAL_FR (sizeof(special_chars_fr) / sizeof(special_chars_fr[0]))

static const special_char_map_t special_chars_uk[] = {
    {' ',  HID_KEY_SPACE,      0},
    {'!',  HID_KEY_1,          HID_MOD_LSHIFT},
    {'"',  HID_KEY_2,          HID_MOD_LSHIFT}, // UK: Shift+2 = " (not @)
    {'#',  HID_KEY_BACKSLASH,  0},              // UK: # is on the key next to Enter (0x32 maps to BACKSLASH in US pos)
    {'$',  HID_KEY_4,          HID_MOD_LSHIFT},
    {'%',  HID_KEY_5,          HID_MOD_LSHIFT},
    {'^',  HID_KEY_6,          HID_MOD_LSHIFT},
    {'&',  HID_KEY_7,          HID_MOD_LSHIFT},
    {'*',  HID_KEY_8,          HID_MOD_LSHIFT},
    {'(',  HID_KEY_9,          HID_MOD_LSHIFT},
    {')',  HID_KEY_0,          HID_MOD_LSHIFT},
    {'-',  HID_KEY_MINUS,      0},
    {'_',  HID_KEY_MINUS,      HID_MOD_LSHIFT},
    {'=',  HID_KEY_EQUAL,      0},
    {'+',  HID_KEY_EQUAL,      HID_MOD_LSHIFT},
    {'[',  HID_KEY_LBRACKET,   0},
    {'{',  HID_KEY_LBRACKET,   HID_MOD_LSHIFT},
    {']',  HID_KEY_RBRACKET,   0},
    {'}',  HID_KEY_RBRACKET,   HID_MOD_LSHIFT},
    {'\\', 0x64,               0},              // UK: \ is on key 102 (non-US backslash)
    {'|',  0x64,               HID_MOD_LSHIFT},
    {';',  HID_KEY_SEMICOLON,  0},
    {':',  HID_KEY_SEMICOLON,  HID_MOD_LSHIFT},
    {'\'', HID_KEY_APOSTROPHE, 0},
    {'@',  HID_KEY_APOSTROPHE, HID_MOD_LSHIFT}, // UK: Shift+' = @
    {'`',  HID_KEY_GRAVE,      0},
    {'~',  HID_KEY_BACKSLASH,  HID_MOD_LSHIFT}, // UK: Shift+# = ~
    {',',  HID_KEY_COMMA,      0},
    {'<',  HID_KEY_COMMA,      HID_MOD_LSHIFT},
    {'.',  HID_KEY_PERIOD,     0},
    {'>',  HID_KEY_PERIOD,     HID_MOD_LSHIFT},
    {'/',  HID_KEY_SLASH,      0},
    {'?',  HID_KEY_SLASH,      HID_MOD_LSHIFT},
    {'\n', HID_KEY_ENTER,      0},
    {'\t', HID_KEY_TAB,        0},
};
#define NUM_SPECIAL_UK (sizeof(special_chars_uk) / sizeof(special_chars_uk[0]))

static const special_char_map_t special_chars_es[] = {
    {' ',  HID_KEY_SPACE,      0},
    {'!',  HID_KEY_1,          HID_MOD_LSHIFT},
    {'"',  HID_KEY_2,          HID_MOD_LSHIFT},
    {'#',  HID_KEY_3,          HID_MOD_RALT},  // AltGr+3
    {'$',  HID_KEY_4,          HID_MOD_LSHIFT},
    {'%',  HID_KEY_5,          HID_MOD_LSHIFT},
    {'&',  HID_KEY_6,          HID_MOD_LSHIFT},
    {'/',  HID_KEY_7,          HID_MOD_LSHIFT},
    {'(',  HID_KEY_8,          HID_MOD_LSHIFT},
    {')',  HID_KEY_9,          HID_MOD_LSHIFT},
    {'=',  HID_KEY_0,          HID_MOD_LSHIFT},
    {'\'', HID_KEY_MINUS,      0},             // ' key
    {'?',  HID_KEY_MINUS,      HID_MOD_LSHIFT},
    {'+',  HID_KEY_RBRACKET,   0},
    {'*',  HID_KEY_RBRACKET,   HID_MOD_LSHIFT},
    {'-',  HID_KEY_SLASH,      0},
    {'_',  HID_KEY_SLASH,      HID_MOD_LSHIFT},
    {'.',  HID_KEY_PERIOD,     0},
    {':',  HID_KEY_PERIOD,     HID_MOD_LSHIFT},
    {',',  HID_KEY_COMMA,      0},
    {';',  HID_KEY_COMMA,      HID_MOD_LSHIFT},
    {'<',  0x64,               0},
    {'>',  0x64,               HID_MOD_LSHIFT},
    {'@',  HID_KEY_2,          HID_MOD_RALT},  // AltGr+2
    {'[',  HID_KEY_LBRACKET,   HID_MOD_RALT},  // AltGr+`
    {']',  HID_KEY_RBRACKET,   HID_MOD_RALT},  // AltGr++
    {'{',  HID_KEY_APOSTROPHE, HID_MOD_RALT},  // AltGr+´
    {'}',  HID_KEY_BACKSLASH,  HID_MOD_RALT},  // AltGr+ç
    {'\\', HID_KEY_GRAVE,      HID_MOD_RALT},  // AltGr+º
    {'|',  HID_KEY_1,          HID_MOD_RALT},  // AltGr+1
    {'~',  HID_KEY_4,          HID_MOD_RALT},  // AltGr+4
    {'^',  HID_KEY_LBRACKET,   HID_MOD_LSHIFT}, // Shift+` (dead key)
    {'`',  HID_KEY_LBRACKET,   0},             // ` key (dead key)
    {'\n', HID_KEY_ENTER,      0},
    {'\t', HID_KEY_TAB,        0},
};
#define NUM_SPECIAL_ES (sizeof(special_chars_es) / sizeof(special_chars_es[0]))

bool hid_ascii_to_keycode(char c, uint8_t *keycode, uint8_t *modifier) {
    *modifier = 0;

    const special_char_map_t *smap;
    size_t smap_count;
    switch (s_active_layout) {
        case 1: smap = special_chars_de; smap_count = NUM_SPECIAL_DE; break;
        case 2: smap = special_chars_fr; smap_count = NUM_SPECIAL_FR; break;
        case 3: smap = special_chars_uk; smap_count = NUM_SPECIAL_UK; break;
        case 4: smap = special_chars_es; smap_count = NUM_SPECIAL_ES; break;
        default: smap = special_chars_us; smap_count = NUM_SPECIAL_US; break;
    }

    if (c >= 'a' && c <= 'z') {
        uint8_t base = c - 'a';
        if (s_active_layout == 1) {
            if (c == 'y') base = 'z' - 'a';
            else if (c == 'z') base = 'y' - 'a';
        }
        else if (s_active_layout == 2) {
            if (c == 'a') base = 'q' - 'a';
            else if (c == 'q') base = 'a' - 'a';
            else if (c == 'z') base = 'w' - 'a';
            else if (c == 'w') base = 'z' - 'a';
            else if (c == 'm') { *keycode = HID_KEY_SEMICOLON; return true; }
        }
        *keycode = HID_KEY_A + base;
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        uint8_t base = c - 'A';
        if (s_active_layout == 1) {
            if (c == 'Y') base = 'Z' - 'A';
            else if (c == 'Z') base = 'Y' - 'A';
        }
        else if (s_active_layout == 2) {
            if (c == 'A') base = 'Q' - 'A';
            else if (c == 'Q') base = 'A' - 'A';
            else if (c == 'Z') base = 'W' - 'A';
            else if (c == 'W') base = 'Z' - 'A';
            else if (c == 'M') { *keycode = HID_KEY_SEMICOLON; *modifier = HID_MOD_LSHIFT; return true; }
        }
        *keycode = HID_KEY_A + base;
        *modifier = HID_MOD_LSHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *keycode = HID_KEY_1 + (c - '1');
        if (s_active_layout == 2) *modifier = HID_MOD_LSHIFT;
        return true;
    }
    if (c == '0') {
        *keycode = HID_KEY_0;
        if (s_active_layout == 2) *modifier = HID_MOD_LSHIFT;
        return true;
    }
    for (size_t i = 0; i < smap_count; i++) {
        if (smap[i].ch == c) {
            *keycode = smap[i].keycode;
            *modifier = smap[i].modifier;
            return true;
        }
    }
    return false;
}

typedef struct {
    const char *name;
    uint8_t keycode;
} key_name_entry_t;

static const key_name_entry_t key_names[] = {
    {"APP",          HID_KEY_MENU},
    {"BREAK",        HID_KEY_PAUSE},
    {"CAPSLOCK",     HID_KEY_CAPSLOCK},
    {"DELETE",       HID_KEY_DELETE},
    {"DOWN",         HID_KEY_DOWN},
    {"DOWNARROW",    HID_KEY_DOWN},
    {"END",          HID_KEY_END},
    {"ENTER",        HID_KEY_ENTER},
    {"ESC",          HID_KEY_ESCAPE},
    {"ESCAPE",       HID_KEY_ESCAPE},
    {"F1",           HID_KEY_F1},
    {"F10",          HID_KEY_F10},
    {"F11",          HID_KEY_F11},
    {"F12",          HID_KEY_F12},
    {"F2",           HID_KEY_F2},
    {"F3",           HID_KEY_F3},
    {"F4",           HID_KEY_F4},
    {"F5",           HID_KEY_F5},
    {"F6",           HID_KEY_F6},
    {"F7",           HID_KEY_F7},
    {"F8",           HID_KEY_F8},
    {"F9",           HID_KEY_F9},
    {"HOME",         HID_KEY_HOME},
    {"INSERT",       HID_KEY_INSERT},
    {"LEFT",         HID_KEY_LEFT},
    {"LEFTARROW",    HID_KEY_LEFT},
    {"MENU",         HID_KEY_MENU},
    {"NUMLOCK",      HID_KEY_NUMLOCK},
    {"PAGEDOWN",     HID_KEY_PAGEDOWN},
    {"PAGEUP",       HID_KEY_PAGEUP},
    {"PAUSE",        HID_KEY_PAUSE},
    {"PRINTSCREEN",  HID_KEY_PRINTSCREEN},
    {"RIGHT",        HID_KEY_RIGHT},
    {"RIGHTARROW",   HID_KEY_RIGHT},
    {"SCROLLLOCK",   HID_KEY_SCROLLLOCK},
    {"SPACE",        HID_KEY_SPACE},
    {"TAB",          HID_KEY_TAB},
    {"UP",           HID_KEY_UP},
    {"UPARROW",      HID_KEY_UP},
};

#define NUM_KEY_NAMES (sizeof(key_names) / sizeof(key_names[0]))

static int key_name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, ((const key_name_entry_t *)b)->name);
}

uint8_t hid_key_name_to_keycode(const char *name) {
    const key_name_entry_t *entry = bsearch(name, key_names, NUM_KEY_NAMES,
                                             sizeof(key_name_entry_t), key_name_cmp);
    return entry ? entry->keycode : HID_KEY_NONE;
}

static void strip_trailing(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                        line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

static uint8_t parse_modifier(const char *name) {
    if (strcmp(name, "CTRL") == 0 || strcmp(name, "CONTROL") == 0) return HID_MOD_LCTRL;
    if (strcmp(name, "SHIFT") == 0) return HID_MOD_LSHIFT;
    if (strcmp(name, "ALT") == 0) return HID_MOD_LALT;
    if (strcmp(name, "GUI") == 0 || strcmp(name, "WINDOWS") == 0) return HID_MOD_LGUI;
    return 0;
}

typedef enum {
    ACTION_NONE,
    ACTION_KEY,
    ACTION_STRING,
    ACTION_DELAY,
} action_type_t;

typedef struct {
    action_type_t type;
    uint8_t modifiers;
    uint8_t keycode;
    const char *string_ptr;
    size_t string_len;
    uint32_t delay_ms;
} prev_action_t;

static int process_line(const char *line, const hid_transport_t *transport,
                        uint32_t *default_delay, prev_action_t *prev, bool *stop_flag) {
    if (line[0] == '\0') return 0;

    if (strncmp(line, "REM ", 4) == 0 || strcmp(line, "REM") == 0) {
        return 0;
    }

    if (strncmp(line, "DEFAULT_DELAY ", 14) == 0) {
        *default_delay = (uint32_t)atoi(line + 14);
        return 0;
    }
    if (strncmp(line, "DEFAULTDELAY ", 13) == 0) {
        *default_delay = (uint32_t)atoi(line + 13);
        return 0;
    }

    if (strncmp(line, "DELAY ", 6) == 0) {
        uint32_t ms = (uint32_t)atoi(line + 6);
        transport->delay(ms, transport->ctx);
        prev->type = ACTION_DELAY;
        prev->delay_ms = ms;
        return 0;
    }

    if (strncmp(line, "STRING ", 7) == 0) {
        const char *text = line + 7;
        size_t len = strlen(text);
        transport->send_string(text, len, transport->ctx);
        prev->type = ACTION_STRING;
        prev->string_ptr = text;
        prev->string_len = len;
        if (*default_delay > 0) transport->delay(*default_delay, transport->ctx);
        transport->release_keys(transport->ctx);
        return 0;
    }

    if (strncmp(line, "REPEAT ", 7) == 0) {
        int count = atoi(line + 7);
        if (count < 1) count = 1;
        for (int i = 0; i < count; i++) {
            switch (prev->type) {
                case ACTION_KEY:
                    transport->send_key(prev->modifiers, prev->keycode, transport->ctx);
                    if (*default_delay > 0) transport->delay(*default_delay, transport->ctx);
                    transport->release_keys(transport->ctx);
                    break;
                case ACTION_STRING:
                    transport->send_string(prev->string_ptr, prev->string_len, transport->ctx);
                    if (*default_delay > 0) transport->delay(*default_delay, transport->ctx);
                    transport->release_keys(transport->ctx);
                    break;
                case ACTION_DELAY:
                    transport->delay(prev->delay_ms, transport->ctx);
                    break;
                default:
                    break;
            }
        }
        return 0;
    }

    {
        char buf[256];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        uint8_t modifiers = 0;
        uint8_t keycode = HID_KEY_NONE;

        char *saveptr = NULL;
        char *token = strtok_r(buf, " ", &saveptr);
        while (token) {
            uint8_t mod = parse_modifier(token);
            if (mod) {
                modifiers |= mod;
            } else {
                uint8_t kc = hid_key_name_to_keycode(token);
                if (kc != HID_KEY_NONE) {
                    keycode = kc;
                } else if (strlen(token) == 1) {
                    uint8_t kc2, mod2;
                    if (hid_ascii_to_keycode(token[0], &kc2, &mod2)) {
                        keycode = kc2;
                        if (!modifiers) {
                            modifiers |= mod2;
                        }
                    }
                }
            }
            token = strtok_r(NULL, " ", &saveptr);
        }

        if (modifiers || keycode != HID_KEY_NONE) {
            transport->send_key(modifiers, keycode, transport->ctx);
            prev->type = ACTION_KEY;
            prev->modifiers = modifiers;
            prev->keycode = keycode;
            if (*default_delay > 0) transport->delay(*default_delay, transport->ctx);
            transport->release_keys(transport->ctx);
            return 0;
        }
    }

    return 0;
}

int hid_script_execute_file(FILE *f, const hid_transport_t *transport) {
    if (!f || !transport) return -1;

    char line[256];
    uint32_t default_delay = 0;
    prev_action_t prev = {0};
    bool stop = false;
    int lines_executed = 0;

    while (fgets(line, sizeof(line), f) && !stop) {
        strip_trailing(line);
        process_line(line, transport, &default_delay, &prev, &stop);
        lines_executed++;
    }

    transport->release_keys(transport->ctx);
    return lines_executed;
}

int hid_script_execute(char *script_buf, const hid_transport_t *transport) {
    if (!script_buf || !transport) return -1;

    uint32_t default_delay = 0;
    prev_action_t prev = {0};
    bool stop = false;
    int lines_executed = 0;

    char *saveptr = NULL;
    char *line = strtok_r(script_buf, "\n", &saveptr);
    while (line && !stop) {
        strip_trailing(line);
        process_line(line, transport, &default_delay, &prev, &stop);
        lines_executed++;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    transport->release_keys(transport->ctx);
    return lines_executed;
}

#endif // CONFIG_HAS_BADUSB
