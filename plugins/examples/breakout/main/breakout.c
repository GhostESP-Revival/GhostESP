#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define GAME_W 100
#define GAME_H 150
#define BRICK_ROWS 6
#define BRICK_COLS 8
#define BRICK_W 11
#define BRICK_H 5
#define BRICK_GAP 1
#define BRICK_X 3
#define BRICK_Y 25
#define PADDLE_W 24
#define PADDLE_H 4
#define PADDLE_Y 137
#define BALL_R 2
#define FRAME_MS 16
#define TOUCH_BAR_HEIGHT 34

#define C_BG 0x0000
#define C_WALL 0x3186
#define C_TEXT 0xFFFF
#define C_PADDLE 0x07FF
#define C_BALL 0xFFFF
#define C_PANEL 0x1082

typedef enum { STATE_READY, STATE_PLAYING, STATE_PAUSED, STATE_WON, STATE_LOST } game_state_t;

static const uint16_t brick_colors[BRICK_ROWS] = { 0xF81F, 0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x001F };
static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas, touch_bar;
static ghostesp_ui_timer_t frame_timer;
static uint16_t *framebuffer;
static size_t framebuffer_size;
static int canvas_width, canvas_height, canvas_left;
static int paddle_x, ball_x, ball_y, ball_dx, ball_dy;
static int old_paddle_x, old_ball_x, old_ball_y;
static int score, lives, level, old_score, old_lives, old_level;
static int bricks_left;
static bool bricks[BRICK_ROWS][BRICK_COLS];
static bool left_held, right_held, touch_active, snapshot_input, exit_requested;
static int touch_target;
static uint32_t frame_accumulator, last_frame_ms;
static ghostesp_touch_state_t touch_state;
static game_state_t state;

static void game_frame(void *user);

static int clamp_paddle(int x) {
    if (x < 2) return 2;
    if (x > GAME_W - PADDLE_W - 2) return GAME_W - PADDLE_W - 2;
    return x;
}

static void request_exit(void) {
    if (exit_requested) return;
    exit_requested = true;
    GH_VOID(api, request_exit);
}

static void touch_back(void *user) { (void)user; request_exit(); }

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > GAME_W) x2 = GAME_W;
    if (y2 > GAME_H) y2 = GAME_H;
    if (x >= x2 || y >= y2) return;
    int left = x * canvas_width / GAME_W, right = x2 * canvas_width / GAME_W;
    int top = y * canvas_height / GAME_H, bottom = y2 * canvas_height / GAME_H;
    if (right <= left) right = left + 1;
    if (bottom <= top) bottom = top + 1;
    for (int row = top; row < bottom; ++row) {
        uint16_t *pixel = framebuffer + (size_t)row * canvas_width + left;
        for (int col = left; col < right; ++col) *pixel++ = color;
    }
}

static void draw_char(char ch, int x, int y, uint16_t color) {
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
    };
    static const uint8_t letters[26][5] = {
        ['A'-'A']={2,5,7,5,5},['D'-'A']={6,5,5,5,6},['E'-'A']={7,4,6,4,7},
        ['I'-'A']={7,2,2,2,7},['L'-'A']={4,4,4,4,7},['N'-'A']={5,7,7,7,5},
        ['O'-'A']={7,5,5,5,7},['P'-'A']={6,5,6,4,4},['R'-'A']={6,5,6,5,5},
        ['S'-'A']={7,4,7,1,7},['T'-'A']={7,2,2,2,2},['U'-'A']={5,5,5,5,7},
        ['V'-'A']={5,5,5,5,2},['W'-'A']={5,5,7,7,5},['Y'-'A']={5,5,2,2,2}
    };
    const uint8_t *glyph = NULL;
    if (ch >= '0' && ch <= '9') glyph = digits[ch - '0'];
    else if (ch >= 'A' && ch <= 'Z') glyph = letters[ch - 'A'];
    if (!glyph) return;
    for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 3; ++col)
            if (glyph[row] & (1u << (2 - col))) fill_rect(x + col, y + row, 1, 1, color);
}

static void draw_text(const char *text, int x, int y, uint16_t color) {
    while (*text) { draw_char(*text++, x, y, color); x += 4; }
}

static void draw_number(int value, int x, int y) {
    char buf[12];
    int length = 0;
    do { buf[length++] = (char)('0' + value % 10); value /= 10; } while (value && length < 11);
    while (length--) { draw_char(buf[length], x, y, C_TEXT); x += 4; }
}

