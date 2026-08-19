#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GRID_W 120
#define MAX_GRID_H 72
static int grid_width;
static int grid_height;
static bool landscape_layout;
#define GRID_W grid_width
#define GRID_H grid_height
#define TOUCH_BAR_HEIGHT 34
#define FRAME_MS 55
#define ROUND_PAUSE_TICKS 18

#define COLOR_BG 0x0000
#define COLOR_GRID 0x0841
#define COLOR_PLAYER 0x07FF
#define COLOR_PLAYER_HEAD 0xFFFF
#define COLOR_CPU 0xF81F
#define COLOR_CPU_HEAD 0xFFE0

typedef enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT } direction_t;

typedef struct {
    int x;
    int y;
    direction_t direction;
    direction_t queued_direction;
} rider_t;

static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t touch_bar;
static ghostesp_ui_timer_t frame_timer;
static int canvas_width;
static int canvas_height;
static int view_width;
static int view_height;
static int view_left;
static int view_top;
static uint16_t *framebuffer;
static size_t framebuffer_size;
static uint8_t arena[MAX_GRID_H][MAX_GRID_W];
static rider_t player;
static rider_t cpu;
static ghostesp_touch_state_t touch_state;
static uint32_t rng_state;
static int round_pause;
static bool exit_requested;

static uint32_t game_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static bool opposite(direction_t a, direction_t b) {
    return ((int)a + 2) % 4 == (int)b;
}

static void request_exit(void) {
    if (exit_requested) return;
    exit_requested = true;
    GH_VOID(api, request_exit);
}

static void touch_back(void *user) {
    (void)user;
    request_exit();
}

static void queue_player_direction(direction_t direction) {
    if (!opposite(direction, player.direction)) player.queued_direction = direction;
}

static void cell_bounds(int x, int y, int *left, int *top, int *right, int *bottom) {
    *left = view_left + x * view_width / GRID_W;
    *right = view_left + (x + 1) * view_width / GRID_W;
    *top = view_top + y * view_height / GRID_H;
    *bottom = view_top + (y + 1) * view_height / GRID_H;
}

static void draw_cell(int x, int y, uint16_t color) {
    int left, top, right, bottom;
    cell_bounds(x, y, &left, &top, &right, &bottom);
    for (int row = top; row < bottom; ++row) {
        uint16_t *pixel = framebuffer + (size_t)row * canvas_width + left;
        for (int column = left; column < right; ++column) *pixel++ = color;
    }
}

static void blit_cell(int x, int y) {
    int left, top, right, bottom;
    cell_bounds(x, y, &left, &top, &right, &bottom);
    api->ui_canvas_blit_rgb565(canvas, framebuffer + (size_t)top * canvas_width + left,
                              right - left, bottom - top, canvas_width,
                              left, top, right - left, bottom - top);
}

static void fill_pixels(int x, int y, int width, int height, uint16_t color) {
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > canvas_width) width = canvas_width - x;
    if (y + height > canvas_height) height = canvas_height - y;
    if (width <= 0 || height <= 0) return;
    for (int row = y; row < y + height; ++row) {
        uint16_t *pixel = framebuffer + (size_t)row * canvas_width + x;
        for (int column = 0; column < width; ++column) *pixel++ = color;
    }
}

static void draw_result_text(const char *text, uint16_t color) {
    static const uint8_t glyphs[][5] = {
        ['A' - 'A'] = {2, 5, 7, 5, 5}, ['D' - 'A'] = {6, 5, 5, 5, 6},
        ['E' - 'A'] = {7, 4, 6, 4, 7}, ['I' - 'A'] = {7, 2, 2, 2, 7},
        ['L' - 'A'] = {4, 4, 4, 4, 7}, ['N' - 'A'] = {5, 7, 7, 7, 5},
        ['O' - 'A'] = {7, 5, 5, 5, 7}, ['S' - 'A'] = {7, 4, 7, 1, 7},
        ['R' - 'A'] = {6, 5, 6, 5, 5},
        ['U' - 'A'] = {5, 5, 5, 5, 7}, ['W' - 'A'] = {5, 5, 7, 7, 5},
        ['Y' - 'A'] = {5, 5, 2, 2, 2},
    };
    int length = (int)strlen(text);
    int scale = view_width / 72;
    if (scale < 2) scale = 2;
    int char_width = 4 * scale;
    int box_width = length * char_width + 4 * scale;
    int box_height = 9 * scale;
    int left = view_left + (view_width - box_width) / 2;
    int top = view_top + (view_height - box_height) / 2;
    fill_pixels(left, top, box_width, box_height, COLOR_BG);
    for (int i = 0; i < length; ++i) {
        if (text[i] == ' ') continue;
        const uint8_t *glyph = glyphs[text[i] - 'A'];
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (glyph[row] & (1u << (2 - column))) {
                    fill_pixels(left + 2 * scale + i * char_width + column * scale,
                                top + 2 * scale + row * scale, scale, scale, color);
                }
            }
        }
    }
    api->ui_canvas_blit_rgb565(canvas, framebuffer + (size_t)top * canvas_width + left,
                              box_width, box_height, canvas_width,
                              left, top, box_width, box_height);
}

