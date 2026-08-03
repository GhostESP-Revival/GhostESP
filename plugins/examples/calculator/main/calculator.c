#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"
#include <stdio.h>
#include <string.h>

void *memmove(void *dst, const void *src, size_t n) __attribute__((weak));
void *memmove(void *dst, const void *src, size_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

char *strcpy(char *dst, const char *src) __attribute__((weak));
char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {}
    return out;
}

long long __divdi3(long long a, long long b) __attribute__((weak));
long long __divdi3(long long a, long long b) {
    if (b == 0) return 0;
    bool neg = (a < 0) != (b < 0);
    unsigned long long ua = a < 0 ? (unsigned long long)(-(a + 1)) + 1ULL : (unsigned long long)a;
    unsigned long long ub = b < 0 ? (unsigned long long)(-(b + 1)) + 1ULL : (unsigned long long)b;
    unsigned long long q = 0, bit = 1;
    while (ub < ua && bit <= (ua >> 1) && !(ub & (1ULL << 63))) { ub <<= 1; bit <<= 1; }
    while (bit) { if (ua >= ub) { ua -= ub; q |= bit; } ub >>= 1; bit >>= 1; }
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
    while (bit) { if (a >= b) { a -= b; q |= bit; } b >>= 1; bit >>= 1; }
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

static const ghostesp_api_t *api;
static ghostesp_theme_t theme;
static ghostesp_layout_t layout;
static ghostesp_touch_state_t touch_state;

static ghostesp_ui_obj_t screen;
static ghostesp_ui_obj_t display_label;
static ghostesp_ui_obj_t btn_objs[CALC_BTN_COUNT];
static ghostesp_ui_obj_t row_containers[CALC_ROWS];
static ghostesp_ui_obj_t button_grid;
static ghostesp_ui_obj_t touch_bar;

static ghostesp_grid_t grid;
static const uint8_t grid_cols[CALC_ROWS] = {4, 4, 4, 4, 3};

static char s_app_id[] = "calculator";
static char s_app_name[] = "Calculator";

#define CALC_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, ui_has_touchscreen) + sizeof(((ghostesp_api_t *)0)->ui_has_touchscreen))

typedef enum {
    BTN_AC = 0, BTN_SIGN, BTN_PERCENT, BTN_DIV,
    BTN_7, BTN_8, BTN_9, BTN_MUL,
    BTN_4, BTN_5, BTN_6, BTN_SUB,
    BTN_1, BTN_2, BTN_3, BTN_ADD,
    BTN_0, BTN_DOT, BTN_EQ,
} calc_btn_id_t;

typedef struct {
    const char *label;
    uint32_t bg_color;
    uint32_t text_color;
    int flex_grow;
} calc_btn_def_t;

static const calc_btn_def_t btn_defs[CALC_BTN_COUNT] = {
    {"AC",  0xA5A5A5, 0x000000, 1}, {"+/-", 0xA5A5A5, 0x000000, 1},
    {"%",   0xA5A5A5, 0x000000, 1}, {"/",   0xFF9F0A, 0xFFFFFF, 1},
    {"7",   0x333333, 0xFFFFFF, 1}, {"8",   0x333333, 0xFFFFFF, 1},
    {"9",   0x333333, 0xFFFFFF, 1}, {"x",   0xFF9F0A, 0xFFFFFF, 1},
    {"4",   0x333333, 0xFFFFFF, 1}, {"5",   0x333333, 0xFFFFFF, 1},
    {"6",   0x333333, 0xFFFFFF, 1}, {"-",   0xFF9F0A, 0xFFFFFF, 1},
    {"1",   0x333333, 0xFFFFFF, 1}, {"2",   0x333333, 0xFFFFFF, 1},
    {"3",   0x333333, 0xFFFFFF, 1}, {"+",   0xFF9F0A, 0xFFFFFF, 1},
    {"0",   0x333333, 0xFFFFFF, 2}, {".",   0x333333, 0xFFFFFF, 1},
    {"=",   0xFF9F0A, 0xFFFFFF, 1},
};

static char display_text[CALC_DISPLAY_MAX] = "0";
#define CALC_MAX_SCALE 10000000000LL
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
    long long int_part = 0, frac_part = 0, frac_div = 10;
    bool in_frac = false;
    for (; display_text[i]; i++) {
        if (display_text[i] == '.') { in_frac = true; continue; }
        if (display_text[i] < '0' || display_text[i] > '9') continue;
        int d = display_text[i] - '0';
        if (in_frac) { if (frac_div <= CALC_MAX_SCALE) { frac_part = frac_part * 10 + d; frac_div *= 10; } }
        else { int_part = int_part * 10 + d; }
    }
    long long result = int_part * CALC_MAX_SCALE + frac_part * (CALC_MAX_SCALE / frac_div);
    return neg ? -result : result;
}

