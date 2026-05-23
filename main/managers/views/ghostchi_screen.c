#include "managers/views/ghostchi_screen.h"

#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/ghostchi_activity.h"
#include "managers/ghostchi_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/error_popup.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void capitalize_ascii_first(char *text) {
    if (!text || !text[0]) return;
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] = (char)(text[0] - ('a' - 'A'));
    }
}

static const char s_onsets[] = "bcdfghjklmnprstvwyz";
static const char s_vowels[] = "aeiou";
static const char s_soft_codas[] = "lmnrst";
#define ONSET_COUNT 20
#define VOWEL_COUNT 5
#define CODA_COUNT 6

typedef struct {
    unsigned int level;
    unsigned int total_xp;
    unsigned int xp_into_level;
    unsigned int xp_for_next;
    bool max_level;
} ghostchi_progress_t;

static const unsigned int s_ghostchi_level_xp[] = {
    0, 18, 48, 92, 152, 230, 328, 448, 592, 762, 960
};

static void generate_ghostchi_name(char *buf, size_t buf_len) {
    uint8_t mac[6] = {0};
    uint8_t h0, h1, h2, h3, h4, h5;
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    h0 = mac[0] ^ mac[1] ^ mac[2];
    h1 = mac[3] ^ mac[4] ^ mac[5];
    h2 = (mac[0] ^ mac[3]) + (mac[1] ^ mac[4]) + (mac[2] ^ mac[5]);
    h3 = (h0 >> 3) ^ (h1 << 2) ^ h2;
    h4 = mac[1] ^ mac[4] ^ (uint8_t)(h2 + h3);
    h5 = mac[2] ^ mac[5] ^ (uint8_t)(h0 + h1);

    char c1 = s_onsets[h0 % ONSET_COUNT];
    char v1 = s_vowels[(h0 >> 4) % VOWEL_COUNT];
    char c2 = s_onsets[h1 % ONSET_COUNT];
    char v2 = s_vowels[(h1 >> 4) % VOWEL_COUNT];
    char c3 = s_onsets[h2 % ONSET_COUNT];
    char v3 = s_vowels[(h2 >> 4) % VOWEL_COUNT];

    int pattern = h4 % 3;
    if (pattern == 0) {
        char coda = s_soft_codas[h5 % CODA_COUNT];
        snprintf(buf, buf_len, "%c%c%c%c%c", c1, v1, c2, v2, coda);
    } else if (pattern == 1) {
        snprintf(buf, buf_len, "%c%c%c%c%c%c", c1, v1, c2, v2, c3, v3);
    } else {
        char coda = s_soft_codas[h5 % CODA_COUNT];
        snprintf(buf, buf_len, "%c%c%c%c%c%c", c1, v1, c2, v2, c3, coda);
    }
    capitalize_ascii_first(buf);
}

static unsigned int ghostchi_effective_captures(const ghostchi_snapshot_t *snap) {
    unsigned int handshakes;
    unsigned int attempts;

    if (!snap) return 0;
    handshakes = (unsigned int)snap->handshakes;
    attempts = (unsigned int)snap->attempts;
    if (attempts == 0) return handshakes;
    return handshakes < attempts ? handshakes : attempts;
}

static void ghostchi_get_progress(const ghostchi_snapshot_t *snap, ghostchi_progress_t *out) {
    unsigned int sessions;
    unsigned int attempts;
    unsigned int captures;
    unsigned int xp;
    size_t i;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->level = 1;

    if (!snap) {
        out->xp_for_next = s_ghostchi_level_xp[1];
        return;
    }

    sessions = (unsigned int)snap->total_sessions;
    attempts = (unsigned int)snap->attempts;
    captures = ghostchi_effective_captures(snap);
    xp = (sessions * 10u) + (attempts * 3u) + (captures * 24u);
    out->total_xp = xp;

    for (i = 1; i < (sizeof(s_ghostchi_level_xp) / sizeof(s_ghostchi_level_xp[0])); ++i) {
        if (xp < s_ghostchi_level_xp[i]) {
            out->level = (unsigned int)i;
            out->xp_into_level = xp - s_ghostchi_level_xp[i - 1];
            out->xp_for_next = s_ghostchi_level_xp[i] - s_ghostchi_level_xp[i - 1];
            return;
        }
    }

    out->level = (unsigned int)(sizeof(s_ghostchi_level_xp) / sizeof(s_ghostchi_level_xp[0])) - 1u;
    out->max_level = true;
    out->xp_into_level = s_ghostchi_level_xp[out->level] - s_ghostchi_level_xp[out->level - 1u];
    out->xp_for_next = out->xp_into_level;
}

extern const lv_img_dsc_t happy_50x50;
extern const lv_img_dsc_t angry_50x50;
extern const lv_img_dsc_t evil_50x50;
extern const lv_img_dsc_t love_50x50;
extern const lv_img_dsc_t tired_50x50;
extern const lv_img_dsc_t what2_50x50;
extern const lv_img_dsc_t speech;

#define GHOST_W 50
#define GHOST_H 50

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static lv_obj_t *s_art;
static lv_obj_t *s_ghost;
static lv_obj_t *s_bubble_box;
static lv_obj_t *s_bubble_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_reason_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_touch_btn_left;
static lv_obj_t *s_touch_btn_mid;
static lv_obj_t *s_touch_btn_right;
static lv_obj_t *s_touch_btn_left_label;
static lv_obj_t *s_touch_btn_mid_label;
static lv_obj_t *s_touch_btn_right_label;
static lv_obj_t *s_stats[6][2];
static lv_obj_t *s_xp_bar;
static lv_obj_t *s_xp_fill;
static lv_obj_t *s_xp_left_label;
static lv_obj_t *s_xp_right_label;
static lv_timer_t *s_timer;
static int s_page = 0;
static bool s_touch_started = false;
static int s_touch_start_x = 0;
static int s_touch_start_y = 0;

static lv_obj_t *s_encoder_btns[3];
static lv_obj_t *s_encoder_btn_labels[3];
static int s_encoder_btn_focus = 1;

static const int TAP_THRESHOLD = 12;

static uint32_t color_bg;
static uint32_t color_surface;
static uint32_t color_surface_alt;
static uint32_t color_text;
static uint32_t color_muted;

typedef struct {
    uint8_t x_pct;
    uint8_t y_pct;
    uint8_t size;
    uint8_t phase;
} ghostchi_star_t;

typedef struct {
    int ghost_x_offset;
    int ghost_y_offset;
    uint16_t bubble_zoom;
    int bubble_x_offset;
    int bubble_y_offset;
    int bubble_min_x;
    int bubble_max_x_margin;
    int bubble_min_y;
    int bubble_max_y_pct;
    int text_x_offset;
    int text_y_offset;
    int text_width_trim;
    int text_pad_x;
    int text_pad_y;
} ghostchi_layout_t;

static const ghostchi_layout_t k_layout_portrait = {
    .ghost_x_offset = 16,
    .ghost_y_offset = 0,
    .bubble_zoom = 512,
    .bubble_x_offset = 26,
    .bubble_y_offset = 2,
    .bubble_min_x = 6,
    .bubble_max_x_margin = 6,
    .bubble_min_y = 8,
    .bubble_max_y_pct = 42,
    .text_x_offset = -16,
    .text_y_offset = -11,
    .text_width_trim = 10,
    .text_pad_x = 14,
    .text_pad_y = 10,
};

