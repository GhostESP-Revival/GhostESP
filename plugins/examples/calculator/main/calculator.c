#include "../../../sdk/ghostesp_plugin_api.h"
#include <stdio.h>
#include <string.h>

void *memmove(void *dst, const void *src, size_t n) __attribute__((weak));
void *memmove(void *dst, const void *src, size_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

long long __divdi3(long long a, long long b) __attribute__((weak));
long long __divdi3(long long a, long long b) {
    if (b == 0) return 0;
    bool neg = (a < 0) != (b < 0);
    unsigned long long ua = a < 0 ? (unsigned long long)(-(a + 1)) + 1ULL : (unsigned long long)a;
    unsigned long long ub = b < 0 ? (unsigned long long)(-(b + 1)) + 1ULL : (unsigned long long)b;
    unsigned long long q = 0, bit = 1;
    while (ub < ua && bit <= (ua >> 1) && !(ub & (1ULL << 63))) { ub <<= 1; bit <<= 1; }
    while (bit) {
        if (ua >= ub) { ua -= ub; q |= bit; }
        ub >>= 1; bit >>= 1;
    }
    return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long a, long long b) __attribute__((weak));
long long __moddi3(long long a, long long b) {
    if (b == 0) return 0;
    unsigned long long ua = a < 0 ? (unsigned long long)(-(a + 1)) + 1ULL : (unsigned long long)a;
    unsigned long long ub = b < 0 ? (unsigned long long)(-(b + 1)) + 1ULL : (unsigned long long)b;
    while (ub < ua && !(ub & (1ULL << 63))) ub <<= 1;
    while (ub > (unsigned long long)(b < 0 ? -(b + 1) + 1ULL : b)) {
        if (ua >= ub) ua -= ub;
        ub >>= 1;
        if (ub == 0) break;
    }
    if (ub > 0 && ua >= ub) ua -= ub;
    return a < 0 ? -(long long)ua : (long long)ua;
}

unsigned long long __udivdi3(unsigned long long a, unsigned long long b) __attribute__((weak));
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) {
    if (b == 0) return 0;
    unsigned long long q = 0, bit = 1;
    while (b < a && bit <= (a >> 1) && !(b & (1ULL << 63))) { b <<= 1; bit <<= 1; }
    while (bit) {
        if (a >= b) { a -= b; q |= bit; }
        b >>= 1; bit >>= 1;
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b) __attribute__((weak));
unsigned long long __umoddi3(unsigned long long a, unsigned long long b) {
    if (b == 0) return 0;
    unsigned long long orig = b;
    while (b < a && !(b & (1ULL << 63))) b <<= 1;
    while (b >= orig) {
        if (a >= b) a -= b;
        b >>= 1;
        if (b == 0) break;
    }
    return a;
}

#define CALC_DISPLAY_MAX 32
#define CALC_ROWS 5
#define CALC_COLS 4
#define CALC_BTN_COUNT 19
#define TOUCH_SWIPE_THRESHOLD 24

static const ghostesp_api_t *api;

static ghostesp_ui_obj_t screen;
static ghostesp_ui_obj_t display_label;
static ghostesp_ui_obj_t btn_objs[CALC_BTN_COUNT];
static ghostesp_ui_obj_t row_containers[CALC_ROWS];
static ghostesp_ui_obj_t button_grid;
static ghostesp_ui_obj_t touch_bar;

static int scr_w, scr_h;
static bool compact;

static int sel_row = 0;
static int sel_col = 0;

static int touch_start_x;
static int touch_start_y;
static bool touch_started;

static char s_app_id[] = "calculator";
static char s_app_name[] = "Calculator";

#define CALC_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, ui_has_touchscreen) + sizeof(((ghostesp_api_t *)0)->ui_has_touchscreen))

typedef enum {
    BTN_AC = 0,
    BTN_SIGN,
    BTN_PERCENT,
    BTN_DIV,
    BTN_7,
    BTN_8,
    BTN_9,
    BTN_MUL,
    BTN_4,
    BTN_5,
    BTN_6,
    BTN_SUB,
    BTN_1,
    BTN_2,
    BTN_3,
    BTN_ADD,
    BTN_0,
    BTN_DOT,
    BTN_EQ,
} calc_btn_id_t;

typedef struct {
    const char *label;
    calc_btn_id_t id;
    uint32_t bg_color;
    uint32_t text_color;
    int flex_grow;
} calc_btn_def_t;

static const calc_btn_def_t btn_defs[CALC_BTN_COUNT] = {
    {"AC",  BTN_AC,      0xA5A5A5, 0x000000, 1},
    {"+/-", BTN_SIGN,    0xA5A5A5, 0x000000, 1},
    {"%",   BTN_PERCENT, 0xA5A5A5, 0x000000, 1},
    {"/",   BTN_DIV,     0xFF9F0A, 0xFFFFFF, 1},
    {"7",   BTN_7,       0x333333, 0xFFFFFF, 1},
    {"8",   BTN_8,       0x333333, 0xFFFFFF, 1},
    {"9",   BTN_9,       0x333333, 0xFFFFFF, 1},
    {"x",   BTN_MUL,     0xFF9F0A, 0xFFFFFF, 1},
    {"4",   BTN_4,       0x333333, 0xFFFFFF, 1},
    {"5",   BTN_5,       0x333333, 0xFFFFFF, 1},
    {"6",   BTN_6,       0x333333, 0xFFFFFF, 1},
    {"-",   BTN_SUB,     0xFF9F0A, 0xFFFFFF, 1},
    {"1",   BTN_1,       0x333333, 0xFFFFFF, 1},
    {"2",   BTN_2,       0x333333, 0xFFFFFF, 1},
    {"3",   BTN_3,       0x333333, 0xFFFFFF, 1},
    {"+",   BTN_ADD,     0xFF9F0A, 0xFFFFFF, 1},
    {"0",   BTN_0,       0x333333, 0xFFFFFF, 2},
    {".",   BTN_DOT,     0x333333, 0xFFFFFF, 1},
    {"=",   BTN_EQ,      0xFF9F0A, 0xFFFFFF, 1},
};

static const int btn_row_map[CALC_BTN_COUNT] = {
    0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4,
};

static const int btn_col_map[CALC_BTN_COUNT] = {
    0, 1, 2, 3,
    0, 1, 2, 3,
    0, 1, 2, 3,
    0, 1, 2, 3,
    0, 2, 3,
};

static char display_text[CALC_DISPLAY_MAX] = "0";

#define CALC_MAX_SCALE 10000000000LL
#define CALC_SCALE_DIGITS 10

static long long current_value = 0;
static long long stored_value = 0;
static char pending_op = 0;
static bool entering_number = false;
static bool has_decimal = false;
static bool just_evaluated = false;

static long long parse_display(void) {
    int i = 0;
    bool neg = false;
    if (display_text[0] == '-') { neg = true; i = 1; }
    long long int_part = 0;
    long long frac_part = 0;
    long long frac_div = 10;
    bool in_frac = false;
    for (; display_text[i] != '\0'; i++) {
        if (display_text[i] == '.') { in_frac = true; continue; }
        if (display_text[i] < '0' || display_text[i] > '9') continue;
        int d = display_text[i] - '0';
        if (in_frac) {
            if (frac_div <= CALC_MAX_SCALE) {
                frac_part = frac_part * 10 + d;
                frac_div *= 10;
            }
        } else {
            int_part = int_part * 10 + d;
        }
    }
    long long result = int_part * CALC_MAX_SCALE + frac_part * (CALC_MAX_SCALE / frac_div);
    return neg ? -result : result;
}

static void format_value(long long val, char *buf, size_t buf_len) {
    if (val == 0) {
        snprintf(buf, buf_len, "0");
        return;
    }
    bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    long long int_part = val / CALC_MAX_SCALE;
    long long frac_part = val % CALC_MAX_SCALE;
    char tmp[32];
    int pos = 0;
    if (neg) tmp[pos++] = '-';
    if (int_part == 0) {
        tmp[pos++] = '0';
    } else {
        char ibuf[20];
        int ilen = 0;
        while (int_part > 0) {
            ibuf[ilen++] = '0' + (int)(int_part % 10);
            int_part /= 10;
        }
        while (ilen > 0) tmp[pos++] = ibuf[--ilen];
    }
    if (frac_part > 0) {
        tmp[pos++] = '.';
        char fbuf[16];
        int flen = 0;
        long long fd = CALC_MAX_SCALE / 10;
        while (fd > 0) {
            fbuf[flen++] = '0' + (int)((frac_part / fd) % 10);
            fd /= 10;
        }
        while (flen > 0 && fbuf[flen - 1] == '0') flen--;
        for (int i = 0; i < flen; i++) tmp[pos++] = fbuf[i];
    }
    tmp[pos] = '\0';
    snprintf(buf, buf_len, "%s", tmp);
}

static void update_display(void);
static void on_btn_click(void *user);
static void calc_layout(void);
static void create_ui(void);
static void destroy_ui(void);
static void update_selection(void);
static void handle_button(calc_btn_id_t id);

static bool has_touchscreen(void) {
    return api->ui_has_touchscreen ? api->ui_has_touchscreen() : (api->ui_touch_bar_create != NULL);
}

static void format_number(long long val, char *buf, size_t buf_len) {
    format_value(val, buf, buf_len);
}

static void update_display(void) {
    if (!display_label || !api->ui_label_set_text) return;

    if (entering_number || just_evaluated) {
        if (pending_op != 0 && entering_number && !just_evaluated) {
            char buf[CALC_DISPLAY_MAX * 2 + 4];
            char sval[CALC_DISPLAY_MAX];
            format_number(stored_value, sval, sizeof(sval));
            snprintf(buf, sizeof(buf), "%s %c %s", sval, pending_op, display_text);
            api->ui_label_set_text(display_label, buf);
        } else {
            api->ui_label_set_text(display_label, display_text);
        }
    } else {
        char buf[CALC_DISPLAY_MAX];
        format_number(current_value, buf, sizeof(buf));
        snprintf(display_text, sizeof(display_text), "%s", buf);
        if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
    }
}

static void input_digit(int digit) {
    if (just_evaluated) {
        display_text[0] = '0';
        display_text[1] = '\0';
        has_decimal = false;
        just_evaluated = false;
        entering_number = true;
    }

    if (!entering_number) {
        display_text[0] = '0';
        display_text[1] = '\0';
        has_decimal = false;
        entering_number = true;
    }

    int len = strlen(display_text);

    if (display_text[0] == '0' && !has_decimal) {
        if (digit == 0) return;
        display_text[0] = '0' + digit;
        display_text[1] = '\0';
    } else {
        if (len < CALC_DISPLAY_MAX - 1) {
            display_text[len] = '0' + digit;
            display_text[len + 1] = '\0';
        }
    }

    current_value = parse_display();
    update_display();
}

static void input_decimal(void) {
    if (just_evaluated) {
        display_text[0] = '0';
        display_text[1] = '\0';
        has_decimal = false;
        just_evaluated = false;
        entering_number = true;
    }

    if (!entering_number) {
        display_text[0] = '0';
        display_text[1] = '\0';
        has_decimal = false;
        entering_number = true;
    }

    if (!has_decimal) {
        int len = strlen(display_text);
        if (len < CALC_DISPLAY_MAX - 1) {
            display_text[len] = '.';
            display_text[len + 1] = '\0';
            has_decimal = true;
        }
    }

    update_display();
}

static long long calc_add(long long a, long long b) { return a + b; }
static long long calc_sub(long long a, long long b) { return a - b; }
static long long calc_mul(long long a, long long b) {
    return (a / CALC_MAX_SCALE) * b + (a % CALC_MAX_SCALE) * (b / CALC_MAX_SCALE) +
           ((a % CALC_MAX_SCALE) * (b % CALC_MAX_SCALE)) / CALC_MAX_SCALE;
}
static long long calc_div(long long a, long long b) {
    if (b == 0) return 0;
    return (a / b) * CALC_MAX_SCALE + ((a % b) * CALC_MAX_SCALE) / b;
}

static void input_operator(char op) {
    if (pending_op != 0 && entering_number) {
        long long result = 0;
        bool valid = true;
        switch (pending_op) {
            case '+': result = calc_add(stored_value, current_value); break;
            case '-': result = calc_sub(stored_value, current_value); break;
            case 'x': result = calc_mul(stored_value, current_value); break;
            case '/':
                if (current_value == 0) {
                    valid = false;
                } else {
                    result = calc_div(stored_value, current_value);
                }
                break;
            default: valid = false; break;
        }

        if (!valid) {
            if (api->ui_label_set_text) api->ui_label_set_text(display_label, "Error");
            pending_op = 0;
            entering_number = false;
            just_evaluated = true;
            current_value = 0;
            stored_value = 0;
            return;
        }

        current_value = result;
        update_display();
    }

    stored_value = current_value;
    pending_op = op;
    entering_number = false;
    just_evaluated = false;
    has_decimal = false;
}

static void input_equals(void) {
    if (pending_op == 0) return;

    long long result = 0;
    bool valid = true;

    switch (pending_op) {
        case '+': result = calc_add(stored_value, current_value); break;
        case '-': result = calc_sub(stored_value, current_value); break;
        case 'x': result = calc_mul(stored_value, current_value); break;
        case '/':
            if (current_value == 0) {
                valid = false;
            } else {
                result = calc_div(stored_value, current_value);
            }
            break;
        default: valid = false; break;
    }

    pending_op = 0;
    entering_number = false;
    just_evaluated = true;

    if (!valid) {
        if (api->ui_label_set_text) api->ui_label_set_text(display_label, "Error");
        current_value = 0;
        stored_value = 0;
        return;
    }

    current_value = result;
    stored_value = 0;

    char buf[CALC_DISPLAY_MAX];
    format_number(current_value, buf, sizeof(buf));
    snprintf(display_text, sizeof(display_text), "%s", buf);
    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void input_clear(void) {
    current_value = 0;
    stored_value = 0;
    pending_op = 0;
    entering_number = false;
    has_decimal = false;
    just_evaluated = false;
    display_text[0] = '0';
    display_text[1] = '\0';
    update_display();
}

static void input_sign(void) {
    if (current_value == 0) return;
    current_value = -current_value;

    if (entering_number && !just_evaluated) {
        if (display_text[0] == '-') {
            int len = strlen(display_text);
            for (int i = 0; i < len; i++) display_text[i] = display_text[i + 1];
        } else {
            int len = strlen(display_text);
            if (len < CALC_DISPLAY_MAX - 1) {
                for (int i = len; i >= 0; i--) display_text[i + 1] = display_text[i];
                display_text[0] = '-';
            }
        }
    } else {
        char buf[CALC_DISPLAY_MAX];
        format_number(current_value, buf, sizeof(buf));
        snprintf(display_text, sizeof(display_text), "%s", buf);
    }

    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void input_percent(void) {
    current_value = current_value / 100;
    entering_number = false;
    just_evaluated = true;

    char buf[CALC_DISPLAY_MAX];
    format_number(current_value, buf, sizeof(buf));
    snprintf(display_text, sizeof(display_text), "%s", buf);
    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void handle_button(calc_btn_id_t id) {
    switch (id) {
        case BTN_0: input_digit(0); break;
        case BTN_1: input_digit(1); break;
        case BTN_2: input_digit(2); break;
        case BTN_3: input_digit(3); break;
        case BTN_4: input_digit(4); break;
        case BTN_5: input_digit(5); break;
        case BTN_6: input_digit(6); break;
        case BTN_7: input_digit(7); break;
        case BTN_8: input_digit(8); break;
        case BTN_9: input_digit(9); break;
        case BTN_DOT: input_decimal(); break;
        case BTN_ADD: input_operator('+'); break;
        case BTN_SUB: input_operator('-'); break;
        case BTN_MUL: input_operator('x'); break;
        case BTN_DIV: input_operator('/'); break;
        case BTN_EQ: input_equals(); break;
        case BTN_AC: input_clear(); break;
        case BTN_SIGN: input_sign(); break;
        case BTN_PERCENT: input_percent(); break;
    }
}

static void on_btn_click(void *user) {
    calc_btn_id_t id = (calc_btn_id_t)(intptr_t)user;
    handle_button(id);
    if (api->ui_anim_press_pulse && btn_objs[id]) {
        api->ui_anim_press_pulse(btn_objs[id]);
    }
}

#define CALC_TOUCH_BAR_HEIGHT 34

static void calc_layout(void) {
    scr_w = api->ui_screen_get_content_width ? api->ui_screen_get_content_width() : 240;
    scr_h = api->ui_screen_get_content_height ? api->ui_screen_get_content_height() : 320;
    compact = api->ui_screen_is_compact ? api->ui_screen_is_compact() : (scr_w < 200 || scr_h < 200);
    if (has_touchscreen()) scr_h -= CALC_TOUCH_BAR_HEIGHT;
}

static int get_btn_index(int row, int col) {
    for (int i = 0; i < CALC_BTN_COUNT; i++) {
        if (btn_row_map[i] == row && btn_col_map[i] == col) return i;
    }
    return -1;
}

static void update_selection(void) {
    for (int i = 0; i < CALC_BTN_COUNT; i++) {
        if (!btn_objs[i] || !api->ui_button_set_selected) continue;
        bool selected = (btn_row_map[i] == sel_row && btn_col_map[i] == sel_col);
        api->ui_button_set_selected(btn_objs[i], selected);
    }
}

static void touch_back_clicked(void *user) {
    (void)user;
    if (api->app_exit) api->app_exit();
}

static void create_touch_controls(void) {
    if (touch_bar) return;
    if (!has_touchscreen() || !api->ui_touch_bar_create) return;

    touch_bar = api->ui_touch_bar_create(NULL);
    if (!touch_bar) return;
    if (api->ui_touch_bar_add_back) api->ui_touch_bar_add_back(touch_bar, touch_back_clicked, NULL);
}

static void destroy_touch_bar(void) {
    touch_bar = NULL;
}

static void create_ui(void) {
    calc_layout();

    uint32_t bg_color = 0x000000;
    uint32_t text_color = 0xFFFFFF;

    screen = api->ui_screen_create(s_app_name);
    if (!screen) return;

    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(screen, bg_color);
    if (api->ui_obj_set_pad) api->ui_obj_set_pad(screen, compact ? 2 : 4, compact ? 2 : 4, 0, 0);
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(screen, GHOSTESP_FLEX_FLOW_COLUMN);
    if (api->ui_obj_set_flex_align) api->ui_obj_set_flex_align(screen, GHOSTESP_FLEX_ALIGN_SPACE_BETWEEN, GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(screen, false);
    if (api->ui_obj_set_height) api->ui_obj_set_height(screen, scr_h);

    int display_h = compact ? (scr_h / 6) : (scr_h / 5);
    if (display_h < 30) display_h = 30;
    if (display_h > 80) display_h = 80;

    display_label = api->ui_label_create(screen, "0");
    if (display_label) {
        if (api->ui_obj_set_width) api->ui_obj_set_width(display_label, scr_w - (compact ? 8 : 16));
        if (api->ui_obj_set_height) api->ui_obj_set_height(display_label, display_h);
        if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(display_label, text_color);
        if (api->ui_obj_set_font) api->ui_obj_set_font(display_label, GHOSTESP_FONT_TITLE);
        if (api->ui_obj_align) api->ui_obj_align(display_label, GHOSTESP_ALIGN_BOTTOM_RIGHT, 0, 0);
        if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(display_label, bg_color);
    }

    button_grid = api->ui_card_create(screen);
    if (!button_grid) button_grid = screen;
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(button_grid, bg_color);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(button_grid, 0);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(button_grid, 0);
    if (api->ui_obj_set_pad) api->ui_obj_set_pad(button_grid, 0, 0, 0, 0);
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(button_grid, GHOSTESP_FLEX_FLOW_COLUMN);
    if (api->ui_obj_set_pad_row) api->ui_obj_set_pad_row(button_grid, compact ? 3 : 6);
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(button_grid, false);
    if (api->ui_obj_set_flex_grow) api->ui_obj_set_flex_grow(button_grid, 1);

    int avail_h = scr_h - display_h - (compact ? 4 : 8);
    int row_gap = compact ? 3 : 6;
    int btn_h = (avail_h - row_gap * (CALC_ROWS - 1)) / CALC_ROWS;
    if (btn_h < 24) btn_h = 24;

    for (int r = 0; r < CALC_ROWS; r++) {
        row_containers[r] = api->ui_card_create(button_grid);
        if (!row_containers[r]) row_containers[r] = button_grid;
        if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(row_containers[r], bg_color);
        if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(row_containers[r], 0);
        if (api->ui_obj_set_radius) api->ui_obj_set_radius(row_containers[r], 0);
        if (api->ui_obj_set_pad) api->ui_obj_set_pad(row_containers[r], 0, 0, 0, 0);
        if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(row_containers[r], GHOSTESP_FLEX_FLOW_ROW);
        if (api->ui_obj_set_pad_column) api->ui_obj_set_pad_column(row_containers[r], compact ? 3 : 6);
        if (api->ui_obj_set_height) api->ui_obj_set_height(row_containers[r], btn_h);
        if (api->ui_obj_set_flex_grow) api->ui_obj_set_flex_grow(row_containers[r], 1);
        if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(row_containers[r], false);
    }

    for (int i = 0; i < CALC_BTN_COUNT; i++) {
        int r = btn_row_map[i];
        ghostesp_ui_obj_t parent = row_containers[r];
        if (!parent) continue;

        btn_objs[i] = api->ui_button_create(parent, btn_defs[i].label, on_btn_click, (void *)(intptr_t)i);
        if (!btn_objs[i]) continue;

        if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(btn_objs[i], btn_defs[i].bg_color);
        if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(btn_objs[i], btn_defs[i].text_color);
        if (api->ui_obj_set_radius) api->ui_obj_set_radius(btn_objs[i], compact ? 8 : 14);
        if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(btn_objs[i], 0);
        if (api->ui_obj_set_flex_grow) api->ui_obj_set_flex_grow(btn_objs[i], (uint8_t)btn_defs[i].flex_grow);
        if (api->ui_obj_set_height) api->ui_obj_set_height(btn_objs[i], btn_h);
        if (api->ui_obj_set_font) api->ui_obj_set_font(btn_objs[i], GHOSTESP_FONT_BODY);
    }

    create_touch_controls();
    update_selection();
    update_display();
}

static void destroy_ui(void) {
    destroy_touch_bar();
    for (int i = 0; i < CALC_BTN_COUNT; i++) btn_objs[i] = NULL;
    for (int i = 0; i < CALC_ROWS; i++) row_containers[i] = NULL;
    button_grid = NULL;
    display_label = NULL;
    screen = NULL;
}

static void calculator_start(void) {
    if (api->log) api->log("Calculator started");

    current_value = 0;
    stored_value = 0;
    pending_op = 0;
    entering_number = false;
    has_decimal = false;
    just_evaluated = false;
    touch_started = false;
    sel_row = 0;
    sel_col = 0;
    display_text[0] = '0';
    display_text[1] = '\0';

    create_ui();
}

static void calculator_stop(void) {
    destroy_ui();
    if (api->log) api->log("Calculator stopped");
}

static bool handle_touch_navigation(const ghostesp_input_event_t *event) {
    if (!event || event->type != GHOSTESP_INPUT_TOUCH) return false;

    if (event->pressed) {
        touch_started = true;
        touch_start_x = event->x;
        touch_start_y = event->y;
        return false;
    }

    if (!touch_started) return false;
    touch_started = false;

    int dx = event->x - touch_start_x;
    int dy = event->y - touch_start_y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;

    if (abs_dx >= TOUCH_SWIPE_THRESHOLD && abs_dx > abs_dy && dx > 0) {
        if (api->app_exit) api->app_exit();
        return true;
    }
    if (abs_dx >= TOUCH_SWIPE_THRESHOLD && abs_dx > abs_dy && dx < 0) {
        return true;
    }
    if (abs_dy >= TOUCH_SWIPE_THRESHOLD && abs_dy >= abs_dx) {
        return true;
    }

    return false;
}

static void move_selection(int d_row, int d_col) {
    int new_row = sel_row + d_row;
    int new_col = sel_col + d_col;

    if (new_row < 0) new_row = CALC_ROWS - 1;
    if (new_row >= CALC_ROWS) new_row = 0;
    if (new_col < 0) new_col = CALC_COLS - 1;
    if (new_col >= CALC_COLS) new_col = 0;

    if (d_col != 0) {
        int attempts = 0;
        while (get_btn_index(new_row, new_col) < 0 && attempts < CALC_COLS) {
            new_col += d_col;
            if (new_col < 0) new_col = CALC_COLS - 1;
            if (new_col >= CALC_COLS) new_col = 0;
            attempts++;
        }
    }

    if (get_btn_index(new_row, new_col) < 0) return;

    sel_row = new_row;
    sel_col = new_col;
    update_selection();
}

static void calculator_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        if (handle_touch_navigation(event)) return;
        return;
    }

    if (event->type == GHOSTESP_INPUT_BACK) {
        if (api->app_exit) api->app_exit();
        return;
    }

    if (event->type == GHOSTESP_INPUT_UP) {
        move_selection(-1, 0);
        return;
    }

    if (event->type == GHOSTESP_INPUT_DOWN) {
        move_selection(1, 0);
        return;
    }

    if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_RIGHT) {
        int d_col = (event->type == GHOSTESP_INPUT_RIGHT) ? 1 : -1;
        int new_row = sel_row;
        int new_col = sel_col + d_col;
        if (new_col < 0) {
            if (new_row > 0) { new_row--; new_col = CALC_COLS - 1; }
            else new_col = CALC_COLS - 1;
        } else if (new_col >= CALC_COLS) {
            if (new_row < CALC_ROWS - 1) { new_row++; new_col = 0; }
            else new_col = 0;
        }
        int attempts = 0;
        while (get_btn_index(new_row, new_col) < 0 && attempts < CALC_COLS + 1) {
            new_col += d_col;
            if (new_col < 0) {
                if (new_row > 0) { new_row--; new_col = CALC_COLS - 1; }
                else new_col = CALC_COLS - 1;
            } else if (new_col >= CALC_COLS) {
                if (new_row < CALC_ROWS - 1) { new_row++; new_col = 0; }
                else new_col = 0;
            }
            attempts++;
        }
        if (get_btn_index(new_row, new_col) >= 0) {
            sel_row = new_row;
            sel_col = new_col;
            update_selection();
        }
        return;
    }

    if (event->type == GHOSTESP_INPUT_SELECT) {
        int idx = get_btn_index(sel_row, sel_col);
        if (idx >= 0) {
            handle_button((calc_btn_id_t)idx);
            if (api->ui_anim_press_pulse && btn_objs[idx]) {
                api->ui_anim_press_pulse(btn_objs[idx]);
            }
        }
        return;
    }

    if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == 27 || v == 8 || v == 127 || v == 'q' || v == 'Q') {
            if (api->app_exit) api->app_exit();
        } else if (v == '0') handle_button(BTN_0);
        else if (v == '1') handle_button(BTN_1);
        else if (v == '2') handle_button(BTN_2);
        else if (v == '3') handle_button(BTN_3);
        else if (v == '4') handle_button(BTN_4);
        else if (v == '5') handle_button(BTN_5);
        else if (v == '6') handle_button(BTN_6);
        else if (v == '7') handle_button(BTN_7);
        else if (v == '8') handle_button(BTN_8);
        else if (v == '9') handle_button(BTN_9);
        else if (v == '.' || v == ',') handle_button(BTN_DOT);
        else if (v == '+') handle_button(BTN_ADD);
        else if (v == '-') handle_button(BTN_SUB);
        else if (v == '*') handle_button(BTN_MUL);
        else if (v == '/') handle_button(BTN_DIV);
        else if (v == '=' || v == 10) handle_button(BTN_EQ);
        else if (v == 'c' || v == 'C' || v == ' ') handle_button(BTN_AC);
        else if (v == '%') handle_button(BTN_PERCENT);
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_APP_STRUCT_SIZE_V1,
    .id = s_app_id,
    .name = s_app_name,
    .on_start = calculator_start,
    .on_stop = calculator_stop,
    .on_input = calculator_input,
    .on_tick = NULL,
};

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < CALC_REQUIRED_API_SIZE) {
        if (host_api->log) host_api->log("Calculator requires newer plugin API");
        return 0;
    }
    api = host_api;
    return &app;
}

void app_main(void) {}