static void blit_region(int x, int y, int w, int h) {
    int left = x * canvas_width / GAME_W, top = y * canvas_height / GAME_H;
    int right = (x + w) * canvas_width / GAME_W, bottom = (y + h) * canvas_height / GAME_H;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > canvas_width) right = canvas_width;
    if (bottom > canvas_height) bottom = canvas_height;
    if (left >= right || top >= bottom) return;
    api->ui_canvas_blit_rgb565(canvas, framebuffer + (size_t)top * canvas_width + left,
                              right-left, bottom-top, canvas_width, left, top, right-left, bottom-top);
}

static void render(void) {
    memset(framebuffer, 0, framebuffer_size);
    fill_rect(0, 9, 2, GAME_H - 9, C_WALL);
    fill_rect(GAME_W - 2, 9, 2, GAME_H - 9, C_WALL);
    fill_rect(0, 9, GAME_W, 2, C_WALL);
    draw_text("S", 3, 2, C_TEXT); draw_number(score, 8, 2);
    draw_text("L", 47, 2, C_TEXT); draw_number(level, 52, 2);
    draw_text("V", 79, 2, C_TEXT); draw_number(lives, 84, 2);
    for (int row = 0; row < BRICK_ROWS; ++row)
        for (int col = 0; col < BRICK_COLS; ++col)
            if (bricks[row][col]) fill_rect(BRICK_X + col * (BRICK_W + BRICK_GAP),
                                            BRICK_Y + row * (BRICK_H + BRICK_GAP),
                                            BRICK_W, BRICK_H, brick_colors[row]);
    fill_rect(paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, C_PADDLE);
    fill_rect(paddle_x + 3, PADDLE_Y, PADDLE_W - 6, 1, C_TEXT);
    fill_rect(ball_x - BALL_R, ball_y - BALL_R, BALL_R * 2 + 1, BALL_R * 2 + 1, C_BALL);
    if (state != STATE_PLAYING) {
        const char *message = state == STATE_READY ? "TAP TO PLAY" : state == STATE_PAUSED ? "PAUSED" :
                              state == STATE_WON ? "YOU WIN" : "GAME OVER";
        int width = (int)strlen(message) * 4 + 6;
        fill_rect((GAME_W-width)/2, 82, width, 11, C_PANEL);
        draw_text(message, (GAME_W-(int)strlen(message)*4+1)/2, 85,
                  state == STATE_LOST ? brick_colors[1] : C_TEXT);
    }
}

static void present(void) {
    render();
    api->ui_canvas_blit_rgb565(canvas, framebuffer, canvas_width, canvas_height, canvas_width,
                              0, 0, canvas_width, canvas_height);
    old_paddle_x=paddle_x; old_ball_x=ball_x; old_ball_y=ball_y;
    old_score=score; old_lives=lives; old_level=level;
}

static void present_dirty(int hit_row, int hit_col) {
    render();
    int left = old_paddle_x < paddle_x ? old_paddle_x : paddle_x;
    int right = old_paddle_x > paddle_x ? old_paddle_x : paddle_x;
    blit_region(left, PADDLE_Y, right-left+PADDLE_W, PADDLE_H);
    left = old_ball_x < ball_x ? old_ball_x : ball_x;
    right = old_ball_x > ball_x ? old_ball_x : ball_x;
    int top = old_ball_y < ball_y ? old_ball_y : ball_y;
    int bottom = old_ball_y > ball_y ? old_ball_y : ball_y;
    blit_region(left-BALL_R, top-BALL_R, right-left+BALL_R*2+1, bottom-top+BALL_R*2+1);
    if (hit_row >= 0) blit_region(BRICK_X, BRICK_Y,
                                  BRICK_COLS*(BRICK_W+BRICK_GAP),
                                  BRICK_ROWS*(BRICK_H+BRICK_GAP));
    if (old_score!=score || old_lives!=lives || old_level!=level) blit_region(0,0,GAME_W,8);
    old_paddle_x=paddle_x; old_ball_x=ball_x; old_ball_y=ball_y;
    old_score=score; old_lives=lives; old_level=level;
}

static void reset_ball(void) {
    ball_x = paddle_x + PADDLE_W/2;
    ball_y = PADDLE_Y - BALL_R - 1;
    ball_dx = (level & 1) ? 1 : -1;
    ball_dy = -1;
    state = STATE_READY;
}

static void make_level(void) {
    memset(bricks, 1, sizeof(bricks));
    bricks_left = BRICK_ROWS * BRICK_COLS;
    paddle_x = (GAME_W-PADDLE_W)/2;
    reset_ball();
}

static void new_game(void) { score=0; lives=3; level=1; make_level(); }

static void activate(void) {
    if (state == STATE_READY) state = STATE_PLAYING;
    else if (state == STATE_PAUSED) state = STATE_PLAYING;
    else if (state == STATE_PLAYING) state = STATE_PAUSED;
    else if (state == STATE_WON) make_level();
    else new_game();
    present();
}