static const ghostchi_layout_t k_layout_landscape = {
    .ghost_x_offset = 50,
    .ghost_y_offset = 14,
    .bubble_zoom = 512,
    .bubble_x_offset = 22,
    .bubble_min_x = 10,
    .bubble_max_x_margin = 6,
    .bubble_min_y = 10,
    .bubble_max_y_pct = 100,
    .text_x_offset = -20,
    .text_y_offset = -11,
    .text_width_trim = 2,
    .text_pad_x = 14,
    .text_pad_y = 10,
};

static const ghostchi_star_t k_portrait_stars[] = {
    {10, 10, 1,  0}, {18, 17, 1,  3}, {24, 25, 2,  7}, {12, 33, 1, 11},
    { 8, 42, 1, 15}, {27, 38, 1, 19}, {20, 48, 1,  6}, {14, 55, 2, 10},
    {76, 11, 1,  5}, {84, 18, 2,  9}, {90, 27, 1, 13}, {73, 35, 1, 17},
    {81, 43, 1,  2}, {88, 51, 1, 12}, {79, 57, 2, 16}, {50,  8, 1, 20}
};

static void update_ui(lv_timer_t *timer);
static void switch_page(int delta);
static void handle_select(void);
static bool is_portrait_layout(void);

static bool is_portrait_layout(void) {
    return LV_VER_RES > LV_HOR_RES;
}

static const ghostchi_layout_t *active_layout(void) {
    return is_portrait_layout() ? &k_layout_portrait : &k_layout_landscape;
}

static int split_pct(void) {
    return is_portrait_layout() ? 48 : 50;
}

static bool has_touch_controls(void) {
#ifdef CONFIG_USE_TOUCHSCREEN
    return true;
#else
    return false;
#endif
}

static bool has_encoder_buttons(void) {
    return !has_touch_controls() && !is_portrait_layout();
}

