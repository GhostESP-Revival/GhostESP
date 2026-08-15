#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BOARD_W 10
#define BOARD_H 20
#define HIDDEN_H 2
#define TOTAL_H (BOARD_H + HIDDEN_H)
#define NEXT_COUNT 5
#define TOUCH_BAR_HEIGHT 34
#define TIMER_MS 16
#define DAS_DELAY_MS 150
#define DAS_REPEAT_MS 45

#define C_BG 0x0000
#define C_PANEL 0x1082
#define C_GRID 0x2104
#define C_TEXT 0xFFFF
#define C_MUTED 0x7BEF

typedef struct { int x, y; } point_t;
typedef struct { int type, rotation, x, y; } piece_t;

static const uint16_t colors[8] = {C_BG, 0x07FF, 0xFFE0, 0xF81F, 0x07E0, 0xF800, 0x001F, 0xFD20};
static const point_t shapes[7][4][4] = {
    {{{0,1},{1,1},{2,1},{3,1}},{{2,0},{2,1},{2,2},{2,3}},{{0,2},{1,2},{2,2},{3,2}},{{1,0},{1,1},{1,2},{1,3}}},
    {{{1,0},{2,0},{1,1},{2,1}},{{1,0},{2,0},{1,1},{2,1}},{{1,0},{2,0},{1,1},{2,1}},{{1,0},{2,0},{1,1},{2,1}}},
    {{{1,0},{0,1},{1,1},{2,1}},{{1,0},{1,1},{2,1},{1,2}},{{0,1},{1,1},{2,1},{1,2}},{{1,0},{0,1},{1,1},{1,2}}},
    {{{1,0},{2,0},{0,1},{1,1}},{{1,0},{1,1},{2,1},{2,2}},{{1,1},{2,1},{0,2},{1,2}},{{0,0},{0,1},{1,1},{1,2}}},
    {{{0,0},{1,0},{1,1},{2,1}},{{2,0},{1,1},{2,1},{1,2}},{{0,1},{1,1},{1,2},{2,2}},{{1,0},{0,1},{1,1},{0,2}}},
    {{{0,0},{0,1},{1,1},{2,1}},{{1,0},{2,0},{1,1},{1,2}},{{0,1},{1,1},{2,1},{2,2}},{{1,0},{1,1},{0,2},{1,2}}},
    {{{2,0},{0,1},{1,1},{2,1}},{{1,0},{1,1},{1,2},{2,2}},{{0,1},{1,1},{2,1},{0,2}},{{0,0},{1,0},{1,1},{1,2}}},
};

static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas, touch_bar;
static ghostesp_ui_timer_t timer;
static uint16_t *framebuffer;
static int screen_w, screen_h, board_x, board_y, cell, panel_x, panel_y, panel_w, panel_h;
static uint8_t board[TOTAL_H][BOARD_W];
static int bag[7], bag_pos, next_queue[NEXT_COUNT], hold_piece;
static piece_t active;
static uint32_t rng_state, last_tick, fall_accumulator;
static int score, lines, level;
static bool hold_used, paused, game_over, exit_requested;
static bool snapshot_input;
static uint32_t das_dir, das_next_ms;
static bool das_repeating;
static bool needs_render;
static ghostesp_touch_state_t touch_state;
static bool touch_moved;

static uint32_t random_u32(void) {
    uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng_state = x;
}

static void request_exit(void) { if (!exit_requested) { exit_requested = true; GH_VOID(api, request_exit); } }
static void touch_back(void *user) { (void)user; request_exit(); }

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; ++row) {
        uint16_t *p = framebuffer + (size_t)row * screen_w + x;
        for (int i = 0; i < w; ++i) *p++ = color;
    }
}