static void draw_arena(void) {
    memset(framebuffer, 0, framebuffer_size);
    for (int y = 0; y < GRID_H; ++y) {
        for (int x = 0; x < GRID_W; ++x) {
            if ((x % 8 == 0 || y % 8 == 0) && arena[y][x] == 0) draw_cell(x, y, COLOR_GRID);
        }
    }
    draw_cell(player.x, player.y, COLOR_PLAYER_HEAD);
    draw_cell(cpu.x, cpu.y, COLOR_CPU_HEAD);
    api->ui_canvas_blit_rgb565(canvas, framebuffer, canvas_width, canvas_height, canvas_width,
                              0, 0, canvas_width, canvas_height);
}

static void reset_round(void) {
    memset(arena, 0, sizeof(arena));
    player = (rider_t){ .x = GRID_W / 4, .y = GRID_H / 2,
                        .direction = DIR_UP, .queued_direction = DIR_UP };
    cpu = (rider_t){ .x = GRID_W * 3 / 4, .y = GRID_H / 2,
                     .direction = DIR_DOWN, .queued_direction = DIR_DOWN };
    arena[player.y][player.x] = 1;
    arena[cpu.y][cpu.x] = 2;
    round_pause = 0;
    draw_arena();
}

static void next_position(const rider_t *rider, direction_t direction, int *x, int *y) {
    *x = rider->x;
    *y = rider->y;
    if (direction == DIR_UP) --*y;
    else if (direction == DIR_RIGHT) ++*x;
    else if (direction == DIR_DOWN) ++*y;
    else --*x;
}

static bool open_cell(int x, int y) {
    return x > 0 && x < GRID_W - 1 && y > 0 && y < GRID_H - 1 && arena[y][x] == 0;
}

static direction_t choose_cpu_direction(void) {
    direction_t choices[3] = {
        cpu.direction,
        (direction_t)(((int)cpu.direction + 3) % 4),
        (direction_t)(((int)cpu.direction + 1) % 4),
    };
    if (game_random() & 1u) {
        direction_t swap = choices[1];
        choices[1] = choices[2];
        choices[2] = swap;
    }
    for (int i = 0; i < 3; ++i) {
        int x, y;
        next_position(&cpu, choices[i], &x, &y);
        if (open_cell(x, y)) return choices[i];
    }
    return cpu.direction;
}

static void game_step(void *user) {
    (void)user;
    if (exit_requested || !canvas) return;
    if (round_pause > 0) {
        if (--round_pause == 0) reset_round();
        return;
    }

    player.direction = player.queued_direction;
    cpu.direction = choose_cpu_direction();
    int player_x, player_y, cpu_x, cpu_y;
    next_position(&player, player.direction, &player_x, &player_y);
    next_position(&cpu, cpu.direction, &cpu_x, &cpu_y);
    bool same_cell = player_x == cpu_x && player_y == cpu_y;
    bool player_crashed = same_cell || !open_cell(player_x, player_y);
    bool cpu_crashed = same_cell || !open_cell(cpu_x, cpu_y);

    draw_cell(player.x, player.y, COLOR_PLAYER);
    blit_cell(player.x, player.y);
    draw_cell(cpu.x, cpu.y, COLOR_CPU);
    blit_cell(cpu.x, cpu.y);
    if (player_crashed || cpu_crashed) {
        if (player_crashed && cpu_crashed) draw_result_text("DRAW", COLOR_PLAYER_HEAD);
        else if (cpu_crashed) draw_result_text("YOU WIN", COLOR_PLAYER_HEAD);
        else draw_result_text("YOU LOSE", COLOR_CPU_HEAD);
        round_pause = ROUND_PAUSE_TICKS;
        return;
    }

    player.x = player_x;
    player.y = player_y;
    cpu.x = cpu_x;
    cpu.y = cpu_y;
    arena[player.y][player.x] = 1;
    arena[cpu.y][cpu.x] = 2;
    draw_cell(player.x, player.y, COLOR_PLAYER_HEAD);
    blit_cell(player.x, player.y);
    draw_cell(cpu.x, cpu.y, COLOR_CPU_HEAD);
    blit_cell(cpu.x, cpu.y);
}