static void encoder_btn_update_focus(void) {
    for (int i = 0; i < 3; ++i) {
        bool focused = (i == s_encoder_btn_focus);
        lv_obj_set_style_border_color(s_encoder_btns[i],
            focused ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(s_encoder_btns[i], focused ? 2 : 1, 0);
        lv_obj_set_style_bg_opa(s_encoder_btns[i],
            focused ? LV_OPA_50 : LV_OPA_20, 0);
    }
}

static uint32_t idle_hours(const ghostchi_snapshot_t *snap) {
    if (!snap) return 0;
    return snap->idle_for_sec / 3600u;
}

static int ghost_bob_offset(void) {
    uint32_t phase = (uint32_t)((esp_timer_get_time() / 70000ULL) % 24ULL);
    int step = (phase < 12U) ? (int)phase : (23 - (int)phase);
    return step / 2;
}

static bool ghostchi_is_thriving(const ghostchi_snapshot_t *snap) {
    if (!snap) return false;
    return snap->total_sessions >= 6 || snap->handshakes >= 3;
}

static bool should_show_bubble(const ghostchi_snapshot_t *snap) {
    uint32_t phase;
    if (!snap || !snap->sd_ready) return false;
    phase = (uint32_t)((esp_timer_get_time() / 1000000ULL) % 20ULL);
    if (snap->running) {
        return phase < 8;
    }
    if (ghostchi_is_thriving(snap)) {
        return phase < 5;
    }
    return phase < 4;
}

static const char *bubble_text(const ghostchi_snapshot_t *snap,
                               const ghostchi_activity_snapshot_t *act) {
    uint32_t hours;
    uint32_t phase;
    uint32_t act_slot;
    if (!snap) return "boo";

    hours    = idle_hours(snap);
    phase    = (uint32_t)((esp_timer_get_time() / 3000000ULL) % 3ULL);
    act_slot = (uint32_t)((esp_timer_get_time() / 9000000ULL) % 6ULL);
    if (!snap->sd_ready) return "";

    if (act_slot == 5)
        return "drawn by pr3!";

    /* Activity-driven interrupts — fire for one 9s slot per 54s cycle */
    if (act && act_slot == 0) {
        if (act->battery_valid && act->battery_pct < 15 && !act->battery_charging)
            return phase == 0 ? "battery's dying." : (phase == 1 ? "charge me." : "really low.");
        if (act->aerial_devices > 0)
            return act->aerial_devices > 2 ? "drones nearby." : "something's flying.";
        if (act->sd_stats_valid && act->sd_used_pct > 88)
            return "disk's filling up.";
    }
    if (act && act_slot == 1) {
        if (act->battery_valid && act->battery_pct < 30 && act->battery_pct >= 15)
            return act->battery_charging ? "charging." : "running low.";
        if (!snap->running && act->gps_has_fix)
            return "got a fix.";
        if (!snap->running && act->gps_seen && !act->gps_has_fix)
            return "no gps fix.";
    }

    if (snap->running) {
        /* Battery warning while active */
        if (act && act->battery_valid && act->battery_pct < 15 && phase == 2)
            return "battery's dying.";
        switch (snap->state) {
            case GHOSTCHI_STATE_SWEEP:
                if (act_slot == 4)
                    return phase == 0 ? "out for a walk." : (phase == 1 ? "just roaming." : "seeing what's up.");
                if (act_slot >= 3)
                    return phase == 0 ? "checking again." : (phase == 1 ? "still looking." : "sweeping through.");
                return phase == 0 ? "scanning." : (phase == 1 ? "same channels." : "still scanning.");
            case GHOSTCHI_STATE_LOCK:
                if (snap->confidence > 75) {
                    if (act_slot == 4)
                        return phase == 0 ? "got a vibe." : (phase == 1 ? "this looks good." : "stay with me.");
                    return act_slot >= 3
                               ? (phase == 0 ? "close enough." : (phase == 1 ? "right there." : "looks promising."))
                               : (phase == 0 ? "almost. probably." : (phase == 1 ? "any second now." : "got a feel for it."));
                }
                if (act_slot == 4)
                    return phase == 0 ? "hanging on." : (phase == 1 ? "just a sec." : "being patient.");
                return act_slot >= 3
                           ? (phase == 0 ? "just listening." : (phase == 1 ? "not yet." : "holding here."))
                           : (phase == 0 ? "waiting on a packet." : (phase == 1 ? "listening." : "patience."));
            case GHOSTCHI_STATE_STIM:
                if (act_slot == 4)
                    return phase == 0 ? "getting loud." : (phase == 1 ? "bit rude, yeah." : "wake up already.");
                return act_slot >= 3
                           ? (phase == 0 ? "forcing it." : (phase == 1 ? "nudging them." : "stirring things up."))
                           : (phase == 0 ? "fine. deauthing." : (phase == 1 ? "pushing them." : "making noise."));
            case GHOSTCHI_STATE_COOLDOWN:
                if (snap->handshakes) {
                    if (act_slot == 4)
                        return phase == 0 ? "nice." : (phase == 1 ? "I'll take it." : "good little haul.");
                    return act_slot >= 3
                               ? (phase == 0 ? "saved it." : (phase == 1 ? "good enough." : "got the packet."))
                               : (phase == 0 ? "logged it." : (phase == 1 ? "that's one." : "captured."));
                }
                if (act_slot == 4)
                    return phase == 0 ? "eh, whatever." : (phase == 1 ? "next one maybe." : "can't win all.");
                return act_slot >= 3
                           ? (phase == 0 ? "not this one." : (phase == 1 ? "came up empty." : "moving on."))
                           : (phase == 0 ? "wasn't the one." : (phase == 1 ? "missed it." : "nothing this time."));
            case GHOSTCHI_STATE_STOPPING:
                return phase == 0 ? "done. for now." : (phase == 1 ? "winding down." : "that's enough.");
            default:
                return phase == 0 ? "working." : (phase == 1 ? "on it." : "doing things.");
        }
    }

    if (ghostchi_is_thriving(snap)) {
        if (hours < 2) {
            if (act_slot == 4)
                return phase == 0 ? "pretty good." : (phase == 1 ? "I'm into it." : "that was fun.");
            return act_slot >= 3
                       ? (phase == 0 ? "went alright." : (phase == 1 ? "not too bad." : "could do more."))
                       : (phase == 0 ? "it worked." : (phase == 1 ? "took long enough." : "one for the log."));
        }
        if (hours < 8) {
            if (act_slot == 4)
                return phase == 0 ? "still buzzing." : (phase == 1 ? "I'm around." : "say the word.");
            return act_slot >= 3
                       ? (phase == 0 ? "still awake." : (phase == 1 ? "say when." : "I'm ready."))
                       : (phase == 0 ? "could go again." : (phase == 1 ? "whenever." : "nothing else on."));
        }
        if (act_slot == 4)
            return phase == 0 ? "just vibing." : (phase == 1 ? "wide awake." : "let's do stuff.");
        return act_slot >= 3
                   ? (phase == 0 ? "still here." : (phase == 1 ? "same as ever." : "give me something."))
                   : (phase == 0 ? "been a while." : (phase == 1 ? "the whole time." : "run me already."));
    }
    if (snap->total_sessions == 0) {
        switch (phase) {
            case 0: return "nothing yet.";
            case 1: return "waiting, as usual.";
            default: return "I'll be here.";
        }
    }
    if (hours >= 24) {
        switch (phase) {
            case 0: return "still running.";
            case 1: return "long shift.";
            default: return "you left me on.";
        }
    }
    if (hours >= 8) {
        if (act_slot == 4) {
            switch (phase) {
                case 0: return "kinda sleepy.";
                case 1: return "slow day.";
                default: return "I'm still here.";
            }
        }
        if (act_slot >= 3) {
            switch (phase) {
                case 0: return "dead quiet.";
                case 1: return "nothing moving.";
                default: return "same as before.";
            }
        }
        switch (phase) {
            case 0: return "quiet.";
            case 1: return "just watching.";
            default: return "not much out here.";
        }
    }
    if (hours >= 2) {
        if (act_slot == 4) {
            switch (phase) {
                case 0: return "pretty calm.";
                case 1: return "no rush.";
                default: return "we can chill.";
            }
        }
        if (act_slot >= 3) {
            switch (phase) {
                case 0: return "all clear.";
                case 1: return "your call.";
                default: return "nothing urgent.";
            }
        }
        switch (phase) {
            case 0: return "probably clear.";
            case 1: return "up to you.";
            default: return "could scan.";
        }
    }
    if (snap->handshakes > 0) {
        if (act_slot == 4) {
            switch (phase) {
                case 0: return "I've got one.";
                case 1: return "feeling lucky.";
                default: return "maybe again.";
            }
        }
        if (act_slot >= 3) {
            switch (phase) {
                case 0: return "had one before.";
                case 1: return "might again.";
                default: return "worth a try.";
            }
        }
        switch (phase) {
            case 0: return "got one earlier.";
            case 1: return "not bad, actually.";
            default: return "could happen again.";
        }
    }
    if (act_slot == 4) {
        switch (phase) {
            case 0: return "I'm bored.";
            case 1: return "do a thing.";
            default: return "I'm just here.";
        }
    }
    if (act_slot >= 3) {
        switch (phase) {
            case 0: return "something's out there.";
            case 1: return "could check.";
            default: return "your move.";
        }
    }
    switch (phase) {
        case 0: return "signal's there.";
        case 1: return "could scan.";
        default: return "pick something.";
    }
}

static const char *state_mood(ghostchi_state_t state) {
    switch (state) {
        case GHOSTCHI_STATE_BLOCKED: return "storage missing";
        case GHOSTCHI_STATE_IDLE: return "waiting for a run";
        case GHOSTCHI_STATE_SWEEP: return "observing channels";
        case GHOSTCHI_STATE_RANK: return "scoring targets";
        case GHOSTCHI_STATE_LOCK: return "listening for eapol";
        case GHOSTCHI_STATE_STIM: return "forcing movement";
        case GHOSTCHI_STATE_COOLDOWN: return "avoiding thrash";
        case GHOSTCHI_STATE_STOPPING: return "closing session";
        default: return "standing by";
    }
}

static const char *state_label_text(ghostchi_state_t state) {
    switch (state) {
        case GHOSTCHI_STATE_BLOCKED: return "BLOCKED";
        case GHOSTCHI_STATE_IDLE: return "READY";
        case GHOSTCHI_STATE_SWEEP: return "SWEEP";
        case GHOSTCHI_STATE_RANK: return "RANK";
        case GHOSTCHI_STATE_LOCK: return "LOCK";
        case GHOSTCHI_STATE_STIM: return "STIM";
        case GHOSTCHI_STATE_COOLDOWN: return "COOL";
        case GHOSTCHI_STATE_STOPPING: return "STOP";
        default: return "GHOST";
    }
}

static const lv_img_dsc_t *pick_ghost_sprite(const ghostchi_snapshot_t *snap) {
    if (!snap) return &happy_50x50;
    if (!snap->running) {
        uint32_t hours = idle_hours(snap);
        if (!snap->sd_ready) return &what2_50x50;
        if (hours >= 8) return &tired_50x50;
        if (ghostchi_is_thriving(snap)) return &love_50x50;
        return &happy_50x50;
    }
    switch (snap->state) {
        case GHOSTCHI_STATE_STIM:    return &evil_50x50;
        case GHOSTCHI_STATE_LOCK:    return &angry_50x50;
        case GHOSTCHI_STATE_COOLDOWN:return &tired_50x50;
        case GHOSTCHI_STATE_SWEEP:   return &what2_50x50;
        case GHOSTCHI_STATE_IDLE:    return ghostchi_is_thriving(snap) ? &love_50x50 : &happy_50x50;
        default:                     return &happy_50x50;
    }
}

static void load_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    color_bg = theme_palette_get_background(theme);
    color_surface = theme_palette_get_surface(theme);
    color_surface_alt = theme_palette_get_surface_alt(theme);
    color_text = theme_palette_get_text(theme);
    color_muted = theme_palette_get_text_muted(theme);
}

static void draw_panel_rect(lv_draw_ctx_t *draw_ctx, const lv_area_t *coords, lv_color_t bg, lv_color_t border, lv_opa_t opa, lv_coord_t radius) {
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = bg;
    rect.bg_opa = opa;
    rect.border_color = border;
    rect.border_opa = LV_OPA_0;
    rect.border_width = 0;
    rect.radius = radius;
    lv_draw_rect(draw_ctx, &rect, coords);
}

