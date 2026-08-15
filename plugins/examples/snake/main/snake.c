#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID 24
#define MAX_BODY (GRID * GRID)
#define TOUCH_BAR_HEIGHT 34
#define START_MS 190
#define MIN_MS 65
#define COLOR_BG 0x0841
#define COLOR_BOARD 0x1082
#define COLOR_GRID 0x18C3
#define COLOR_BODY 0x3666
#define COLOR_HEAD 0x07E0
#define COLOR_FOOD 0xF800
#define COLOR_TEXT 0xFFFF
#define COLOR_MUTED 0x8410

typedef enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT } direction_t;
typedef struct { uint8_t x, y; } point_t;

static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas, touch_bar;
static ghostesp_ui_timer_t timer;
static ghostesp_touch_state_t touch_state;
static uint16_t *pixels;
static size_t pixels_size;
static int canvas_w, canvas_h, board_x, board_y, board_px, cell_px;
static int hud_x, hud_y, hud_w, hud_h;
static point_t body[MAX_BODY], food;
static uint8_t occupied[GRID][GRID];
static int length, score;
static direction_t direction, queued;
static uint32_t rng_state;
static bool paused, game_over, won, exit_requested;

static uint32_t game_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void request_exit(void) {
    if (exit_requested) return;
    exit_requested = true;
    GH_VOID(api, request_exit);
}

static void touch_back(void *user) { (void)user; request_exit(); }

static void fill(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas_w) w = canvas_w - x;
    if (y + h > canvas_h) h = canvas_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; ++row) {
        uint16_t *p = pixels + (size_t)row * canvas_w + x;
        for (int col = 0; col < w; ++col) *p++ = color;
    }
}

static void blit(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    api->ui_canvas_blit_rgb565(canvas, pixels + (size_t)y * canvas_w + x,
                              w, h, canvas_w, x, y, w, h);
}

