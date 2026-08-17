/* Native Pong for GhostESP. The game renders to a compact high-resolution
   framebuffer and scales to the complete available display. */
#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int game_width;
static int game_height;
static bool landscape_layout;
#define GAME_W game_width
#define GAME_H game_height
#define PADDLE_W (landscape_layout ? 40 : 22)
#define PADDLE_H 3
#define CPU_Y 7
#define PLAYER_Y (GAME_H - (landscape_layout ? 10 : 10))
#define MOVE_STEP 2
#define FRAME_MS 16
#define TOUCH_BAR_HEIGHT 34

#define COLOR_COURT 0x0841
#define COLOR_LINE 0x39E7
#define COLOR_WHITE 0xFFFF
#define COLOR_PLAYER 0x07FF
#define COLOR_CPU 0xFD20
#define COLOR_BALL_GLOW 0x7BEF

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
static ghostesp_touch_state_t touch_state;
static uint32_t rng_state;
static uint32_t frame_accumulator;
static uint32_t last_frame_ms;
static int player_x;
static int cpu_x;
static int ball_x;
static int ball_y;
static int ball_dx;
static int ball_dy;
static int cpu_aim_offset;
static int player_score;
static int cpu_score;
static int rendered_player_x;
static int rendered_cpu_x;
static int rendered_ball_x;
static int rendered_ball_y;
static int rendered_player_score;
static int rendered_cpu_score;
static int touch_target;
static bool snapshot_input;
static bool left_held;
static bool right_held;
static bool touch_active;
static bool exit_requested;

static void pong_frame(void *user);

static uint32_t pong_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int clamp_paddle(int x) {
    if (x < 1) return 1;
    if (x > GAME_W - PADDLE_W - 1) return GAME_W - PADDLE_W - 1;
    return x;
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

static void serve_ball(int direction) {
    ball_x = GAME_W / 2;
    ball_y = GAME_H / 2;
    ball_dx = (pong_random() & 1u) ? MOVE_STEP : -MOVE_STEP;
    ball_dy = direction * MOVE_STEP;
    cpu_aim_offset = ((int)(pong_random() % 7u) - 3) * MOVE_STEP;
}

static void reset_game(void) {
    player_x = (GAME_W - PADDLE_W) / 2;
    cpu_x = player_x;
    player_score = 0;
    cpu_score = 0;
    serve_ball((pong_random() & 1u) ? 1 : -1);
}

static void fill_rect(int x, int y, int width, int height, uint16_t color) {
    int x2 = x + width;
    int y2 = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > GAME_W) x2 = GAME_W;
    if (y2 > GAME_H) y2 = GAME_H;
    if (x >= x2 || y >= y2) return;

    int left = view_left + x * view_width / GAME_W;
    int right = view_left + x2 * view_width / GAME_W;
    int top = view_top + y * view_height / GAME_H;
    int bottom = view_top + y2 * view_height / GAME_H;
    if (right <= left) right = left + 1;
    if (bottom <= top) bottom = top + 1;
    for (int row = top; row < bottom; row++) {
        uint16_t *destination = framebuffer + (size_t)row * canvas_width + left;
        for (int column = left; column < right; column++) *destination++ = color;
    }
}

static void set_pixel(int x, int y, uint16_t color) {
    fill_rect(x, y, 1, 1, color);
}