static void draw_portrait_starfield(lv_draw_ctx_t *draw_ctx, const lv_area_t *coords) {
    lv_draw_rect_dsc_t star;
    int w = lv_area_get_width(coords);
    int h = lv_area_get_height(coords);
    int stats_top = coords->y1 + (h * 46) / 100;
    uint32_t tick = (uint32_t)(esp_timer_get_time() / 90000ULL);

    lv_draw_rect_dsc_init(&star);
    star.bg_color = lv_color_hex(0xFFFFFF);
    star.border_width = 0;
    star.radius = 0;

    for (size_t i = 0; i < sizeof(k_portrait_stars) / sizeof(k_portrait_stars[0]); ++i) {
        const ghostchi_star_t *s = &k_portrait_stars[i];
        lv_area_t dot;
        uint32_t phase = (tick + s->phase) % 24U;
        uint32_t twinkle = phase < 12U ? phase : (24U - phase);
        lv_opa_t opa = (lv_opa_t)(18 + (twinkle * 10));
        int x = coords->x1 + (w * s->x_pct) / 100;
        int y = coords->y1 + (h * s->y_pct) / 100;

        if (s->x_pct > 35 && s->x_pct < 65 && y > coords->y1 + 18) continue;
        if (y + s->size >= stats_top - 4) continue;

        dot.x1 = x;
        dot.y1 = y;
        dot.x2 = x + s->size - 1;
        dot.y2 = y + s->size - 1;
        star.bg_opa = opa;
        lv_draw_rect(draw_ctx, &star, &dot);
    }
}

static void ghostchi_draw_event(lv_event_t *e) {
    ghostchi_snapshot_t snap;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    int split_x;
    lv_area_t stat_panel;
    lv_area_t footer_panel;
    if (!obj || !draw_ctx) return;

    ghostchi_manager_get_snapshot(&snap);
    lv_obj_get_coords(obj, &coords);
    split_x = coords.x1 + (lv_area_get_width(&coords) * split_pct()) / 100;

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(color_bg);
    bg.bg_opa = LV_OPA_COVER;
    lv_draw_rect(draw_ctx, &bg, &coords);

    if (is_portrait_layout()) {
        draw_portrait_starfield(draw_ctx, &coords);
    }

    if (is_portrait_layout()) {
        stat_panel.x1 = coords.x1 + 10;
        stat_panel.y1 = coords.y1 + (lv_area_get_height(&coords) * 46) / 100;
        stat_panel.x2 = coords.x2 - 10;
        stat_panel.y2 = coords.y2 - 34;
    } else {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_hex(color_surface_alt);
        line.opa = LV_OPA_20;
        line.width = 1;
        lv_point_t pts[2] = {{split_x, coords.y1 + 10}, {split_x, coords.y2 - 24}};
        lv_draw_line(draw_ctx, &line, &pts[0], &pts[1]);

        stat_panel.x1 = split_x + 8;
        stat_panel.y1 = coords.y1 + 8;
        stat_panel.x2 = coords.x2 - 8;
        stat_panel.y2 = coords.y2 - 34;
    }
    draw_panel_rect(draw_ctx, &stat_panel, lv_color_hex(color_surface), lv_color_hex(color_surface_alt), LV_OPA_10, 10);

    footer_panel.x1 = coords.x1 + 10;
    footer_panel.y1 = coords.y2 - 30;
    footer_panel.x2 = coords.x2 - 10;
    footer_panel.y2 = coords.y2 - 8;
    draw_panel_rect(draw_ctx, &footer_panel, lv_color_hex(color_surface_alt), lv_color_hex(color_surface_alt), LV_OPA_20, 8);
}

static void set_stat(int row, const char *key, const char *value) {
    if (row < 0 || row >= 6) return;
    lv_label_set_text(s_stats[row][0], key ? key : "");
    lv_label_set_text(s_stats[row][1], value ? value : "");
}

static void layout_stats(void) {
    int key_w, value_w, gap, key_x, value_x, row_y, row_start, row_step;
    int content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
    int split = (LV_HOR_RES * split_pct()) / 100;

    if (has_encoder_buttons() || s_page == 2) {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, has_touch_controls() ? -38 : -8);
    }

    if (is_portrait_layout()) {
        key_w   = 56;
        value_w = 84;
        gap     = 4;
        row_start = (LV_VER_RES * 48) / 100;
        row_step = 15;
        key_x = (LV_HOR_RES / 2) - gap / 2 - key_w;
        value_x = (LV_HOR_RES / 2) + (gap + 1) / 2;
    } else {
        int panel_x = split + 12;
        int panel_w = LV_HOR_RES - split - 20;
        bool compact = content_h <= 120;
        key_w = compact ? 42 : 56;
        gap = 4;
        value_w = panel_w - key_w - gap;
        if (value_w < 30) value_w = 30;
        row_start = compact ? 10 : 14;
        row_step = compact ? 11 : 16;
        key_x = panel_x;
        value_x = panel_x + key_w + gap;
    }

    for (int i = 0; i < 6; ++i) {
        row_y = row_start + (i * row_step);
        lv_obj_set_pos(s_stats[i][0], key_x, row_y);
        lv_obj_set_width(s_stats[i][0], key_w);
        lv_obj_set_pos(s_stats[i][1], value_x, row_y);
        lv_obj_set_width(s_stats[i][1], value_w);
    }
}

static void handle_footer_action(int zone) {
    ghostchi_snapshot_t snap;
    ghostchi_manager_get_snapshot(&snap);
    if (zone == 0) {
        if (snap.running) {
            ghostchi_manager_stop();
        } else {
            display_manager_switch_view(&apps_menu_view);
        }
    } else if (zone == 1) {
        handle_select();
    } else {
        switch_page(1);
    }
}