static const uint8_t *glyph(char c) {
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
    };
    static const uint8_t letters[26][5] = {
        {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
        {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
        {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},
        {6,5,6,4,4},{2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
        {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},
        {7,1,2,4,7}
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    return NULL;
}

static void text(const char *s, int x, int y, int scale, uint16_t color) {
    for (; *s; ++s, x += 4 * scale) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if (g[row] & (1u << (2 - col))) fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

static void draw_hud(void) {
    char number[12];
    fill(hud_x, hud_y, hud_w, hud_h, COLOR_BG);
    int scale = hud_h >= 34 && hud_w >= 90 ? 2 : 1;
    int x = hud_x + 5, y = hud_y + 4;
    text("SCORE", x, y, scale, COLOR_MUTED);
    snprintf(number, sizeof(number), "%d", score);
    if (hud_w > 70) text(number, x, y + 7 * scale, scale, COLOR_TEXT);
    else text(number, hud_x + hud_w - ((int)strlen(number) * 4 + 1) * scale, y, scale, COLOR_TEXT);
    if (paused) text("PAUSED", x, y + 14 * scale, scale, COLOR_TEXT);
    blit(hud_x, hud_y, hud_w, hud_h);
}

static void draw_game_over(void) {
    const char *message = won ? "YOU WIN" : "GAME OVER";
    int scale = board_px >= 180 ? 2 : 1;
    int width = (int)strlen(message) * 4 * scale + 8;
    int height = 5 * scale + 8;
    int x = board_x + (board_px - width) / 2;
    int y = board_y + (board_px - height) / 2;
    fill(x, y, width, height, COLOR_BG);
    text(message, x + 4, y + 4, scale, COLOR_FOOD);
    blit(x, y, width, height);
}

static void draw_cell(int x, int y, uint16_t color, bool present) {
    int px = board_x + x * cell_px, py = board_y + y * cell_px;
    fill(px, py, cell_px, cell_px, COLOR_GRID);
    int inset = cell_px >= 4 ? 1 : 0;
    fill(px + inset, py + inset, cell_px - inset, cell_px - inset, color);
    if (present) blit(px, py, cell_px, cell_px);
}

static bool place_food(void) {
    int empty = MAX_BODY - length;
    if (!empty) return false;
    int target = (int)(game_random() % (uint32_t)empty);
    for (int y = 0; y < GRID; ++y) for (int x = 0; x < GRID; ++x) {
        if (occupied[y][x]) continue;
        if (target-- == 0) { food = (point_t){(uint8_t)x, (uint8_t)y}; return true; }
    }
    return false;
}

static void full_draw(void) {
    fill(0, 0, canvas_w, canvas_h, COLOR_BG);
    fill(board_x, board_y, board_px, board_px, COLOR_BOARD);
    for (int i = length - 1; i >= 0; --i)
        draw_cell(body[i].x, body[i].y, i ? COLOR_BODY : COLOR_HEAD, false);
    if (!won) draw_cell(food.x, food.y, COLOR_FOOD, false);
    api->ui_canvas_blit_rgb565(canvas, pixels, canvas_w, canvas_h, canvas_w, 0, 0, canvas_w, canvas_h);
    draw_hud();
}

static void reset_game(void) {
    memset(occupied, 0, sizeof(occupied));
    length = 4; score = 0; direction = queued = DIR_RIGHT;
    paused = game_over = won = false;
    int cy = GRID / 2;
    for (int i = 0; i < length; ++i) {
        body[i] = (point_t){(uint8_t)(GRID / 2 - i), (uint8_t)cy};
        occupied[cy][body[i].x] = 1;
    }
    place_food();
    if (timer && api->ui_timer_set_interval) api->ui_timer_set_interval(timer, START_MS);
    full_draw();
}

static bool opposite(direction_t a, direction_t b) { return ((int)a + 2) % 4 == (int)b; }
static void turn(direction_t d) { if (!opposite(d, direction)) queued = d; }

static void game_step(void *user) {
    (void)user;
    if (exit_requested || paused || game_over || !canvas) return;
    direction = queued;
    int nx = body[0].x, ny = body[0].y;
    if (direction == DIR_UP) --ny; else if (direction == DIR_RIGHT) ++nx;
    else if (direction == DIR_DOWN) ++ny; else --nx;
    bool eating = nx == food.x && ny == food.y;
    point_t tail = body[length - 1];
    bool hits_body = nx >= 0 && nx < GRID && ny >= 0 && ny < GRID && occupied[ny][nx];
    if (!eating && nx == tail.x && ny == tail.y) hits_body = false;
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID || hits_body) {
        game_over = true; draw_hud(); draw_game_over(); return;
    }
    point_t old_head = body[0];
    if (!eating) occupied[tail.y][tail.x] = 0;
    else if (length < MAX_BODY) ++length;
    for (int i = length - 1; i > 0; --i) body[i] = body[i - 1];
    body[0] = (point_t){(uint8_t)nx, (uint8_t)ny};
    occupied[ny][nx] = 1;
    if (!eating) draw_cell(tail.x, tail.y, COLOR_BOARD, true);
    draw_cell(old_head.x, old_head.y, COLOR_BODY, true);
    draw_cell(nx, ny, COLOR_HEAD, true);
    if (eating) {
        ++score;
        if (!place_food()) { won = game_over = true; draw_game_over(); }
        else draw_cell(food.x, food.y, COLOR_FOOD, true);
        if (timer && api->ui_timer_set_interval) {
            int interval = START_MS - score * 6;
            api->ui_timer_set_interval(timer, interval < MIN_MS ? MIN_MS : interval);
        }
        draw_hud();
    }
}

static void snake_start(void) {
    exit_requested = false; gh_touch_reset(&touch_state);
    if (!api->ui_canvas_create || !api->ui_canvas_blit_rgb565 ||
        !api->ui_screen_get_content_width || !api->ui_screen_get_content_height ||
        !api->ui_timer_create || !api->ui_timer_delete || !api->request_exit) {
        GH_VOID(api, toast, "Snake requires the RGB565 canvas API"); request_exit(); return;
    }
    ghostesp_ui_obj_t screen = api->ui_screen_create("Snake");
    if (!screen) { request_exit(); return; }
    GH_VOID(api, ui_obj_set_scrollable, screen, false);
    GH_VOID(api, ui_obj_set_bg_color, screen, COLOR_BG);
    GH_VOID(api, ui_obj_set_pad, screen, 0, 0, 0, 0);
    GH_VOID(api, ui_obj_set_flex_flow, screen, GHOSTESP_FLEX_FLOW_NONE);
    touch_bar = gh_touch_bar(api, true, touch_back, NULL);
    canvas_w = api->ui_screen_get_content_width();
    canvas_h = api->ui_screen_get_content_height() - (touch_bar ? TOUCH_BAR_HEIGHT : 0);
    int portrait = canvas_h >= canvas_w;
    int reserve = portrait ? 24 : 64;
    int available = portrait ? canvas_h - reserve : canvas_w - reserve;
    int short_side = portrait ? canvas_w : canvas_h;
    cell_px = (available < short_side ? available : short_side) / GRID;
    if (cell_px < 1) { GH_VOID(api, toast, "Display is too small for Snake"); request_exit(); return; }
    board_px = cell_px * GRID;
    if (portrait) {
        board_x = (canvas_w - board_px) / 2; board_y = reserve + (canvas_h - reserve - board_px) / 2;
        hud_x = 0; hud_y = 0; hud_w = canvas_w; hud_h = board_y;
    } else {
        board_x = (canvas_w - reserve - board_px) / 2; board_y = (canvas_h - board_px) / 2;
        hud_x = board_x + board_px; hud_y = 0; hud_w = canvas_w - hud_x; hud_h = canvas_h;
    }
    canvas = api->ui_canvas_create(screen, canvas_w, canvas_h);
    if (!canvas) { request_exit(); return; }
    GH_VOID(api, ui_obj_set_pos, canvas, 0, 0);
    pixels_size = (size_t)canvas_w * canvas_h * sizeof(*pixels);
    pixels = malloc(pixels_size);
    if (!pixels) { request_exit(); return; }
    rng_state = api->system_uptime_us ? (uint32_t)api->system_uptime_us() : 0x534E414Bu;
    if (!rng_state) rng_state = 0x534E414Bu;
    timer = api->ui_timer_create(game_step, START_MS, NULL);
    if (!timer) { request_exit(); return; }
    reset_game();
}

static void snake_stop(void) {
    if (timer && api->ui_timer_delete) api->ui_timer_delete(timer);
    timer = NULL; canvas = NULL; touch_bar = NULL;
    free(pixels); pixels = NULL; pixels_size = 0;
}

static void activate(void) {
    if (game_over) reset_game();
    else { paused = !paused; draw_hud(); }
}

static void snake_input(const ghostesp_input_event_t *event) {
    if (!event || exit_requested) return;
    if (event->type == GHOSTESP_INPUT_BACK && event->pressed) { request_exit(); return; }
    if (event->type == GHOSTESP_INPUT_TOUCH) {
        bool tap = false;
        ghostesp_input_type_t swipe = gh_touch_update_tap(&touch_state, event, &tap);
        if (swipe == GHOSTESP_INPUT_UP) turn(DIR_UP); else if (swipe == GHOSTESP_INPUT_RIGHT) turn(DIR_RIGHT);
        else if (swipe == GHOSTESP_INPUT_DOWN) turn(DIR_DOWN); else if (swipe == GHOSTESP_INPUT_LEFT) turn(DIR_LEFT);
        else if (tap) activate();
        return;
    }
    if (!event->pressed) return;
    if (event->type == GHOSTESP_INPUT_UP) turn(DIR_UP); else if (event->type == GHOSTESP_INPUT_RIGHT) turn(DIR_RIGHT);
    else if (event->type == GHOSTESP_INPUT_DOWN) turn(DIR_DOWN); else if (event->type == GHOSTESP_INPUT_LEFT) turn(DIR_LEFT);
    else if (event->type == GHOSTESP_INPUT_SELECT) activate();
    else if (event->type == GHOSTESP_INPUT_KEY) {
        int k = event->value;
        if (k == 27 || k == 8 || k == 127 || k == 'q' || k == 'Q') request_exit();
        else if (k == 'w' || k == 'W' || k == 'k' || k == 'K') turn(DIR_UP);
        else if (k == 'd' || k == 'D' || k == 'l' || k == 'L') turn(DIR_RIGHT);
        else if (k == 's' || k == 'S' || k == 'j' || k == 'J') turn(DIR_DOWN);
        else if (k == 'a' || k == 'A' || k == 'h' || k == 'H') turn(DIR_LEFT);
        else if (k == ' ' || k == '\r' || k == '\n' || k == 'p' || k == 'P') activate();
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "snake", "Snake", snake_start, snake_stop, snake_input, NULL
);
#define SNAKE_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, request_exit) + sizeof(((ghostesp_api_t *)0)->request_exit))
GHOSTESP_APP_INIT_WITH_API(app, api, "snake", SNAKE_REQUIRED_API_SIZE)