static void draw_digit(int digit, int x, int y_offset) {
    static const uint8_t glyphs[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
        {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
        {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    };
    digit %= 10;
    for (int y = 0; y < 5; y++)
        for (int bit = 0; bit < 3; bit++)
            if (glyphs[digit][y] & (1u << (2 - bit))) {
                fill_rect(x + bit * 2, y_offset + y * 2, 2, 2, COLOR_WHITE);
            }
}

static void render_frame(void) {
    memset(framebuffer, 0, framebuffer_size);
    for (int y = 1; y < GAME_H - 1; y++) {
        set_pixel(1, y, COLOR_COURT);
        set_pixel(GAME_W - 2, y, COLOR_COURT);
    }
    for (int x = 2; x < GAME_W - 2; x += 6) {
        fill_rect(x, GAME_H / 2, 3, 1, COLOR_LINE);
    }
    for (int y = GAME_H / 2 - 8; y <= GAME_H / 2 + 8; y++) {
        int dx = y - GAME_H / 2;
        if (dx < 0) dx = -dx;
        if (dx == 8 || dx == 7) {
            set_pixel(GAME_W / 2 - 2, y, COLOR_COURT);
            set_pixel(GAME_W / 2 + 2, y, COLOR_COURT);
        }
    }
    fill_rect(player_x, PLAYER_Y, PADDLE_W, PADDLE_H, COLOR_PLAYER);
    fill_rect(player_x + 2, PLAYER_Y, PADDLE_W - 4, 1, COLOR_WHITE);
    fill_rect(cpu_x, CPU_Y, PADDLE_W, PADDLE_H, COLOR_CPU);
    fill_rect(cpu_x + 2, CPU_Y + PADDLE_H - 1, PADDLE_W - 4, 1, COLOR_WHITE);

    fill_rect(ball_x - 2, ball_y - 1, 5, 3, COLOR_BALL_GLOW);
    fill_rect(ball_x - 1, ball_y - 2, 3, 5, COLOR_BALL_GLOW);
    fill_rect(ball_x - 1, ball_y - 1, 3, 3, COLOR_WHITE);
    draw_digit(cpu_score, GAME_W / 2 - 3, landscape_layout ? 18 : 22);
    draw_digit(player_score, GAME_W / 2 - 3, GAME_H - (landscape_layout ? 18 : 34));

}

static void blit_game_region(int x, int y, int width, int height) {
    int left = view_left + x * view_width / GAME_W;
    int top = view_top + y * view_height / GAME_H;
    int right = view_left + (x + width) * view_width / GAME_W;
    int bottom = view_top + (y + height) * view_height / GAME_H;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > canvas_width) right = canvas_width;
    if (bottom > canvas_height) bottom = canvas_height;
    if (left >= right || top >= bottom) return;

    api->ui_canvas_blit_rgb565(canvas, framebuffer + (size_t)top * canvas_width + left,
                              right - left, bottom - top, canvas_width,
                              left, top, right - left, bottom - top);
}

static void present(void) {
    render_frame();
    api->ui_canvas_blit_rgb565(canvas, framebuffer,
                              canvas_width, canvas_height, canvas_width,
                              0, 0, canvas_width, canvas_height);
    rendered_player_x = player_x;
    rendered_cpu_x = cpu_x;
    rendered_ball_x = ball_x;
    rendered_ball_y = ball_y;
    rendered_player_score = player_score;
    rendered_cpu_score = cpu_score;
}

static void present_dirty(void) {
    render_frame();

    int left = rendered_player_x < player_x ? rendered_player_x : player_x;
    int right = rendered_player_x > player_x ? rendered_player_x : player_x;
    blit_game_region(left, PLAYER_Y, right - left + PADDLE_W, PADDLE_H);

    left = rendered_cpu_x < cpu_x ? rendered_cpu_x : cpu_x;
    right = rendered_cpu_x > cpu_x ? rendered_cpu_x : cpu_x;
    blit_game_region(left, CPU_Y, right - left + PADDLE_W, PADDLE_H);

    left = rendered_ball_x < ball_x ? rendered_ball_x : ball_x;
    right = rendered_ball_x > ball_x ? rendered_ball_x : ball_x;
    int top = rendered_ball_y < ball_y ? rendered_ball_y : ball_y;
    int bottom = rendered_ball_y > ball_y ? rendered_ball_y : ball_y;
    blit_game_region(left - 2, top - 2, right - left + 5, bottom - top + 5);

    if (rendered_cpu_score != cpu_score) blit_game_region(GAME_W / 2 - 3, landscape_layout ? 18 : 22, 6, 10);
    if (rendered_player_score != player_score) blit_game_region(GAME_W / 2 - 3, GAME_H - (landscape_layout ? 18 : 34), 6, 10);
    rendered_player_x = player_x;
    rendered_cpu_x = cpu_x;
    rendered_ball_x = ball_x;
    rendered_ball_y = ball_y;
    rendered_player_score = player_score;
    rendered_cpu_score = cpu_score;
}

static void update_player(void) {
    bool left = left_held;
    bool right = right_held;
    if (snapshot_input) {
        ghostesp_input_snapshot_t snapshot;
        if (api->input_snapshot(&snapshot)) {
            left = (snapshot.held & GHOSTESP_BUTTON_LEFT) != 0;
            right = (snapshot.held & GHOSTESP_BUTTON_RIGHT) != 0;
        } else {
            left = false;
            right = false;
        }
    }
    if (touch_active) {
        int center = player_x + PADDLE_W / 2;
        if (touch_target < center) left = true;
        if (touch_target > center) right = true;
    }
    if (left != right) player_x = clamp_paddle(player_x + (right ? MOVE_STEP : -MOVE_STEP));
}

static void update_cpu(void) {
    int target = GAME_W / 2;
    if (ball_dy < 0 && ball_y < GAME_H * 2 / 3) target = ball_x + cpu_aim_offset;
    int center = cpu_x + PADDLE_W / 2;
    if (target < center - MOVE_STEP) cpu_x -= MOVE_STEP;
    else if (target > center + MOVE_STEP) cpu_x += MOVE_STEP;
    cpu_x = clamp_paddle(cpu_x);
}

static void update_ball(void) {
    int next_x = ball_x + ball_dx;
    int next_y = ball_y + ball_dy;
    if (next_x <= 3 || next_x >= GAME_W - 4) {
        ball_dx = -ball_dx;
        next_x = ball_x + ball_dx;
    }
    if (ball_dy > 0 && next_y + 2 >= PLAYER_Y && ball_y + 2 <= PLAYER_Y &&
        next_x >= player_x - 1 && next_x <= player_x + PADDLE_W) {
        ball_dy = -MOVE_STEP;
        ball_dx = next_x < player_x + PADDLE_W / 2 ? -MOVE_STEP : MOVE_STEP;
        next_y = PLAYER_Y - 3;
    } else if (ball_dy < 0 && next_y - 2 <= CPU_Y + PADDLE_H && ball_y >= CPU_Y &&
               next_x >= cpu_x - 1 && next_x <= cpu_x + PADDLE_W) {
        ball_dy = MOVE_STEP;
        ball_dx = next_x < cpu_x + PADDLE_W / 2 ? -MOVE_STEP : MOVE_STEP;
        next_y = CPU_Y + PADDLE_H + 2;
        cpu_aim_offset = ((int)(pong_random() % 9u) - 4) * MOVE_STEP;
    }
    ball_x = next_x;
    ball_y = next_y;
    if (ball_y < 0) {
        player_score = (player_score + 1) % 10;
        serve_ball(1);
    } else if (ball_y >= GAME_H) {
        cpu_score = (cpu_score + 1) % 10;
        serve_ball(-1);
    }
}

static void pong_start(void) {
    snapshot_input = api->input_snapshot != NULL;
    exit_requested = false;
    left_held = false;
    right_held = false;
    touch_active = false;
    frame_accumulator = 0;
    gh_touch_reset(&touch_state);

    if (!api->ui_canvas_create || !api->ui_canvas_blit_rgb565 ||
        !api->ui_screen_get_content_width || !api->ui_screen_get_content_height ||
        !api->ui_timer_create || !api->ui_timer_delete || !api->request_exit) {
        if (api->toast) api->toast("Pong requires the RGB565 canvas API");
        request_exit();
        return;
    }

    ghostesp_ui_obj_t screen = api->ui_screen_create("Pong");
    if (!screen) { request_exit(); return; }
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_bg_color(screen, 0x000000);
    GH_VOID(api, ui_obj_set_pad, screen, 0, 0, 0, 0);
    GH_VOID(api, ui_obj_set_flex_flow, screen, GHOSTESP_FLEX_FLOW_NONE);
    touch_bar = gh_touch_bar(api, true, touch_back, NULL);

    int width = api->ui_screen_get_content_width();
    int height = api->ui_screen_get_content_height();
    api->ui_obj_set_size(screen, width, height);

    int usable_height = height - (touch_bar ? TOUCH_BAR_HEIGHT : 0);
    landscape_layout = width > usable_height;
    game_width = landscape_layout ? 240 : 96;
    game_height = landscape_layout ? 100 : 144;
    canvas_width = width;
    canvas_height = usable_height;
    view_width = usable_height * GAME_W / GAME_H;
    view_height = usable_height;
    if (view_width > width) {
        view_width = width;
        view_height = width * GAME_H / GAME_W;
    }
    view_left = (width - view_width) / 2;
    view_top = (usable_height - view_height) / 2;

    canvas = api->ui_canvas_create(screen, canvas_width, canvas_height);
    if (!canvas) { request_exit(); return; }
    framebuffer_size = (size_t)canvas_width * canvas_height * sizeof(*framebuffer);
    framebuffer = malloc(framebuffer_size);
    if (!framebuffer) { request_exit(); return; }
    api->ui_canvas_fill(canvas, 0x000000);
    if (api->ui_obj_set_pos)
        api->ui_obj_set_pos(canvas, 0, 0);
    rng_state = api->system_uptime_us ? (uint32_t)api->system_uptime_us() : 0x504F4E47u;
    if (!rng_state) rng_state = 0x504F4E47u;
    reset_game();
    present();
    last_frame_ms = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    frame_timer = api->ui_timer_create(pong_frame, FRAME_MS, NULL);
    if (!frame_timer) request_exit();
}

static void pong_stop(void) {
    if (frame_timer && api->ui_timer_delete) api->ui_timer_delete(frame_timer);
    frame_timer = NULL;
    canvas = NULL;
    touch_bar = NULL;
    free(framebuffer);
    framebuffer = NULL;
    framebuffer_size = 0;
}

static void pong_input(const ghostesp_input_event_t *event) {
    if (!event || exit_requested) return;
    if (event->type == GHOSTESP_INPUT_BACK && event->pressed) { request_exit(); return; }

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_LEFT)
            player_x = clamp_paddle(player_x - 8);
        else if (swipe == GHOSTESP_INPUT_RIGHT)
            player_x = clamp_paddle(player_x + 8);
        touch_active = event->pressed;
        touch_target = ((event->x - view_left) * GAME_W) / view_width;
        if (touch_target < 0) touch_target = 0;
        if (touch_target >= GAME_W) touch_target = GAME_W - 1;
        return;
    }
    if (event->type == GHOSTESP_INPUT_KEY) {
        if (!event->pressed) return;
        int key = event->value;
        if (key == 27 || key == 8 || key == 127 || key == 'q' || key == 'Q') request_exit();
        else if (key == 'a' || key == 'A' || key == 'h' || key == 'H' ||
                 key == ',' || key == ';') player_x = clamp_paddle(player_x - 4);
        else if (key == 'd' || key == 'D' || key == 'l' || key == 'L' ||
                 key == '/' || key == '.') player_x = clamp_paddle(player_x + 4);
        return;
    }
    if (event->type == GHOSTESP_INPUT_LEFT) {
        left_held = event->pressed;
        if (snapshot_input && event->pressed) player_x = clamp_paddle(player_x - MOVE_STEP);
    } else if (event->type == GHOSTESP_INPUT_RIGHT) {
        right_held = event->pressed;
        if (snapshot_input && event->pressed) player_x = clamp_paddle(player_x + MOVE_STEP);
    }
}

static void pong_frame(void *user) {
    (void)user;
    if (exit_requested || !canvas) return;
    uint32_t now_ms = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    uint32_t elapsed_ms = last_frame_ms && now_ms ? now_ms - last_frame_ms : FRAME_MS;
    last_frame_ms = now_ms;
    frame_accumulator += elapsed_ms;
    if (frame_accumulator > FRAME_MS * 4) frame_accumulator = FRAME_MS * 4;
    bool updated = false;
    while (frame_accumulator >= FRAME_MS) {
        frame_accumulator -= FRAME_MS;
        update_player();
        update_cpu();
        update_ball();
        updated = true;
    }
    if (updated) present_dirty();
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "pong", "Pong", pong_start, pong_stop, pong_input, NULL
);

#define PONG_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, request_exit) + sizeof(((ghostesp_api_t *)0)->request_exit))

GHOSTESP_APP_INIT_WITH_API(app, api, "pong", PONG_REQUIRED_API_SIZE)