static void update_ui(lv_timer_t *timer) {
    ghostchi_snapshot_t snap;
    ghostchi_activity_snapshot_t act;
    char buf[64];
    int art_w;
    int art_h;
    int content_h;
    int split;
    int ghost_x;
    int ghost_base_y;
    int ghost_y;
    int state_y = 0;
    int bubble_w;
    int bubble_h;
    int bubble_x;
    int bubble_y;
    int bubble_text_y;
    uint16_t bubble_zoom;
    int bubble_pad_x;
    int bubble_pad_y;
    int bubble_text_h;
    int bubble_text_inner_h;
    bool show_bubble;
    bool compact_landscape;
    const ghostchi_layout_t *layout;
    LV_UNUSED(timer);

    if (!s_content || !lv_obj_is_valid(s_content)) return;
    ghostchi_manager_tick();
    ghostchi_manager_get_snapshot(&snap);
    ghostchi_activity_get_snapshot(&act);
    layout = active_layout();
    show_bubble = should_show_bubble(&snap);
    content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
    compact_landscape = !is_portrait_layout() && content_h <= 120;
    split = (LV_HOR_RES * split_pct()) / 100;
    art_w = split - 12;
    art_h = content_h - 34;
    if (art_w < 90) art_w = 90;
    if (art_h < 72) art_h = 72;
    lv_obj_set_size(s_art, LV_HOR_RES, content_h);
    lv_obj_invalidate(s_art);

    lv_img_set_src(s_ghost, pick_ghost_sprite(&snap));

    int small_screen_y_offset = (LV_VER_RES <= 135) ? 8 : 0;
    if (is_portrait_layout()) {
        ghost_x = (LV_HOR_RES / 2) - (GHOST_W / 2);
        ghost_base_y = (LV_VER_RES * 16) / 100 + layout->ghost_y_offset;
    } else {
        ghost_x = (split / 2) - (GHOST_H / 2);
        ghost_base_y = (compact_landscape ? 2 : layout->ghost_y_offset) + small_screen_y_offset;
    }
    if (show_bubble) {
        ghost_x += layout->ghost_x_offset;
    }
    lv_obj_set_style_img_recolor_opa(s_ghost, LV_OPA_0, 0);
    ghost_y = ghost_bob_offset() - 3;
    lv_obj_set_pos(s_ghost, ghost_x, ghost_base_y + ghost_y);

    bubble_zoom = layout->bubble_zoom;
    bubble_w = (48 * (int)bubble_zoom) / 256;
    bubble_h = (24 * (int)bubble_zoom) / 256;
    bubble_pad_x = layout->text_pad_x;
    bubble_pad_y = layout->text_pad_y;
    lv_img_set_zoom(s_bubble_box, bubble_zoom);

    if (show_bubble) {
        lv_obj_clear_flag(s_bubble_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bubble_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_bubble_label, bubble_text(&snap, &act));
        lv_obj_set_width(s_bubble_label, bubble_w - (bubble_pad_x * 2) - layout->text_width_trim);
        lv_obj_set_style_text_align(s_bubble_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_update_layout(s_bubble_label);
        bubble_text_h = lv_obj_get_height(s_bubble_label);
        bubble_text_inner_h = bubble_h - (bubble_pad_y * 2);
        if (bubble_text_inner_h < bubble_text_h) bubble_text_inner_h = bubble_text_h;
        if (is_portrait_layout()) {
            bubble_x = ghost_x - bubble_w + layout->bubble_x_offset;
            if (bubble_x < layout->bubble_min_x) bubble_x = layout->bubble_min_x;
            if (bubble_x + bubble_w > LV_HOR_RES - layout->bubble_max_x_margin) {
                bubble_x = LV_HOR_RES - bubble_w - layout->bubble_max_x_margin;
            }
            bubble_y = ghost_base_y + ghost_y + layout->bubble_y_offset;
            if (bubble_y + bubble_h > ((LV_VER_RES * layout->bubble_max_y_pct) / 100)) {
                bubble_y = ((LV_VER_RES * layout->bubble_max_y_pct) / 100) - bubble_h;
            }
            if (bubble_y < layout->bubble_min_y) bubble_y = layout->bubble_min_y;
            bubble_text_y = bubble_y + bubble_pad_y + layout->text_y_offset +
                            ((bubble_text_inner_h - bubble_text_h) / 2);
            lv_obj_set_pos(s_bubble_box, bubble_x, bubble_y);
            lv_obj_set_pos(s_bubble_label,
                           bubble_x + bubble_pad_x + layout->text_x_offset,
                           bubble_text_y);
        } else {
            bubble_x = ghost_x - bubble_w + layout->bubble_x_offset;
            if (bubble_x < layout->bubble_min_x) bubble_x = layout->bubble_min_x;
            if (bubble_x + bubble_w > split - layout->bubble_max_x_margin) {
                bubble_x = split - bubble_w - layout->bubble_max_x_margin;
            }
            bubble_y = ghost_base_y + ghost_y + layout->bubble_y_offset;
            if (bubble_y < layout->bubble_min_y) bubble_y = layout->bubble_min_y;
            bubble_text_y = bubble_y + bubble_pad_y + layout->text_y_offset +
                            ((bubble_text_inner_h - bubble_text_h) / 2);
            lv_obj_set_pos(s_bubble_box, bubble_x, bubble_y);
            lv_obj_set_pos(s_bubble_label,
                           bubble_x + bubble_pad_x + layout->text_x_offset,
                           bubble_text_y);
        }
    } else {
        lv_obj_add_flag(s_bubble_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bubble_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_state_label, state_label_text(snap.state));
    lv_obj_set_style_text_color(s_state_label,
                                lv_color_hex(snap.state == GHOSTCHI_STATE_BLOCKED ? 0xDD6655 : color_text),
                                0);
    if (is_portrait_layout()) {
        state_y = ghost_base_y + GHOST_H + 6;
        lv_obj_set_width(s_state_label, LV_HOR_RES - 32);
        lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, state_y);
    } else {
        lv_obj_set_width(s_state_label, split - 24);
        lv_obj_align(s_state_label, LV_ALIGN_TOP_LEFT, 16, compact_landscape ? (58 + small_screen_y_offset) : (ghost_base_y + GHOST_H + 8));
        lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_LEFT, 0);
    }

    snprintf(buf, sizeof(buf), "%s", state_mood(snap.state));
    lv_label_set_text(s_reason_label, buf);
    if (is_portrait_layout()) {
        lv_obj_set_width(s_reason_label, LV_HOR_RES - 36);
        lv_obj_set_style_text_align(s_reason_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_update_layout(s_state_label);
        lv_obj_align(s_reason_label, LV_ALIGN_TOP_MID, 0, state_y + lv_obj_get_height(s_state_label) + 2);
    } else {
        lv_obj_set_width(s_reason_label, split - 24);
        lv_obj_set_style_text_align(s_reason_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align_to(s_reason_label, s_state_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, compact_landscape ? 2 : 4);
    }

    lv_obj_clear_flag(s_state_label, LV_OBJ_FLAG_HIDDEN);
    if (LV_VER_RES <= 135) {
        lv_obj_add_flag(s_reason_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_reason_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (snap.running) {
        if (has_touch_controls()) {
            lv_label_set_text(s_hint_label,
                              "Tap ghost or press Select to stop");
        } else {
            lv_label_set_text(s_hint_label,
                              "Select: Stop  Left/Right: Page\n"
                              "Back: Stop");
        }
    } else if (snap.sd_ready) {
        if (has_touch_controls()) {
            lv_label_set_text(s_hint_label,
                              "Tap ghost or press Select to start");
        } else {
            lv_label_set_text(s_hint_label,
                              "Select: Start  Left/Right: Page\n"
                              "Back: Exit");
        }
    } else {
        if (has_touch_controls()) {
            lv_label_set_text(s_hint_label,
                              "Mount SD to arm Ghostchi");
        } else {
            lv_label_set_text(s_hint_label,
                              "Mount SD to arm Ghostchi\n"
                              "Left/Right: Page  Back: Exit");
        }
    }
    lv_obj_set_width(s_hint_label, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_hint_label, is_portrait_layout() ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    if (has_encoder_buttons() || s_page == 2) {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, has_touch_controls() ? -38 : -8);
    }

    if (has_touch_controls()) {
        int content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
        int btn_y = LV_VER_RES - 26;
        int btn_w = (LV_HOR_RES - 32) / 3;
        int btn_h = 18;
        int btn_gap = 4;
        int btn_x = 12;
        btn_y = content_h - btn_h - 10;
        lv_obj_clear_flag(s_touch_btn_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_touch_btn_mid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_touch_btn_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_touch_btn_left, btn_x, btn_y);
        lv_obj_set_pos(s_touch_btn_mid, btn_x + btn_w + btn_gap, btn_y);
        lv_obj_set_pos(s_touch_btn_right, btn_x + ((btn_w + btn_gap) * 2), btn_y);
        lv_obj_set_size(s_touch_btn_left, btn_w, btn_h);
        lv_obj_set_size(s_touch_btn_mid, btn_w, btn_h);
        lv_obj_set_size(s_touch_btn_right, btn_w, btn_h);
        lv_label_set_text(s_touch_btn_left_label, snap.running ? "STOP" : "EXIT");
        lv_label_set_text(s_touch_btn_mid_label, snap.running ? "STOP" : "START");
        lv_label_set_text(s_touch_btn_right_label, "PAGE");
        lv_obj_center(s_touch_btn_left_label);
        lv_obj_center(s_touch_btn_mid_label);
        lv_obj_center(s_touch_btn_right_label);
    }

    if (has_encoder_buttons()) {
        int enc_btn_gap = 4;
        int enc_btn_w = (LV_HOR_RES - 16 - 2 * enc_btn_gap) / 3;
        int enc_btn_h = 14;
        int enc_btn_x = 8;
        int enc_btn_y = content_h - 22;
        for (int i = 0; i < 3; ++i) {
            lv_obj_clear_flag(s_encoder_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_encoder_btns[i], enc_btn_x + i * (enc_btn_w + enc_btn_gap), enc_btn_y);
            lv_obj_set_size(s_encoder_btns[i], enc_btn_w, enc_btn_h);
        }
        lv_label_set_text(s_encoder_btn_labels[0], snap.running ? "STOP" : "EXIT");
        lv_label_set_text(s_encoder_btn_labels[1], snap.running ? "STOP" : "START");
        lv_label_set_text(s_encoder_btn_labels[2], "PAGE");
        lv_obj_center(s_encoder_btn_labels[0]);
        lv_obj_center(s_encoder_btn_labels[1]);
        lv_obj_center(s_encoder_btn_labels[2]);
        encoder_btn_update_focus();
    } else {
        for (int i = 0; i < 3; ++i) {
            lv_obj_add_flag(s_encoder_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < 6; ++i) {
        lv_obj_clear_flag(s_stats[i][0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_stats[i][1], LV_OBJ_FLAG_HIDDEN);
    }

    if (s_page == 0) {
        set_stat(0, "MODE", snap.running ? "Active" : (snap.sd_ready ? "Standby" : "Blocked"));
        snprintf(buf, sizeof(buf), "%u", (unsigned)snap.current_channel);
        set_stat(1, "CH", snap.current_channel ? buf : "--");
        snprintf(buf, sizeof(buf), "%u", (unsigned)snap.aps_visible);
        set_stat(2, "APS", buf);
        set_stat(3, "TARGET", snap.target_ssid[0] ? snap.target_ssid : "None");
        snprintf(buf, sizeof(buf), "%u", (unsigned)snap.handshakes);
        set_stat(4, "PWND", buf);
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)snap.confidence);
        set_stat(5, "CONF", snap.confidence ? buf : "--");
    } else if (s_page == 1) {
        snprintf(buf, sizeof(buf), "%luK", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
        set_stat(0, "HEAP", buf);
        snprintf(buf, sizeof(buf), "%luK", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        set_stat(1, "IRAM", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)snap.attempts);
        set_stat(2, "TRIES", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)snap.failures);
        set_stat(3, "MISSES", buf);
        if (snap.running) {
            set_stat(4, "IDLE", "hunting");
        } else if (snap.idle_for_sec >= 3600u) {
            snprintf(buf, sizeof(buf), "%luh", (unsigned long)(snap.idle_for_sec / 3600u));
            set_stat(4, "IDLE", buf);
        } else {
            snprintf(buf, sizeof(buf), "%lum", (unsigned long)(snap.idle_for_sec / 60u));
            set_stat(4, "IDLE", buf);
        }
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)snap.total_sessions);
        set_stat(5, "SESS", buf);
    } else {
        char ghost_name[16];
        unsigned long total_captures = (unsigned long)snap.handshakes;
        ghostchi_progress_t progress;
        const char *mood;
        ghostchi_get_progress(&snap, &progress);
        if (!snap.sd_ready) mood = "blocked";
        else if (snap.running) mood = "hunting";
        else if (ghostchi_is_thriving(&snap)) mood = "thriving";
        else if (idle_hours(&snap) >= 24) mood = "restless";
        else if (idle_hours(&snap) >= 8) mood = "drowsy";
        else if (total_captures > 0) mood = "proud";
        else if (snap.total_sessions > 0) mood = "hopeful";
        else mood = "eager";
        
        generate_ghostchi_name(ghost_name, sizeof(ghost_name));
        set_stat(0, "NAME", ghost_name);
        snprintf(buf, sizeof(buf), "%s", mood);
        capitalize_ascii_first(buf);
        set_stat(1, "MOOD", buf);
        snprintf(buf, sizeof(buf), "%u", progress.level);
        set_stat(2, "LEVEL", buf);
        snprintf(buf, sizeof(buf), "%lu", total_captures);
        set_stat(3, "CAPTURES", total_captures ? buf : "--");
        if (LV_VER_RES <= 135) {
            snprintf(buf, sizeof(buf), "%u%%", (total_captures > 0 && snap.attempts > 0)
                     ? (unsigned)((total_captures * 100u) / snap.attempts) : 0);
            set_stat(4, "RATE", (total_captures > 0 && snap.attempts > 0) ? buf : "--");
            lv_obj_add_flag(s_stats[5][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_stats[5][1], LV_OBJ_FLAG_HIDDEN);
        } else {
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)snap.total_sessions);
            set_stat(4, "RUNS", snap.total_sessions ? buf : "--");
            if (total_captures > 0 && snap.attempts > 0) {
                unsigned long successful_attempts = total_captures;
                if (successful_attempts > snap.attempts) {
                    successful_attempts = snap.attempts;
                }
                unsigned int rate = (unsigned)((successful_attempts * 100u) / snap.attempts);
                snprintf(buf, sizeof(buf), "%u%%", rate);
            } else {
                snprintf(buf, sizeof(buf), "--");
            }
            set_stat(5, "RATE", buf);
        }
        
        int xp_bar_width, bar_x;
        if (is_portrait_layout()) {
            xp_bar_width = LV_HOR_RES - 72;
            bar_x = (LV_HOR_RES - xp_bar_width) / 2;
        } else {
            int sp = (LV_HOR_RES * split_pct()) / 100;
            int panel_x = sp + 12;
            int panel_w = LV_HOR_RES - sp - 24;
            xp_bar_width = panel_w - 8;
            if (xp_bar_width < 20) xp_bar_width = 20;
            bar_x = panel_x + (panel_w - xp_bar_width) / 2;
        }
        lv_obj_set_size(s_xp_bar, xp_bar_width, 8);
        int xp_fill_max = xp_bar_width;
        unsigned int xp_in_level = progress.xp_into_level;
        unsigned int xp_for_next = progress.xp_for_next;
        
        if (progress.max_level) {
            xp_in_level = xp_for_next;
        }
        
        int fill_width = (xp_for_next > 0)
                             ? (xp_fill_max * (int)xp_in_level) / (int)xp_for_next
                             : xp_fill_max;
        if (fill_width < 4 && xp_in_level > 0) fill_width = 4;
        if (fill_width > xp_fill_max) fill_width = xp_fill_max;
        if (fill_width > 0) fill_width += 1;
        
        lv_obj_set_size(s_xp_fill, fill_width, 8);
        lv_obj_clear_flag(s_xp_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_xp_fill, LV_OBJ_FLAG_HIDDEN);
        if (is_portrait_layout()) {
            lv_obj_clear_flag(s_xp_left_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_xp_right_label, LV_OBJ_FLAG_HIDDEN);
        }
        
        snprintf(buf, sizeof(buf), "%u", xp_in_level);
        lv_label_set_text(s_xp_left_label, buf);
        if (progress.max_level) snprintf(buf, sizeof(buf), "MAX");
        else snprintf(buf, sizeof(buf), "%u", xp_for_next);
        lv_label_set_text(s_xp_right_label, buf);
        
        int xp_y;
        if (is_portrait_layout()) {
            int row_start = (LV_VER_RES * 48) / 100;
            int row_step = 15;
            xp_y = row_start + (6 * row_step) + 2;
        } else {
            int content_h_local = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
            bool compact = content_h_local <= 120;
            int row_start = compact ? 10 : 14;
            int row_step = compact ? 11 : 16;
            xp_y = row_start + (6 * row_step) + 2;
        }
        lv_obj_set_pos(s_xp_bar, bar_x, xp_y);
        if (is_portrait_layout()) {
            lv_obj_set_pos(s_xp_left_label, bar_x - 20, xp_y + 1);
            lv_obj_set_pos(s_xp_right_label, bar_x + xp_bar_width + 4, xp_y + 1);
        }
    }
    
    if (s_page != 2) {
        lv_obj_add_flag(s_xp_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_xp_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_xp_left_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_xp_right_label, LV_OBJ_FLAG_HIDDEN);
    }
    layout_stats();
}

static void switch_page(int delta) {
    s_page += delta;
    if (s_page < 0) s_page = 2;
    if (s_page > 2) s_page = 0;
    update_ui(NULL);
}

static void handle_select(void) {
    ghostchi_snapshot_t snap;
    ghostchi_manager_get_snapshot(&snap);
    if (snap.running) {
        ghostchi_manager_stop();
        return;
    }
    if (!ghostchi_manager_start()) {
        error_popup_create("Ghostchi requires a mounted SD card");
    }
}

static void normalize_input(InputEvent *event, bool *up, bool *down, bool *left, bool *right, bool *select, bool *back) {
    *up = *down = *left = *right = *select = *back = false;
    if (!event) return;
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            if (!event->data.joystick_pressed) return;
            if (event->data.joystick_index == 2) *up = true;
            else if (event->data.joystick_index == 4) *down = true;
            else if (event->data.joystick_index == 0) *left = true, *back = true;
            else if (event->data.joystick_index == 3) *right = true;
            else if (event->data.joystick_index == 1) *select = true;
            break;
        case INPUT_TYPE_KEYBOARD:
            if (event->data.key_value == LV_KEY_UP || event->data.key_value == ';' || event->data.key_value == 'k') *up = true;
            else if (event->data.key_value == LV_KEY_DOWN || event->data.key_value == '.' || event->data.key_value == 'j') *down = true;
            else if (event->data.key_value == LV_KEY_LEFT || event->data.key_value == ',' || event->data.key_value == 'h') *left = true;
            else if (event->data.key_value == LV_KEY_RIGHT || event->data.key_value == '/' || event->data.key_value == 'l') *right = true;
            else if (event->data.key_value == LV_KEY_ENTER || event->data.key_value == '\n' || event->data.key_value == '\r' || event->data.key_value == 13) *select = true;
            else if (event->data.key_value == LV_KEY_ESC || event->data.key_value == 27 || event->data.key_value == 29 || event->data.key_value == '`') *back = true;
            break;
        case INPUT_TYPE_ENCODER:
            if (event->data.encoder.button) *select = true;
            else if (event->data.encoder.direction < 0) *up = true;
            else if (event->data.encoder.direction > 0) *down = true;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            *back = true;
            break;
        default:
            break;
    }
}

static void ghostchi_input_handler(InputEvent *event) {
    bool up, down, left, right, select, back;
    ghostchi_snapshot_t snap;
    if (!event) return;

    if (has_touch_controls() && event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            s_touch_started = true;
            s_touch_start_x = data->point.x;
            s_touch_start_y = data->point.y;
            return;
        }
        if (data->state == LV_INDEV_STATE_REL && s_touch_started) {
            int dx = data->point.x - s_touch_start_x;
            int dy = data->point.y - s_touch_start_y;
            int gx;
            int gy;
            s_touch_started = false;
            if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                if (data->point.y > (LV_VER_RES - 26)) {
                    int zone = (data->point.x * 3) / LV_HOR_RES;
                    if (zone < 0) zone = 0;
                    if (zone > 2) zone = 2;
                    handle_footer_action(zone);
                    return;
                }
                gx = lv_obj_get_x(s_ghost);
                gy = lv_obj_get_y(s_ghost);
                if (data->point.x >= (gx - 10) && data->point.x <= (gx + GHOST_W + 10) &&
                    data->point.y >= (gy - 10) && data->point.y <= (gy + GHOST_H + 10)) {
                    handle_select();
                }
            }
        }
        return;
    }

    normalize_input(event, &up, &down, &left, &right, &select, &back);
    if (has_encoder_buttons()) {
        if (up || left) {
            if (--s_encoder_btn_focus < 0) s_encoder_btn_focus = 2;
            encoder_btn_update_focus();
            return;
        }
        if (down || right) {
            if (++s_encoder_btn_focus > 2) s_encoder_btn_focus = 0;
            encoder_btn_update_focus();
            return;
        }
        if (select) {
            handle_footer_action(s_encoder_btn_focus);
            return;
        }
    } else {
        if (left || up) switch_page(-1);
        if (right || down) switch_page(1);
        if (select) handle_select();
    }
    if (back) {
        ghostchi_manager_get_snapshot(&snap);
        if (snap.running) {
            ghostchi_manager_stop();
        } else {
            display_manager_switch_view(&apps_menu_view);
        }
    }
}

