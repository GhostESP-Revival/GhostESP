/* Native Pong for GhostESP. The game renders at 64x96 and scales to the
   available display, keeping frame transfers small on single-core targets. */
#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <string.h>

#define GAME_W 64
#define GAME_H 96
#define PADDLE_W 14
#define CPU_Y 4
#define PLAYER_Y 90
#define MOVE_STEP 2
#define FRAME_MS 24
#define TOUCH_BAR_HEIGHT 34

static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t touch_bar;
static int canvas_width;
static int canvas_height;
static int canvas_left;
static uint16_t framebuffer[GAME_W * GAME_H];
static ghostesp_touch_state_t touch_state;
static uint32_t rng_state;
static uint32_t frame_accumulator;
static int player_x;
static int cpu_x;
static int ball_x;
static int ball_y;
static int ball_dx;
static int ball_dy;
static int cpu_aim_offset;
static int player_score;
static int cpu_score;
static int touch_target;
static bool snapshot_input;
static bool left_held;
static bool right_held;
static bool touch_active;
static bool exit_requested;

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
    GH_VOID(api, app_exit);
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

static void set_pixel(int x, int y) {
    if (x >= 0 && x < GAME_W && y >= 0 && y < GAME_H)
        framebuffer[y * GAME_W + x] = 0xFFFF;
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
                set_pixel(x + bit * 2, y_offset + y * 2);
                set_pixel(x + bit * 2 + 1, y_offset + y * 2);
                set_pixel(x + bit * 2, y_offset + y * 2 + 1);
                set_pixel(x + bit * 2 + 1, y_offset + y * 2 + 1);
            }
}

static void present(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
    for (int x = 2; x < GAME_W - 2; x += 4) {
        set_pixel(x, GAME_H / 2);
        set_pixel(x + 1, GAME_H / 2);
    }
    for (int x = 0; x < PADDLE_W; x++) {
        set_pixel(player_x + x, PLAYER_Y);
        set_pixel(cpu_x + x, CPU_Y);
    }
    for (int y = -2; y <= 1; y++) {
        int x_start = (y == -2 || y == 1) ? -1 : -2;
        int x_end = (y == -2 || y == 1) ? 0 : 1;
        for (int x = x_start; x <= x_end; x++) set_pixel(ball_x + x, ball_y + y);
    }
    draw_digit(cpu_score, GAME_W / 2 - 3, 14);
    draw_digit(player_score, GAME_W / 2 - 3, GAME_H - 24);
    api->ui_canvas_blit_rgb565(canvas, framebuffer, GAME_W, GAME_H, GAME_W,
                               0, 0, canvas_width, canvas_height);
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
    if (ball_dy > 0 && next_y + 1 >= PLAYER_Y && ball_y + 1 <= PLAYER_Y &&
        next_x >= player_x - 1 && next_x <= player_x + PADDLE_W) {
        ball_dy = -MOVE_STEP;
        ball_dx = next_x < player_x + PADDLE_W / 2 ? -MOVE_STEP : MOVE_STEP;
        next_y = PLAYER_Y - 4;
    } else if (ball_dy < 0 && next_y <= CPU_Y + 1 && ball_y >= CPU_Y &&
               next_x >= cpu_x - 1 && next_x <= cpu_x + PADDLE_W) {
        ball_dy = MOVE_STEP;
        ball_dx = next_x < cpu_x + PADDLE_W / 2 ? -MOVE_STEP : MOVE_STEP;
        next_y = CPU_Y + 4;
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
        !api->app_exit) {
        if (api->toast) api->toast("Pong requires the RGB565 canvas API");
        request_exit();
        return;
    }

    ghostesp_ui_obj_t screen = api->ui_screen_create("Pong");
    if (!screen) { request_exit(); return; }
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_bg_color(screen, 0x000000);
    GH_VOID(api, ui_obj_set_flex_flow, screen, GHOSTESP_FLEX_FLOW_NONE);
    touch_bar = gh_touch_bar(api, true, touch_back, NULL);

    int width = api->ui_screen_get_content_width();
    int height = api->ui_screen_get_content_height();
    api->ui_obj_set_size(screen, width, height);

    /* Fill the content area while preserving the portrait 2:3 court. */
    int usable_height = height - (touch_bar ? TOUCH_BAR_HEIGHT : 0);
    int play_height = usable_height;
    if (play_height < GAME_H) play_height = GAME_H;
    canvas_width = width;
    canvas_height = width * 3 / 2;
    if (canvas_height > play_height) {
        canvas_height = play_height;
        canvas_width = play_height * 2 / 3;
    }
    if (canvas_width < GAME_W || canvas_height < GAME_H) {
        canvas_width = GAME_W;
        canvas_height = GAME_H;
    }

    canvas = api->ui_canvas_create(screen, canvas_width, canvas_height);
    if (!canvas) { request_exit(); return; }
    api->ui_canvas_fill(canvas, 0x000000);
    int canvas_y = (usable_height - canvas_height) / 2;
    canvas_left = (width - canvas_width) / 2;
    if (api->ui_obj_set_pos)
        api->ui_obj_set_pos(canvas, canvas_left, canvas_y);
    rng_state = api->system_uptime_us ? (uint32_t)api->system_uptime_us() : 0x504F4E47u;
    if (!rng_state) rng_state = 0x504F4E47u;
    reset_game();
    present();
}

static void pong_stop(void) {
    canvas = NULL;
    touch_bar = NULL;
    memset(framebuffer, 0, sizeof(framebuffer));
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
        touch_target = ((event->x - canvas_left) * GAME_W) / canvas_width;
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

static void pong_tick(uint32_t elapsed_ms) {
    if (exit_requested || !canvas) return;
    frame_accumulator += elapsed_ms;
    if (frame_accumulator > FRAME_MS * 4) frame_accumulator = FRAME_MS * 4;
    while (frame_accumulator >= FRAME_MS) {
        frame_accumulator -= FRAME_MS;
        update_player();
        update_cpu();
        update_ball();
    }
    present();
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "pong", "Pong", pong_start, pong_stop, pong_input, pong_tick
);

#define PONG_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, ui_canvas_blit_rgb565) + sizeof(((ghostesp_api_t *)0)->ui_canvas_blit_rgb565))

GHOSTESP_APP_INIT_WITH_API(app, api, "pong", PONG_REQUIRED_API_SIZE)