static void lightcycles_start(void) {
    exit_requested = false;
    gh_touch_reset(&touch_state);
    if (!api->ui_canvas_create || !api->ui_canvas_blit_rgb565 ||
        !api->ui_screen_get_content_width || !api->ui_screen_get_content_height ||
        !api->ui_timer_create || !api->ui_timer_delete || !api->request_exit) {
        if (api->toast) api->toast("Light Cycles requires the RGB565 canvas API");
        request_exit();
        return;
    }

    ghostesp_ui_obj_t screen = api->ui_screen_create("Light Cycles");
    if (!screen) { request_exit(); return; }
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_bg_color(screen, COLOR_BG);
    GH_VOID(api, ui_obj_set_pad, screen, 0, 0, 0, 0);
    GH_VOID(api, ui_obj_set_flex_flow, screen, GHOSTESP_FLEX_FLOW_NONE);
    touch_bar = gh_touch_bar(api, true, touch_back, NULL);
    int screen_width = api->ui_screen_get_content_width();
    int usable_height = api->ui_screen_get_content_height() - (touch_bar ? TOUCH_BAR_HEIGHT : 0);
    landscape_layout = screen_width > usable_height;
    grid_width = landscape_layout ? 120 : 48;
    grid_height = landscape_layout ? 50 : 72;
    canvas_width = screen_width;
    canvas_height = usable_height;
    view_width = usable_height * GRID_W / GRID_H;
    view_height = usable_height;
    if (view_width > screen_width) {
        view_width = screen_width;
        view_height = screen_width * GRID_H / GRID_W;
    }
    view_left = (screen_width - view_width) / 2;
    view_top = (usable_height - view_height) / 2;
    if (view_width < GRID_W || view_height < GRID_H) {
        if (api->toast) api->toast("Display is too small for Light Cycles");
        request_exit();
        return;
    }
    canvas = api->ui_canvas_create(screen, canvas_width, canvas_height);
    if (!canvas) { request_exit(); return; }
    if (api->ui_obj_set_pos) api->ui_obj_set_pos(canvas, 0, 0);
    framebuffer_size = (size_t)canvas_width * canvas_height * sizeof(*framebuffer);
    framebuffer = malloc(framebuffer_size);
    if (!framebuffer) { request_exit(); return; }
    rng_state = api->system_uptime_us ? (uint32_t)api->system_uptime_us() : 0x54524F4Eu;
    if (!rng_state) rng_state = 0x54524F4Eu;
    reset_round();
    frame_timer = api->ui_timer_create(game_step, FRAME_MS, NULL);
    if (!frame_timer) request_exit();
}

static void lightcycles_stop(void) {
    if (frame_timer && api->ui_timer_delete) api->ui_timer_delete(frame_timer);
    frame_timer = NULL;
    canvas = NULL;
    touch_bar = NULL;
    free(framebuffer);
    framebuffer = NULL;
    framebuffer_size = 0;
}

static void lightcycles_input(const ghostesp_input_event_t *event) {
    if (!event || exit_requested) return;
    if (event->type == GHOSTESP_INPUT_BACK && event->pressed) { request_exit(); return; }
    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update_live(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_UP) queue_player_direction(DIR_UP);
        else if (swipe == GHOSTESP_INPUT_RIGHT) queue_player_direction(DIR_RIGHT);
        else if (swipe == GHOSTESP_INPUT_DOWN) queue_player_direction(DIR_DOWN);
        else if (swipe == GHOSTESP_INPUT_LEFT) queue_player_direction(DIR_LEFT);
        return;
    }
    if (!event->pressed) return;
    if (event->type == GHOSTESP_INPUT_UP) queue_player_direction(DIR_UP);
    else if (event->type == GHOSTESP_INPUT_RIGHT) queue_player_direction(DIR_RIGHT);
    else if (event->type == GHOSTESP_INPUT_DOWN) queue_player_direction(DIR_DOWN);
    else if (event->type == GHOSTESP_INPUT_LEFT) queue_player_direction(DIR_LEFT);
    else if (event->type == GHOSTESP_INPUT_KEY) {
        int key = event->value;
        if (key == 27 || key == 8 || key == 127 || key == 'q' || key == 'Q') request_exit();
        else if (key == 'w' || key == 'W' || key == 'k' || key == 'K') queue_player_direction(DIR_UP);
        else if (key == 'd' || key == 'D' || key == 'l' || key == 'L') queue_player_direction(DIR_RIGHT);
        else if (key == 's' || key == 'S' || key == 'j' || key == 'J') queue_player_direction(DIR_DOWN);
        else if (key == 'a' || key == 'A' || key == 'h' || key == 'H') queue_player_direction(DIR_LEFT);
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "lightcycles", "Light Cycles", lightcycles_start, lightcycles_stop, lightcycles_input, NULL
);

#define LIGHTCYCLES_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, request_exit) + sizeof(((ghostesp_api_t *)0)->request_exit))

GHOSTESP_APP_INIT_WITH_API(app, api, "lightcycles", LIGHTCYCLES_REQUIRED_API_SIZE)