static void format_value(long long val, char *buf, size_t buf_len) {
    if (val == 0) { snprintf(buf, buf_len, "0"); return; }
    bool neg = val < 0;
    if (neg) val = -val;
    long long int_part = val / CALC_MAX_SCALE;
    long long frac_part = val % CALC_MAX_SCALE;
    char tmp[32]; int pos = 0;
    if (neg) tmp[pos++] = '-';
    if (int_part == 0) { tmp[pos++] = '0'; }
    else { char ibuf[20]; int ilen = 0;
        while (int_part > 0) { ibuf[ilen++] = '0' + (int)(int_part % 10); int_part /= 10; }
        while (ilen > 0) tmp[pos++] = ibuf[--ilen]; }
    if (frac_part > 0) {
        tmp[pos++] = '.';
        char fbuf[16]; int flen = 0;
        long long fd = CALC_MAX_SCALE / 10;
        while (fd > 0) { fbuf[flen++] = '0' + (int)((frac_part / fd) % 10); fd /= 10; }
        while (flen > 0 && fbuf[flen - 1] == '0') flen--;
        for (int i = 0; i < flen; i++) tmp[pos++] = fbuf[i];
    }
    tmp[pos] = '\0';
    snprintf(buf, buf_len, "%s", tmp);
}

static void update_display(void) {
    if (!display_label || !api->ui_label_set_text) return;
    if (entering_number || just_evaluated) {
        if (pending_op != 0 && entering_number && !just_evaluated) {
            char buf[CALC_DISPLAY_MAX * 2 + 4], sval[CALC_DISPLAY_MAX];
            format_value(stored_value, sval, sizeof(sval));
            snprintf(buf, sizeof(buf), "%s %c %s", sval, pending_op, display_text);
            api->ui_label_set_text(display_label, buf);
        } else {
            api->ui_label_set_text(display_label, display_text);
        }
    } else {
        char buf[CALC_DISPLAY_MAX];
        format_value(current_value, buf, sizeof(buf));
        snprintf(display_text, sizeof(display_text), "%s", buf);
        if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
    }
}

static void input_digit(int digit);
static void input_decimal(void);
static void input_operator(char op);
static void input_equals(void);
static void input_clear(void);
static void input_sign(void);
static void input_percent(void);