static void update_paddle(void) {
    bool left=left_held, right=right_held;
    if (snapshot_input) {
        ghostesp_input_snapshot_t snapshot;
        if (api->input_snapshot(&snapshot)) {
            left = (snapshot.held & GHOSTESP_BUTTON_LEFT) != 0;
            right = (snapshot.held & GHOSTESP_BUTTON_RIGHT) != 0;
        }
    }
    if (touch_active) {
        int center=paddle_x+PADDLE_W/2;
        if (touch_target<center-1) left=true;
        if (touch_target>center+1) right=true;
    }
    if (left != right) paddle_x=clamp_paddle(paddle_x+(right?2:-2));
    if (state == STATE_READY) ball_x=paddle_x+PADDLE_W/2;
}

static void update_ball(int *hit_row, int *hit_col) {
    int steps = 1 + (level - 1) / 2;
    if (steps > 3) steps = 3;
    for (int step=0; step<steps && state==STATE_PLAYING; ++step) {
        int nx=ball_x+ball_dx, ny=ball_y+ball_dy;
        if (nx-BALL_R<=2 || nx+BALL_R>=GAME_W-2) { ball_dx=-ball_dx; nx=ball_x+ball_dx; }
        if (ny-BALL_R<=11) { ball_dy=1; ny=ball_y+ball_dy; }
        if (ball_dy>0 && ny+BALL_R>=PADDLE_Y && ball_y+BALL_R<=PADDLE_Y &&
            nx>=paddle_x-BALL_R && nx<=paddle_x+PADDLE_W+BALL_R) {
            int offset=nx-(paddle_x+PADDLE_W/2);
            ball_dx=offset < -4 ? -1 : offset > 4 ? 1 : ball_dx;
            ball_dy=-1; ny=PADDLE_Y-BALL_R-1;
        }
        bool brick_hit=false;
        for (int row=0; row<BRICK_ROWS && !brick_hit; ++row) for (int col=0; col<BRICK_COLS; ++col) {
            if (!bricks[row][col]) continue;
            int bx=BRICK_X+col*(BRICK_W+BRICK_GAP), by=BRICK_Y+row*(BRICK_H+BRICK_GAP);
            if (nx+BALL_R<bx || nx-BALL_R>=bx+BRICK_W ||
                ny+BALL_R<by || ny-BALL_R>=by+BRICK_H) continue;
            bricks[row][col]=false; --bricks_left; score += (BRICK_ROWS-row)*10;
            *hit_row=row; *hit_col=col; brick_hit=true;
            bool entered_from_side = ball_x+BALL_R<bx || ball_x-BALL_R>=bx+BRICK_W;
            if (entered_from_side) { ball_dx=-ball_dx; nx=ball_x+ball_dx; }
            else { ball_dy=-ball_dy; ny=ball_y+ball_dy; }
            if (!bricks_left) { state=STATE_WON; ++level; score+=250; present(); return; }
            break;
        }
        ball_x=nx; ball_y=ny;
        if (ball_y-BALL_R>GAME_H) {
            if (--lives<=0) state=STATE_LOST; else reset_ball();
            present(); return;
        }
    }
}

static void breakout_start(void) {
    snapshot_input=api->input_snapshot!=NULL; exit_requested=false; left_held=right_held=touch_active=false;
    frame_accumulator=0; gh_touch_reset(&touch_state);
    if (!api->ui_canvas_create || !api->ui_canvas_blit_rgb565 || !api->ui_screen_get_content_width ||
        !api->ui_screen_get_content_height || !api->ui_timer_create || !api->ui_timer_delete || !api->request_exit) {
        if (api->toast) api->toast("Breakout requires the RGB565 canvas API");
        request_exit();
        return;
    }
    ghostesp_ui_obj_t screen=api->ui_screen_create("Breakout");
    if (!screen) { request_exit(); return; }
    api->ui_obj_set_scrollable(screen,false); api->ui_obj_set_bg_color(screen,C_BG);
    GH_VOID(api,ui_obj_set_pad,screen,0,0,0,0); GH_VOID(api,ui_obj_set_flex_flow,screen,GHOSTESP_FLEX_FLOW_NONE);
    touch_bar=gh_touch_bar(api,true,touch_back,NULL);
    int width=api->ui_screen_get_content_width(), height=api->ui_screen_get_content_height();
    api->ui_obj_set_size(screen,width,height);
    canvas_width=width; canvas_height=height-(touch_bar?TOUCH_BAR_HEIGHT:0);
    if (canvas_width<50 || canvas_height<75) {
        if (api->toast) api->toast("Display is too small for Breakout");
        request_exit(); return;
    }
    canvas=api->ui_canvas_create(screen,canvas_width,canvas_height);
    if (!canvas) { request_exit(); return; }
    framebuffer_size=(size_t)canvas_width*canvas_height*sizeof(*framebuffer); framebuffer=malloc(framebuffer_size);
    if (!framebuffer) { request_exit(); return; }
    canvas_left=0; if (api->ui_obj_set_pos) api->ui_obj_set_pos(canvas,canvas_left,0);
    new_game(); present(); last_frame_ms=api->system_uptime_ms?api->system_uptime_ms():0;
    frame_timer=api->ui_timer_create(game_frame,FRAME_MS,NULL); if (!frame_timer) request_exit();
}