static void get_ghostchi_callback(void **callback) {
    if (callback) *callback = (void *)ghostchi_input_handler;
}

void ghostchi_create(void) {
    load_colors();
    display_manager_fill_screen(lv_color_hex(color_bg));
    s_root = gui_screen_create_root(NULL, "Ghostchi", lv_color_hex(color_bg), LV_OPA_COVER);
    ghostchi_view.root = s_root;
    s_content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);

    s_art = lv_obj_create(s_content);
    lv_obj_set_size(s_art, LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_set_style_bg_opa(s_art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_art, 0, 0);
    lv_obj_clear_flag(s_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_art, ghostchi_draw_event, LV_EVENT_DRAW_MAIN, NULL);

    s_ghost = lv_img_create(s_content);
    lv_img_set_src(s_ghost, &happy_50x50);

    s_bubble_box = lv_img_create(s_content);
    lv_img_set_src(s_bubble_box, &speech);
    lv_img_set_zoom(s_bubble_box, 512);
    lv_obj_set_style_img_recolor(s_bubble_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_img_recolor_opa(s_bubble_box, LV_OPA_COVER, 0);

    s_bubble_label = lv_label_create(s_content);
    lv_label_set_long_mode(s_bubble_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_bubble_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_bubble_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_bubble_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bubble_label, 0, 0);

    s_state_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_state_label, accessibility_get_font_title(), 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(color_text), 0);

    s_reason_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_reason_label, accessibility_get_font_body(), 0);
    lv_obj_set_style_text_color(s_reason_label, lv_color_hex(color_muted), 0);

    s_hint_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_hint_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(color_muted), 0);

    s_touch_btn_left = lv_btn_create(s_content);
    s_touch_btn_mid = lv_btn_create(s_content);
    s_touch_btn_right = lv_btn_create(s_content);
    lv_obj_set_style_bg_color(s_touch_btn_left, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_color(s_touch_btn_mid, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_color(s_touch_btn_right, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_touch_btn_left, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_touch_btn_mid, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_touch_btn_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_touch_btn_left, 1, 0);
    lv_obj_set_style_border_width(s_touch_btn_mid, 1, 0);
    lv_obj_set_style_border_width(s_touch_btn_right, 1, 0);
    lv_obj_set_style_border_color(s_touch_btn_left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_touch_btn_mid, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_touch_btn_right, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_touch_btn_left, 6, 0);
    lv_obj_set_style_radius(s_touch_btn_mid, 6, 0);
    lv_obj_set_style_radius(s_touch_btn_right, 6, 0);
    lv_obj_clear_flag(s_touch_btn_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_touch_btn_mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_touch_btn_right, LV_OBJ_FLAG_CLICKABLE);
    if (!has_touch_controls()) {
        lv_obj_add_flag(s_touch_btn_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_touch_btn_mid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_touch_btn_right, LV_OBJ_FLAG_HIDDEN);
    }

    s_touch_btn_left_label = lv_label_create(s_touch_btn_left);
    s_touch_btn_mid_label = lv_label_create(s_touch_btn_mid);
    s_touch_btn_right_label = lv_label_create(s_touch_btn_right);
    lv_obj_center(s_touch_btn_left_label);
    lv_obj_center(s_touch_btn_mid_label);
    lv_obj_center(s_touch_btn_right_label);
    lv_obj_set_style_text_font(s_touch_btn_left_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_font(s_touch_btn_mid_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_font(s_touch_btn_right_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_touch_btn_left_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_touch_btn_mid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_touch_btn_right_label, lv_color_hex(0xFFFFFF), 0);

    s_encoder_btn_focus = 1;
    for (int i = 0; i < 3; ++i) {
        s_encoder_btns[i] = lv_obj_create(s_content);
        lv_obj_set_style_bg_color(s_encoder_btns[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_encoder_btns[i], LV_OPA_20, 0);
        lv_obj_set_style_border_color(s_encoder_btns[i], lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(s_encoder_btns[i], 1, 0);
        lv_obj_set_style_radius(s_encoder_btns[i], 4, 0);
        lv_obj_set_style_pad_all(s_encoder_btns[i], 0, 0);
        lv_obj_clear_flag(s_encoder_btns[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_encoder_btns[i], LV_OBJ_FLAG_HIDDEN);
        s_encoder_btn_labels[i] = lv_label_create(s_encoder_btns[i]);
        lv_obj_set_style_text_font(s_encoder_btn_labels[i], accessibility_get_font_small(), 0);
        lv_obj_set_style_text_color(s_encoder_btn_labels[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(s_encoder_btn_labels[i]);
    }

    for (int i = 0; i < 6; ++i) {
        s_stats[i][0] = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_stats[i][0], accessibility_get_font_small(), 0);
        lv_obj_set_style_text_color(s_stats[i][0], lv_color_hex(color_muted), 0);
        lv_obj_set_style_text_align(s_stats[i][0], LV_TEXT_ALIGN_RIGHT, 0);
        s_stats[i][1] = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_stats[i][1], is_portrait_layout() ? accessibility_get_font_small() : accessibility_get_font_body(), 0);
        lv_obj_set_style_text_color(s_stats[i][1], lv_color_hex(color_text), 0);
        lv_obj_set_style_text_align(s_stats[i][1], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(s_stats[i][1], LV_LABEL_LONG_CLIP);
    }

    s_xp_bar = lv_obj_create(s_content);
    lv_obj_set_size(s_xp_bar, LV_HOR_RES - 72, 8);
    lv_obj_set_style_bg_color(s_xp_bar, lv_color_hex(color_surface_alt), 0);
    lv_obj_set_style_bg_opa(s_xp_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_xp_bar, 1, 0);
    lv_obj_set_style_border_color(s_xp_bar, lv_color_hex(0x666666), 0);
    lv_obj_set_style_radius(s_xp_bar, 4, 0);
    lv_obj_set_style_clip_corner(s_xp_bar, true, 0);
    lv_obj_set_style_pad_all(s_xp_bar, 0, 0);
    lv_obj_add_flag(s_xp_bar, LV_OBJ_FLAG_HIDDEN);

    s_xp_fill = lv_obj_create(s_xp_bar);
    lv_obj_set_size(s_xp_fill, 0, 8);
    lv_obj_set_pos(s_xp_fill, -1, 0);
    lv_obj_set_style_bg_color(s_xp_fill, lv_color_hex(0x666666), 0);
    lv_obj_set_style_bg_opa(s_xp_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_xp_fill, 0, 0);
    lv_obj_set_style_radius(s_xp_fill, 4, 0);
    lv_obj_set_style_pad_all(s_xp_fill, 0, 0);
    lv_obj_clear_flag(s_xp_fill, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_xp_left_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_xp_left_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_xp_left_label, lv_color_hex(color_text), 0);
    lv_obj_add_flag(s_xp_left_label, LV_OBJ_FLAG_HIDDEN);

    s_xp_right_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_xp_right_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_xp_right_label, lv_color_hex(color_muted), 0);
    lv_obj_add_flag(s_xp_right_label, LV_OBJ_FLAG_HIDDEN);

    ghostchi_manager_probe_storage();
    s_timer = lv_timer_create(update_ui, 250, NULL);
    update_ui(NULL);
}

void ghostchi_destroy(void) {
    ghostchi_manager_stop();
    ghostchi_manager_tick();
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    lvgl_obj_del_safe(&s_root);
    ghostchi_view.root = NULL;
    s_root = NULL;
    s_content = NULL;
    s_art = NULL;
    s_ghost = NULL;
    s_bubble_box = NULL;
    s_bubble_label = NULL;
    s_state_label = NULL;
    s_reason_label = NULL;
    s_hint_label = NULL;
    s_touch_btn_left = NULL;
    s_touch_btn_mid = NULL;
    s_touch_btn_right = NULL;
    s_touch_btn_left_label = NULL;
    s_touch_btn_mid_label = NULL;
    s_touch_btn_right_label = NULL;
    s_xp_bar = NULL;
    s_xp_fill = NULL;
    s_xp_left_label = NULL;
    s_xp_right_label = NULL;
    for (int i = 0; i < 3; ++i) {
        s_encoder_btns[i] = NULL;
        s_encoder_btn_labels[i] = NULL;
    }
    s_page = 0;
    s_encoder_btn_focus = 1;
}

View ghostchi_view = {
    .root = NULL,
    .create = ghostchi_create,
    .destroy = ghostchi_destroy,
    .input_callback = ghostchi_input_handler,
    .name = "Ghostchi",
    .get_hardwareinput_callback = get_ghostchi_callback,
};