static void handle_button(calc_btn_id_t id) {
    switch (id) {
        case BTN_0: case BTN_1: case BTN_2: case BTN_3: case BTN_4:
        case BTN_5: case BTN_6: case BTN_7: case BTN_8: case BTN_9:
            input_digit(id - BTN_0); break;
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

static void input_digit(int digit) {
    if (just_evaluated) { display_text[0] = '0'; display_text[1] = '\0'; has_decimal = false; just_evaluated = false; entering_number = true; }
    if (!entering_number) { display_text[0] = '0'; display_text[1] = '\0'; has_decimal = false; entering_number = true; }
    int len = strlen(display_text);
    if (display_text[0] == '0' && !has_decimal) {
        if (digit == 0) return;
        display_text[0] = '0' + digit; display_text[1] = '\0';
    } else {
        if (len < CALC_DISPLAY_MAX - 1) { display_text[len] = '0' + digit; display_text[len + 1] = '\0'; }
    }
    current_value = parse_display();
    update_display();
}

static void input_decimal(void) {
    if (just_evaluated) { display_text[0] = '0'; display_text[1] = '\0'; has_decimal = false; just_evaluated = false; entering_number = true; }
    if (!entering_number) { display_text[0] = '0'; display_text[1] = '\0'; has_decimal = false; entering_number = true; }
    if (!has_decimal) {
        int len = strlen(display_text);
        if (len < CALC_DISPLAY_MAX - 1) { display_text[len] = '.'; display_text[len + 1] = '\0'; has_decimal = true; }
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
        long long result = 0; bool valid = true;
        switch (pending_op) {
            case '+': result = calc_add(stored_value, current_value); break;
            case '-': result = calc_sub(stored_value, current_value); break;
            case 'x': result = calc_mul(stored_value, current_value); break;
            case '/': if (current_value == 0) valid = false; else result = calc_div(stored_value, current_value); break;
            default: valid = false; break;
        }
        if (!valid) {
            if (api->ui_label_set_text) api->ui_label_set_text(display_label, "Error");
            pending_op = 0; entering_number = false; just_evaluated = true;
            current_value = 0; stored_value = 0; return;
        }
        current_value = result;
        update_display();
    }
    stored_value = current_value; pending_op = op;
    entering_number = false; just_evaluated = false; has_decimal = false;
}

static void input_equals(void) {
    if (pending_op == 0) return;
    long long result = 0; bool valid = true;
    switch (pending_op) {
        case '+': result = calc_add(stored_value, current_value); break;
        case '-': result = calc_sub(stored_value, current_value); break;
        case 'x': result = calc_mul(stored_value, current_value); break;
        case '/': if (current_value == 0) valid = false; else result = calc_div(stored_value, current_value); break;
        default: valid = false; break;
    }
    pending_op = 0; entering_number = false; just_evaluated = true;
    if (!valid) {
        if (api->ui_label_set_text) api->ui_label_set_text(display_label, "Error");
        current_value = 0; stored_value = 0; return;
    }
    current_value = result; stored_value = 0;
    char buf[CALC_DISPLAY_MAX]; format_value(current_value, buf, sizeof(buf));
    snprintf(display_text, sizeof(display_text), "%s", buf);
    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void input_clear(void) {
    current_value = 0; stored_value = 0; pending_op = 0;
    entering_number = false; has_decimal = false; just_evaluated = false;
    display_text[0] = '0'; display_text[1] = '\0';
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
        char buf[CALC_DISPLAY_MAX]; format_value(current_value, buf, sizeof(buf));
        snprintf(display_text, sizeof(display_text), "%s", buf);
    }
    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void input_percent(void) {
    current_value = current_value / 100;
    entering_number = false; just_evaluated = true;
    char buf[CALC_DISPLAY_MAX]; format_value(current_value, buf, sizeof(buf));
    snprintf(display_text, sizeof(display_text), "%s", buf);
    if (api->ui_label_set_text) api->ui_label_set_text(display_label, display_text);
}

static void on_btn_click(void *user) {
    calc_btn_id_t id = (calc_btn_id_t)(intptr_t)user;
    handle_button(id);
    if (api->ui_anim_press_pulse && btn_objs[id])
        api->ui_anim_press_pulse(btn_objs[id]);
}

static void touch_back_clicked(void *user) {
    (void)user;
    GH_VOID(api, app_exit);
}

#define CALC_TOUCH_BAR_HEIGHT 34

static void create_ui(void) {
    int margin = layout.compact ? 2 : 4;

    screen = api->ui_screen_create(s_app_name);
    if (!screen) return;
    GH_VOID(api, ui_obj_set_bg_color, screen, theme.bg);
    GH_VOID(api, ui_obj_set_pad, screen, margin, margin, 0, 0);
    GH_VOID(api, ui_obj_set_flex_flow, screen, GHOSTESP_FLEX_FLOW_COLUMN);
    GH_VOID(api, ui_obj_set_flex_align, screen, GHOSTESP_FLEX_ALIGN_SPACE_BETWEEN, GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);
    GH_VOID(api, ui_obj_set_scrollable, screen, false);
    GH_VOID(api, ui_obj_set_height, screen, layout.content_h);

    int display_h = layout.compact ? (layout.content_h / 6) : (layout.content_h / 5);
    if (display_h < 30) display_h = 30;
    if (display_h > 80) display_h = 80;

    display_label = api->ui_label_create(screen, "0");
    if (display_label) {
        GH_VOID(api, ui_obj_set_width, display_label, layout.content_w - (layout.compact ? 8 : 16));
        GH_VOID(api, ui_obj_set_height, display_label, display_h);
        GH_VOID(api, ui_obj_set_text_color, display_label, theme.text);
        GH_VOID(api, ui_obj_set_font, display_label, GHOSTESP_FONT_TITLE);
        GH_VOID(api, ui_obj_align, display_label, GHOSTESP_ALIGN_BOTTOM_RIGHT, 0, 0);
        GH_VOID(api, ui_obj_set_bg_color, display_label, theme.bg);
    }

    button_grid = api->ui_card_create ? api->ui_card_create(screen) : screen;
    if (!button_grid) button_grid = screen;
    GH_VOID(api, ui_obj_set_bg_color, button_grid, theme.bg);
    GH_VOID(api, ui_obj_set_border_width, button_grid, 0);
    GH_VOID(api, ui_obj_set_radius, button_grid, 0);
    GH_VOID(api, ui_obj_set_pad, button_grid, 0, 0, 0, 0);
    GH_VOID(api, ui_obj_set_flex_flow, button_grid, GHOSTESP_FLEX_FLOW_COLUMN);
    GH_VOID(api, ui_obj_set_pad_row, button_grid, layout.compact ? 3 : 6);
    GH_VOID(api, ui_obj_set_scrollable, button_grid, false);
    GH_VOID(api, ui_obj_set_flex_grow, button_grid, 1);

    int avail_h = layout.content_h - display_h - (layout.compact ? 4 : 8);
    int row_gap = layout.compact ? 3 : 6;
    int btn_h = (avail_h - row_gap * (CALC_ROWS - 1)) / CALC_ROWS;
    if (btn_h < 24) btn_h = 24;

    for (int r = 0; r < CALC_ROWS; r++) {
        row_containers[r] = gh_row(api, button_grid);
        GH_VOID(api, ui_obj_set_height, row_containers[r], btn_h);
        GH_VOID(api, ui_obj_set_flex_grow, row_containers[r], 1);
        GH_VOID(api, ui_obj_set_pad_column, row_containers[r], layout.compact ? 3 : 6);
    }

    int idx = 0;
    for (int r = 0; r < CALC_ROWS; r++) {
        for (int c = 0; c < (int)grid_cols[r]; c++) {
            btn_objs[idx] = gh_button(api, row_containers[r], btn_defs[idx].label,
                btn_defs[idx].bg_color, btn_defs[idx].text_color,
                layout.compact ? 8 : 14, on_btn_click, (void *)(intptr_t)idx);
            if (btn_objs[idx]) {
                GH_VOID(api, ui_obj_set_flex_grow, btn_objs[idx], (uint8_t)btn_defs[idx].flex_grow);
                GH_VOID(api, ui_obj_set_height, btn_objs[idx], btn_h);
                GH_VOID(api, ui_obj_set_font, btn_objs[idx], GHOSTESP_FONT_BODY);
            }
            idx++;
        }
    }

    if (layout.has_touch) {
        touch_bar = gh_touch_bar(api, true, touch_back_clicked, NULL);
    }

    gh_grid_highlight(&grid, api, btn_objs, CALC_BTN_COUNT);
    update_display();
}

static void destroy_ui(void) {
    touch_bar = NULL;
    for (int i = 0; i < CALC_BTN_COUNT; i++) btn_objs[i] = NULL;
    for (int i = 0; i < CALC_ROWS; i++) row_containers[i] = NULL;
    button_grid = NULL; display_label = NULL; screen = NULL;
}

static void calculator_start(void) {
    if (api->log) api->log("Calculator started");

    gh_theme_init(api, &theme);
    gh_layout_init(api, &layout);
    gh_grid_init(&grid, CALC_ROWS, grid_cols);
    gh_touch_reset(&touch_state);

    current_value = 0; stored_value = 0; pending_op = 0;
    entering_number = false; has_decimal = false; just_evaluated = false;
    display_text[0] = '0'; display_text[1] = '\0';

    create_ui();
}

static void calculator_stop(void) {
    destroy_ui();
    if (api->log) api->log("Calculator stopped");
}

static void calculator_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    /* Touch: swipe detection */
    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_RIGHT) GH_VOID(api, app_exit);
        return;
    }

    /* D-pad: grid navigation */
    int btn = gh_grid_input(&grid, event);
    if (btn >= 0) {
        if (event->type == GHOSTESP_INPUT_SELECT) {
            handle_button((calc_btn_id_t)btn);
            if (api->ui_anim_press_pulse && btn_objs[btn])
                api->ui_anim_press_pulse(btn_objs[btn]);
        }
        gh_grid_highlight(&grid, api, btn_objs, CALC_BTN_COUNT);
        return;
    }

    /* Back */
    if (event->type == GHOSTESP_INPUT_BACK) {
        GH_VOID(api, app_exit);
        return;
    }

    /* Keyboard */
    if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == 27 || v == 8 || v == 127 || v == 'q' || v == 'Q') { GH_VOID(api, app_exit); }
        else if (v >= '0' && v <= '9') { handle_button((calc_btn_id_t)(BTN_0 + v - '0')); }
        else if (v == '.' || v == ',') { handle_button(BTN_DOT); }
        else if (v == '+') { handle_button(BTN_ADD); }
        else if (v == '-') { handle_button(BTN_SUB); }
        else if (v == '*') { handle_button(BTN_MUL); }
        else if (v == '/') { handle_button(BTN_DIV); }
        else if (v == '=' || v == 10) { handle_button(BTN_EQ); }
        else if (v == 'c' || v == 'C' || v == ' ') { handle_button(BTN_AC); }
        else if (v == '%') { handle_button(BTN_PERCENT); }
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    s_app_id, s_app_name,
    calculator_start, calculator_stop, calculator_input, NULL
);

GHOSTESP_APP_INIT_WITH_API(app, api, "calculator", CALC_REQUIRED_API_SIZE)