static void breakout_stop(void) {
    if (frame_timer && api->ui_timer_delete) api->ui_timer_delete(frame_timer);
    frame_timer=NULL; canvas=NULL; touch_bar=NULL; free(framebuffer); framebuffer=NULL; framebuffer_size=0;
}

static void breakout_input(const ghostesp_input_event_t *event) {
    if (!event || exit_requested) return;
    if (event->type==GHOSTESP_INPUT_BACK && event->pressed) { request_exit(); return; }
    if (event->type==GHOSTESP_INPUT_TOUCH) {
        if (event->y < 0 || event->y >= canvas_height) {
            touch_active=false;
            gh_touch_reset(&touch_state);
            return;
        }
        bool tap=false; ghostesp_input_type_t swipe=gh_touch_update_tap(&touch_state,event,&tap);
        touch_active=event->pressed; touch_target=((event->x-canvas_left)*GAME_W)/canvas_width;
        if (touch_target<0) touch_target=0;
        if (touch_target>=GAME_W) touch_target=GAME_W-1;
        if (swipe==GHOSTESP_INPUT_LEFT) paddle_x=clamp_paddle(paddle_x-8);
        else if (swipe==GHOSTESP_INPUT_RIGHT) paddle_x=clamp_paddle(paddle_x+8);
        else if (swipe==GHOSTESP_INPUT_UP || tap) activate();
        return;
    }
    if (!event->pressed) {
        if (event->type==GHOSTESP_INPUT_LEFT) left_held=false;
        if (event->type==GHOSTESP_INPUT_RIGHT) right_held=false;
        return;
    }
    if (event->type==GHOSTESP_INPUT_LEFT) { left_held=true; paddle_x=clamp_paddle(paddle_x-2); }
    else if (event->type==GHOSTESP_INPUT_RIGHT) { right_held=true; paddle_x=clamp_paddle(paddle_x+2); }
    else if (event->type==GHOSTESP_INPUT_SELECT || event->type==GHOSTESP_INPUT_UP) activate();
    else if (event->type==GHOSTESP_INPUT_KEY) {
        int key=event->value;
        if (key==27 || key==8 || key==127 || key=='q' || key=='Q') request_exit();
        else if (key=='a'||key=='A'||key=='h'||key=='H'||key==',') paddle_x=clamp_paddle(paddle_x-5);
        else if (key=='d'||key=='D'||key=='l'||key=='L'||key=='.') paddle_x=clamp_paddle(paddle_x+5);
        else if (key==' '||key=='\r'||key=='p'||key=='P') activate();
        else if (key=='r'||key=='R') { new_game(); present(); }
    }
}

static void game_frame(void *user) {
    (void)user; if (exit_requested || !canvas) return;
    uint32_t now=api->system_uptime_ms?api->system_uptime_ms():0;
    uint32_t elapsed=last_frame_ms&&now?now-last_frame_ms:FRAME_MS; last_frame_ms=now;
    frame_accumulator+=elapsed; if (frame_accumulator>FRAME_MS*4) frame_accumulator=FRAME_MS*4;
    bool updated=false; int hit_row=-1,hit_col=-1;
    while (frame_accumulator>=FRAME_MS) {
        frame_accumulator-=FRAME_MS; update_paddle();
        if (state==STATE_PLAYING) update_ball(&hit_row,&hit_col);
        updated=true;
    }
    if (updated && (state==STATE_PLAYING || state==STATE_READY)) present_dirty(hit_row,hit_col);
}

static const ghostesp_app_t app=GHOSTESP_APP_DEFINE(
    "breakout","Breakout",breakout_start,breakout_stop,breakout_input,NULL
);
#define BREAKOUT_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t,request_exit)+sizeof(((ghostesp_api_t *)0)->request_exit))
GHOSTESP_APP_INIT_WITH_API(app,api,"breakout",BREAKOUT_REQUIRED_API_SIZE)