static const uint8_t *glyph(char c) {
    static const uint8_t font[36][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
        {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
        {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
        {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{7,5,5,5,7},
        {6,5,6,4,4},{7,5,5,7,1},{6,5,6,5,5},{7,4,7,1,7},{7,2,2,2,2},
        {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7}
    };
    if (c >= '0' && c <= '9') return font[c - '0'];
    if (c >= 'A' && c <= 'Z') return font[10 + c - 'A'];
    return NULL;
}

static void text(const char *s, int x, int y, int scale, uint16_t color) {
    for (; *s; ++s, x += 4 * scale) {
        const uint8_t *g = glyph(*s); if (!g) continue;
        for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
            if (g[r] & (1u << (2-c))) fill_rect(x+c*scale,y+r*scale,scale,scale,color);
    }
}

static void number(int value, int x, int y, int scale, uint16_t color) {
    char buf[12]; int n = 0; if (!value) buf[n++]='0';
    else { char rev[12]; int rn=0; while(value && rn<11){rev[rn++]=(char)('0'+value%10);value/=10;} while(rn)buf[n++]=rev[--rn]; }
    buf[n]=0; text(buf,x,y,scale,color);
}

static bool collides(piece_t p) {
    for (int i=0;i<4;++i) { int x=p.x+shapes[p.type][p.rotation][i].x, y=p.y+shapes[p.type][p.rotation][i].y;
        if (x<0||x>=BOARD_W||y>=TOTAL_H||(y>=0&&board[y][x])) return true; }
    return false;
}

static void refill_bag(void) {
    for(int i=0;i<7;++i)bag[i]=i;
    for(int i=6;i>0;--i){int j=(int)(random_u32()%(uint32_t)(i+1));int t=bag[i];bag[i]=bag[j];bag[j]=t;} bag_pos=0;
}
static int take_bag(void) { if(bag_pos>=7)refill_bag(); return bag[bag_pos++]; }

static void spawn(int type) {
    active=(piece_t){type,0,3,0}; hold_used=false;
    if(collides(active)) game_over=true;
}

static void advance_queue(void) {
    int type=next_queue[0]; for(int i=0;i<NEXT_COUNT-1;++i)next_queue[i]=next_queue[i+1]; next_queue[NEXT_COUNT-1]=take_bag(); spawn(type);
}

static bool move_piece(int dx,int dy) { piece_t p=active;p.x+=dx;p.y+=dy;if(collides(p))return false;active=p;return true; }

static void rotate_piece(int direction) {
    piece_t p=active; p.rotation=(p.rotation+direction+4)%4;
    static const int kicks[]={0,-1,1,-2,2};
    for(int i=0;i<5;++i){p.x=active.x+kicks[i];if(!collides(p)){active=p;return;}}
}

static void clear_lines(void) {
    int cleared=0;
    for(int y=TOTAL_H-1;y>=0;--y){bool full=true;for(int x=0;x<BOARD_W;++x)if(!board[y][x]){full=false;break;}
        if(full){++cleared;for(int row=y;row>0;--row)memcpy(board[row],board[row-1],BOARD_W);memset(board[0],0,BOARD_W);++y;}}
    static const int points[]={0,100,300,500,800}; score+=points[cleared]*(level+1); lines+=cleared; level=lines/10;
}

static void lock_piece(void) {
    for(int i=0;i<4;++i){int x=active.x+shapes[active.type][active.rotation][i].x,y=active.y+shapes[active.type][active.rotation][i].y;if(y>=0&&y<TOTAL_H)board[y][x]=(uint8_t)(active.type+1);}
    clear_lines(); advance_queue();
}

static void hard_drop(void) { if(paused||game_over)return;int dropped=0;while(move_piece(0,1))++dropped;score+=dropped*2;lock_piece(); }
static void hold(void) { if(paused||game_over||hold_used)return;int old=active.type;if(hold_piece<0){hold_piece=old;advance_queue();}else{int t=hold_piece;hold_piece=old;spawn(t);}hold_used=true; }

static int fall_delay(void) { int d=800-level*65;return d<80?80:d; }

static void new_game(void) {
    memset(board,0,sizeof(board));bag_pos=7;hold_piece=-1;score=lines=level=0;paused=game_over=hold_used=false;
    for(int i=0;i<NEXT_COUNT;++i) next_queue[i]=take_bag();
    advance_queue();fall_accumulator=0;
}

static void block(int px,int py,int size,uint16_t color,bool ghost) {
    if(ghost){fill_rect(px+1,py+1,size-2,1,color);fill_rect(px+1,py+size-2,size-2,1,color);fill_rect(px+1,py+1,1,size-2,color);fill_rect(px+size-2,py+1,1,size-2,color);}
    else {fill_rect(px+1,py+1,size-2,size-2,color);if(size>4)fill_rect(px+2,py+2,size-4,1,C_TEXT);}
}

static void mini_piece(int type,int x,int y,int size) {
    for(int i=0;i<4;++i)block(x+shapes[type][0][i].x*size,y+shapes[type][0][i].y*size,size,colors[type+1],false);
}

static void render(void) {
    memset(framebuffer,0,(size_t)screen_w*screen_h*sizeof(*framebuffer));
    fill_rect(board_x-2,board_y-2,BOARD_W*cell+4,BOARD_H*cell+4,C_GRID);fill_rect(board_x,board_y,BOARD_W*cell,BOARD_H*cell,C_BG);
    for(int y=HIDDEN_H;y<TOTAL_H;++y)for(int x=0;x<BOARD_W;++x)if(board[y][x])block(board_x+x*cell,board_y+(y-HIDDEN_H)*cell,cell,colors[board[y][x]],false);
    piece_t ghost=active;while(!collides((piece_t){ghost.type,ghost.rotation,ghost.x,ghost.y+1}))++ghost.y;
    if(!game_over)for(int i=0;i<4;++i){int x=ghost.x+shapes[ghost.type][ghost.rotation][i].x,y=ghost.y+shapes[ghost.type][ghost.rotation][i].y-HIDDEN_H;if(y>=0)block(board_x+x*cell,board_y+y*cell,cell,colors[ghost.type+1],true);}
    if(!game_over)for(int i=0;i<4;++i){int x=active.x+shapes[active.type][active.rotation][i].x,y=active.y+shapes[active.type][active.rotation][i].y-HIDDEN_H;if(y>=0)block(board_x+x*cell,board_y+y*cell,cell,colors[active.type+1],false);}
    fill_rect(panel_x,panel_y,panel_w,panel_h,C_PANEL);int scale=panel_w>=80?2:1;int x=panel_x+5,y=panel_y+5;
    text("SCORE",x,y,scale,C_MUTED);number(score,x,y+7*scale,scale,C_TEXT);y+=16*scale;text("LEVEL",x,y,scale,C_MUTED);number(level+1,x,y+7*scale,scale,C_TEXT);y+=16*scale;text("LINES",x,y,scale,C_MUTED);number(lines,x,y+7*scale,scale,C_TEXT);y+=17*scale;
    int mini=cell>7?cell/2:4;text("HOLD",x,y,scale,C_MUTED);if(hold_piece>=0)mini_piece(hold_piece,x,y+7*scale,mini);y+=7*scale+mini*3;
    text("NEXT",x,y,scale,C_MUTED);y+=7*scale;for(int i=0;i<3&&y+mini*3<panel_y+panel_h;++i){mini_piece(next_queue[i],x,y,mini);y+=mini*3;}
    if(paused||game_over){const char *msg=game_over?"GAME OVER":"PAUSED";int w=(int)strlen(msg)*12+12;int ox=(screen_w-w)/2,oy=screen_h/2-12;fill_rect(ox,oy,w,24,C_PANEL);text(msg,ox+6,oy+7,2,game_over?colors[5]:C_TEXT);}
    api->ui_canvas_blit_rgb565(canvas,framebuffer,screen_w,screen_h,screen_w,0,0,screen_w,screen_h);
}

static void poll_held_moves(uint32_t now) {
    if (paused || game_over || !snapshot_input) return;
    ghostesp_input_snapshot_t snap;
    if (!api->input_snapshot || !api->input_snapshot(&snap)) return;
    uint32_t dir = 0;
    if (snap.held & GHOSTESP_BUTTON_LEFT) dir = 1;
    else if (snap.held & GHOSTESP_BUTTON_RIGHT) dir = 2;
    else if (snap.held & GHOSTESP_BUTTON_DOWN) dir = 3;
    if (dir != das_dir) {
        das_dir = dir;
        das_repeating = false;
        das_next_ms = now + DAS_DELAY_MS;
        if (!dir) return;
    } else if (!dir || now < das_next_ms) {
        return;
    }
    bool moved = false;
    if (dir == 1) moved = move_piece(-1, 0);
    else if (dir == 2) moved = move_piece(1, 0);
    else if (dir == 3) { moved = move_piece(0, 1); if (moved) ++score; }
    if (moved) needs_render = true;
    das_next_ms = now + (das_repeating ? DAS_REPEAT_MS : DAS_DELAY_MS);
    das_repeating = true;
}

static void game_tick(void *user) {
    (void)user;if(exit_requested||!canvas)return;uint32_t now=api->system_uptime_ms?api->system_uptime_ms():last_tick+TIMER_MS;uint32_t elapsed=last_tick?now-last_tick:TIMER_MS;last_tick=now;
    if(!paused&&!game_over){fall_accumulator+=elapsed;if(fall_accumulator>=(uint32_t)fall_delay()){fall_accumulator=0;if(move_piece(0,1))needs_render=true;else{lock_piece();needs_render=true;}}}
    poll_held_moves(now);
    if(needs_render){needs_render=false;render();}
}

static void layout(void) {
    bool landscape=screen_w>screen_h;int gap=6;
    if(landscape){cell=(screen_h-4)/BOARD_H;int bw=BOARD_W*cell;board_x=4;board_y=(screen_h-BOARD_H*cell)/2;panel_x=board_x+bw+gap;panel_y=2;panel_w=screen_w-panel_x-2;panel_h=screen_h-4;}
    else {int side=screen_w/3;cell=(screen_w-side-gap-4)/BOARD_W;int by_height=(screen_h-4)/BOARD_H;if(cell>by_height)cell=by_height;int bw=BOARD_W*cell;board_x=2;board_y=(screen_h-BOARD_H*cell)/2;panel_x=board_x+bw+gap;panel_y=2;panel_w=screen_w-panel_x-2;panel_h=screen_h-4;}
    if(cell<3)cell=3;
}

static void tetris_start(void) {
    exit_requested=false;touch_moved=false;gh_touch_reset(&touch_state);snapshot_input=api->input_snapshot!=NULL;das_dir=0;das_repeating=false;needs_render=false;
    if(!api->ui_canvas_create||!api->ui_canvas_blit_rgb565||!api->ui_screen_get_content_width||!api->ui_screen_get_content_height||!api->ui_timer_create||!api->request_exit){request_exit();return;}
    ghostesp_ui_obj_t screen=api->ui_screen_create("Tetris");if(!screen){request_exit();return;}api->ui_obj_set_scrollable(screen,false);api->ui_obj_set_bg_color(screen,C_BG);GH_VOID(api,ui_obj_set_pad,screen,0,0,0,0);GH_VOID(api,ui_obj_set_flex_flow,screen,GHOSTESP_FLEX_FLOW_NONE);
    touch_bar=gh_touch_bar(api,true,touch_back,NULL);screen_w=api->ui_screen_get_content_width();screen_h=api->ui_screen_get_content_height()-(touch_bar?TOUCH_BAR_HEIGHT:0);layout();canvas=api->ui_canvas_create(screen,screen_w,screen_h);if(!canvas){request_exit();return;}if(api->ui_obj_set_pos)api->ui_obj_set_pos(canvas,0,0);
    framebuffer=malloc((size_t)screen_w*screen_h*sizeof(*framebuffer));if(!framebuffer){request_exit();return;}rng_state=api->system_uptime_us?(uint32_t)api->system_uptime_us():0x54455452u;if(!rng_state)rng_state=1;new_game();last_tick=api->system_uptime_ms?api->system_uptime_ms():0;render();timer=api->ui_timer_create(game_tick,TIMER_MS,NULL);if(!timer)request_exit();
}

static void tetris_stop(void) {if(timer&&api->ui_timer_delete)api->ui_timer_delete(timer);timer=NULL;canvas=NULL;touch_bar=NULL;free(framebuffer);framebuffer=NULL;}

static void tetris_input(const ghostesp_input_event_t *event) {
    if(!event||exit_requested)return;
    if(event->type==GHOSTESP_INPUT_BACK&&event->pressed){request_exit();return;}
    if(event->type==GHOSTESP_INPUT_TOUCH){
        if(event->y<0||event->y>=screen_h){touch_moved=false;gh_touch_reset(&touch_state);return;}
        if(event->pressed){
            if(!touch_state.started){touch_state.started=true;touch_state.start_x=event->x;touch_state.start_y=event->y;touch_moved=false;return;}
            int dx=event->x-touch_state.start_x,dy=event->y-touch_state.start_y;
            int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
            if(ax>=16||ay>=16){
                if(ax>=ay){if(dx>0)move_piece(1,0);else move_piece(-1,0);}
                else if(dy>0)hard_drop();else rotate_piece(1);
                touch_state.start_x=event->x;touch_state.start_y=event->y;touch_moved=true;needs_render=true;
            }
            return;
        }
        if(touch_state.started&&!touch_moved)rotate_piece(1);
        touch_moved=false;gh_touch_reset(&touch_state);needs_render=true;return;
    }
    if(!event->pressed)return;
    if(game_over&&(event->type==GHOSTESP_INPUT_SELECT||event->type==GHOSTESP_INPUT_UP)){new_game();needs_render=true;return;}
    if(event->type==GHOSTESP_INPUT_LEFT){if(!snapshot_input)move_piece(-1,0);}
    else if(event->type==GHOSTESP_INPUT_RIGHT){if(!snapshot_input)move_piece(1,0);}
    else if(event->type==GHOSTESP_INPUT_DOWN){if(!snapshot_input&&move_piece(0,1))++score;}
    else if(event->type==GHOSTESP_INPUT_UP)hard_drop();
    else if(event->type==GHOSTESP_INPUT_SELECT)rotate_piece(1);
    else if(event->type==GHOSTESP_INPUT_KEY){int k=event->value;if(k==27||k=='q'||k=='Q')request_exit();else if(k=='z'||k=='Z')rotate_piece(-1);else if(k=='x'||k=='X')rotate_piece(1);else if(k=='c'||k=='C')hold();else if(k=='p'||k=='P')paused=!paused;else if(k==' '||k=='\r')hard_drop();else if(k=='r'||k=='R')new_game();}
    needs_render=true;
}

static const ghostesp_app_t app=GHOSTESP_APP_DEFINE("tetris","Tetris",tetris_start,tetris_stop,tetris_input,NULL);
#define TETRIS_REQUIRED_API_SIZE (offsetof(ghostesp_api_t,request_exit)+sizeof(((ghostesp_api_t*)0)->request_exit))
GHOSTESP_APP_INIT_WITH_API(app,api,"tetris",TETRIS_REQUIRED_API_SIZE)
