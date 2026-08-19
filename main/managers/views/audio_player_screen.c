#include "managers/views/audio_player_screen.h"
#include "managers/audio_stream_manager.h"
#include "managers/audio_i2s_output.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/toast.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gui/design_tokens.h"
#ifdef CONFIG_HAS_TLV320DAC_I2C
#include "io_manager.h"
#include "tlv320dac3100.h"
#endif
#include "freertos/task.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER

static const char *TAG = "AudioPlayer";

/* Audio Player is reachable both from Apps Gallery and directly from the
 * Main Menu's "Audio" item; back/ESC must return to whichever one actually
 * opened it, not a fixed destination. */
static View *s_return_view = NULL;

void audio_player_set_return_view(View *view) {
    s_return_view = view;
}

/* Bottom touch bar, same style as other views (badusb/settings/etc): a
 * full-width bar with a centered Back button. Height matches
 * SCROLL_BTN_SIZE + 2*SCROLL_BTN_PADDING used by those views. */
#define AUDIO_TOUCH_BAR_H 34

/* The view has two full-screen sub-screens: a Library (track list) and a
 * dedicated Now Playing screen. Tapping a track opens Now Playing; its back
 * control returns to the Library; the Library's back exits the whole view. */
typedef enum {
    SCREEN_LIBRARY = 0,
    SCREEN_NOWPLAYING,
} audio_screen_t;

static audio_screen_t s_screen = SCREEN_LIBRARY;

/* UI element handles */
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_library_cont = NULL;
static lv_obj_t *s_np_cont = NULL;

/* Library */
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_list_hint_label = NULL;
static lv_obj_t *s_lib_back_btn = NULL;

/* Now Playing */
static lv_obj_t *s_np_title = NULL;
static lv_obj_t *s_meta_label = NULL;
static lv_obj_t *s_progress_track = NULL;
static lv_obj_t *s_progress_fill = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_pause_btn = NULL;
static lv_obj_t *s_prev_btn = NULL;
static lv_obj_t *s_next_btn = NULL;
static lv_obj_t *s_np_back_btn = NULL;
static lv_obj_t *s_vol_track = NULL;
static lv_obj_t *s_vol_fill = NULL;
static lv_obj_t *s_vol_minus_btn = NULL;
static lv_obj_t *s_vol_plus_btn = NULL;
static lv_obj_t *s_volume_label = NULL;

/* State */
static int s_selected_index = 0;
static int s_visible_count = 0;
static uint8_t s_volume_percent = 85;
static lv_timer_t *s_update_timer = NULL;
static bool s_vol_dragging = false;

static touch_drag_t s_touch_drag = {0};

static lv_color_t s_bg_color;
static lv_color_t s_surface_color;
static lv_color_t s_surface_alt_color;
static lv_color_t s_text_color;
static lv_color_t s_dim_color;
static lv_color_t s_accent_color;
static int s_last_rendered_playing_index = -2;
static audio_stream_state_t s_last_rendered_state = AUDIO_STREAM_STATE_IDLE;

/* Forward declarations */
static void refresh_theme_colors(void);
static void create_file_list(void);
static void update_file_list_selection(void);
static void audio_player_update_status(void);
static void update_timer_cb(lv_timer_t *timer);
static void audio_player_go_back(void);
static void show_library(void);
static void show_nowplaying(void);
static void apply_volume(uint8_t pct);
static void update_volume_ui(void);
static void adjust_volume(int delta);
static bool play_track_with_toast(int index);
static bool change_track_with_toast(bool next);
static bool point_in_obj(lv_obj_t *obj, int x, int y);

static void refresh_theme_colors(void)
{
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    s_bg_color = lv_color_hex(theme_palette_get_background(theme));
    s_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    s_surface_alt_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    s_text_color = lv_color_hex(theme_palette_get_text(theme));
    s_dim_color = lv_color_hex(theme_palette_get_text_muted(theme));
    s_accent_color = lv_color_hex(theme_palette_get_accent(theme));
}

static void style_track_row(lv_obj_t *obj, bool selected, bool playing)
{
    lv_obj_set_style_bg_color(obj, (selected || playing) ? s_surface_alt_color : s_surface_color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, playing ? s_accent_color : (selected ? s_dim_color : s_surface_alt_color), 0);
    lv_obj_set_style_border_width(obj, playing ? 2 : 1, 0);
    lv_obj_set_style_border_side(obj, (selected || playing) ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(obj, 9, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void update_list_hint(void)
{
    if (!s_list_hint_label) return;

    int total = audio_stream_manager_get_file_count();
    if (total <= 0) {
        lv_label_set_text(s_list_hint_label, "0 tracks");
        return;
    }

    char text[32];
    snprintf(text, sizeof(text), "%d/%d", s_selected_index + 1, total);
    lv_label_set_text(s_list_hint_label, text);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static bool point_in_obj(lv_obj_t *obj, int x, int y)
{
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static void check_sd_and_show_toast(void)
{
    if (audio_stream_manager_get_file_count() > 0) {
        return;
    }
    if (!audio_stream_manager_sd_available()) {
        toast_show("No SD card - insert card with MP3s in /audio", TOAST_WARN);
    } else {
        toast_show("No MP3 files found in /audio", TOAST_INFO);
    }
}

static void format_time(char *buf, size_t buf_len, size_t seconds)
{
    if (seconds >= 3600) {
        snprintf(buf, buf_len, "%lu:%02lu:%02lu",
                 (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds % 3600) / 60),
                 (unsigned long)(seconds % 60));
    } else {
        snprintf(buf, buf_len, "%lu:%02lu",
                 (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    }
}

static uint32_t get_estimated_playback_ms(size_t pos, size_t total_size, uint32_t duration_ms)
{
    /* Once the local receiver or GhostLink peer has reported a playback
     * position at all, trust it completely - including 0 while the
     * prebuffer fills at the start of a track. The byte-offset fallbacks
     * below track the sender, which runs far ahead of the audible position
     * during the initial prebuffer and would make the bar jump around. */
    if (audio_stream_manager_has_receiver_feedback()) {
        return audio_stream_manager_get_playback_ms();
    }

    /* No feedback ever (e.g. a peer that never reports): estimate from the
     * sender-side offsets. */
    uint16_t bitrate = audio_stream_manager_get_bitrate();
    if (bitrate > 0) {
        uint32_t bytes_per_sec = (uint32_t)bitrate * 125;
        if (bytes_per_sec > 0) return (uint32_t)(((uint64_t)pos * 1000) / bytes_per_sec);
    }

    if (total_size > 0 && duration_ms > 0) {
        return (uint32_t)(((uint64_t)pos * duration_ms) / total_size);
    }

    return 0;
}

/* ---------------- Volume ---------------- */

static void update_volume_ui(void)
{
    if (s_vol_track && s_vol_fill && lv_obj_is_valid(s_vol_track)) {
        lv_coord_t w = lv_obj_get_width(s_vol_track);
        lv_coord_t h = lv_obj_get_height(s_vol_track);
        lv_obj_set_size(s_vol_fill, (w * s_volume_percent) / 100, h);
    }
    if (s_volume_label) {
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "Vol %u%%", (unsigned)s_volume_percent);
        lv_label_set_text(s_volume_label, vbuf);
    }
}

static void apply_volume(uint8_t pct)
{
    if (pct > 100) pct = 100;
#ifdef CONFIG_HAS_TLV320DAC_I2C
    esp_err_t ret = tlv320dac3100_set_volume(pct);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(ret));
    }
#else
    /* Routed by the I2S output: the AW88298 has its own attenuator and uses
     * it, other codecs fall back to software PCM scaling. */
    audio_i2s_output_set_volume(pct);
#endif
    s_volume_percent = pct;
    update_volume_ui();
}

static void adjust_volume(int delta)
{
    int volume = (int)s_volume_percent + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == (int)s_volume_percent) return;
    apply_volume((uint8_t)volume);
    ESP_LOGI(TAG, "Audio volume: %u%%", (unsigned)s_volume_percent);
}

/* Map an absolute touch x within the volume track to a 0-100 percentage. */
static void set_volume_from_x(int x)
{
    if (!s_vol_track || !lv_obj_is_valid(s_vol_track)) return;
    lv_area_t a;
    lv_obj_get_coords(s_vol_track, &a);
    int w = a.x2 - a.x1;
    if (w <= 0) return;
    int rel = x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    apply_volume((uint8_t)((rel * 100) / w));
}

/* ---------------- Library list ---------------- */

static void create_file_list(void)
{
    int file_count = audio_stream_manager_get_file_count();
    s_visible_count = file_count;
    s_last_rendered_playing_index = audio_stream_manager_get_current_index();
    s_last_rendered_state = audio_stream_manager_get_state();

    if (!s_file_list) return;

    lv_obj_clean(s_file_list);

    if (file_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_file_list);
        lv_label_set_text(lbl, "No MP3 files found");
        lv_obj_set_style_text_color(lbl, s_dim_color, 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_body(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    audio_stream_state_t state = audio_stream_manager_get_state();
    int playing_index = audio_stream_manager_get_current_index();
    bool has_playing = (state == AUDIO_STREAM_STATE_PLAYING || state == AUDIO_STREAM_STATE_PAUSED) &&
                       playing_index >= 0 && playing_index < file_count;

    for (int i = 0; i < file_count; i++) {
        const char *fname = audio_stream_manager_get_filename(i);
        if (!fname) continue;

        bool selected = i == s_selected_index;
        bool playing = has_playing && i == playing_index;
        lv_obj_t *btn = lv_btn_create(s_file_list);
        gui_apply_pressed_style(btn);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, LV_VER_RES <= 160 ? 30 : 34);
        style_track_row(btn, selected, playing);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        char idx_text[8];
        snprintf(idx_text, sizeof(idx_text), playing ? ">" : "%02d", i + 1);
        lv_obj_t *idx_lbl = lv_label_create(btn);
        lv_label_set_text(idx_lbl, idx_text);
        lv_obj_set_style_text_color(idx_lbl, playing ? s_accent_color : s_dim_color, 0);
        lv_obj_set_style_text_font(idx_lbl, accessibility_get_font_small(), 0);
        lv_obj_align(idx_lbl, LV_ALIGN_LEFT_MID, 7, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fname);
        lv_obj_set_style_text_color(lbl, playing ? s_accent_color : (selected ? s_text_color : s_dim_color), 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_small(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(lbl, LV_HOR_RES - 92);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 34, 0);

        lv_obj_t *badge = lv_label_create(btn);
        lv_label_set_text(badge, playing ? (state == AUDIO_STREAM_STATE_PAUSED ? "PAUSE" : "PLAY") : (selected ? "SEL" : ""));
        lv_obj_set_style_text_color(badge, playing ? s_accent_color : s_dim_color, 0);
        lv_obj_set_style_text_font(badge, accessibility_get_font_small(), 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    update_list_hint();
}

static void update_file_list_selection(void)
{
    if (!s_file_list) return;
    if (s_visible_count <= 0) {
        update_list_hint();
        return;
    }

    uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
    audio_stream_state_t state = audio_stream_manager_get_state();
    int playing_index = audio_stream_manager_get_current_index();
    bool has_playing = (state == AUDIO_STREAM_STATE_PLAYING || state == AUDIO_STREAM_STATE_PAUSED) &&
                       playing_index >= 0 && playing_index < s_visible_count;

    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, i);
        if (!btn) continue;
        bool selected = ((int)i == s_selected_index);
        bool playing = has_playing && ((int)i == playing_index);
        style_track_row(btn, selected, playing);

        lv_obj_t *idx_lbl = lv_obj_get_child(btn, 0);
        if (idx_lbl) {
            char idx_text[8];
            snprintf(idx_text, sizeof(idx_text), playing ? ">" : "%02lu", (unsigned long)i + 1);
            lv_label_set_text(idx_lbl, idx_text);
            lv_obj_set_style_text_color(idx_lbl, playing ? s_accent_color : s_dim_color, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, playing ? s_accent_color : (selected ? s_text_color : s_dim_color), 0);
        }

        lv_obj_t *badge = lv_obj_get_child(btn, 2);
        if (badge) {
            lv_label_set_text(badge, playing ? (state == AUDIO_STREAM_STATE_PAUSED ? "PAUSE" : "PLAY") : (selected ? "SEL" : ""));
            lv_obj_set_style_text_color(badge, playing ? s_accent_color : s_dim_color, 0);
        }
    }

    if (s_selected_index >= 0 && s_selected_index < (int)child_cnt) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, s_selected_index);
        if (btn) lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
    }
    update_list_hint();
}

/* ---------------- Status / Now Playing refresh ---------------- */

static void audio_player_update_status(void)
{
    audio_stream_state_t state = audio_stream_manager_get_state();
    int current = audio_stream_manager_get_current_index();
    int total = audio_stream_manager_get_file_count();

    /* The precheck runs on a worker task, so a refusal (e.g. bitrate too
     * high for the GhostLink link) surfaces asynchronously here. */
    esp_err_t play_err = audio_stream_manager_consume_last_error();
    if (play_err == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    }

    if (current != s_last_rendered_playing_index || state != s_last_rendered_state) {
        s_last_rendered_playing_index = current;
        s_last_rendered_state = state;
        update_file_list_selection();
    }

    bool is_playing = (state == AUDIO_STREAM_STATE_PLAYING);
    if (s_play_btn) {
        if (is_playing) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pause_btn) {
        if (is_playing) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }

    const char *state_str = "Stopped";
    switch (state) {
        case AUDIO_STREAM_STATE_PLAYING: state_str = "Playing"; break;
        case AUDIO_STREAM_STATE_PAUSED:  state_str = "Paused";  break;
        case AUDIO_STREAM_STATE_STOPPED: state_str = "Stopped"; break;
        default: break;
    }

    /* Title: prefer the actually-playing track, else the current selection. */
    int show_index = (total > 0 && current >= 0 && current < total) ? current : s_selected_index;
    if (s_np_title) {
        const char *fname = (show_index >= 0 && show_index < total)
                                ? audio_stream_manager_get_filename(show_index) : NULL;
        lv_label_set_text(s_np_title, fname ? fname : (total > 0 ? "Select a track" : "No MP3 files"));
    }

    /* Progress + time */
    size_t pos = audio_stream_manager_get_position();
    size_t total_size = audio_stream_manager_get_total_size();
    uint32_t duration_ms = audio_stream_manager_get_duration_ms();
    uint32_t played_ms = get_estimated_playback_ms(pos, total_size, duration_ms);
    if (duration_ms > 0 && played_ms > duration_ms) played_ms = duration_ms;
    if (s_progress_fill) {
        lv_coord_t track_w = s_progress_track ? lv_obj_get_width(s_progress_track) : (LV_HOR_RES - 24);
        lv_coord_t track_h = s_progress_track ? lv_obj_get_height(s_progress_track) : 8;
        int32_t pct = 0;
        if (duration_ms > 0) pct = (int32_t)(((uint64_t)played_ms * 100) / duration_ms);
        else if (total_size > 0) pct = (int32_t)((pos * 100) / total_size);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        lv_obj_set_size(s_progress_fill, (track_w * pct) / 100, track_h);
    }

    if (s_time_label) {
        char tbuf[32];
        char tbuf2[32];
        format_time(tbuf, sizeof(tbuf), played_ms / 1000);
        if (duration_ms > 0) {
            format_time(tbuf2, sizeof(tbuf2), duration_ms / 1000);
        } else {
            /* Unknown length: show it instead of a misleading 0:00. */
            snprintf(tbuf2, sizeof(tbuf2), "--:--");
        }
        char tline[80];
        snprintf(tline, sizeof(tline), "%s / %s", tbuf, tbuf2);
        lv_label_set_text(s_time_label, tline);
    }

    if (s_meta_label) {
        uint16_t bitrate = audio_stream_manager_get_bitrate();
        char mbuf[64];
        if (total > 0) {
            if (bitrate > 0) snprintf(mbuf, sizeof(mbuf), "%s  -  %d/%d  -  %uk", state_str, show_index + 1, total, (unsigned)bitrate);
            else snprintf(mbuf, sizeof(mbuf), "%s  -  %d/%d", state_str, show_index + 1, total);
        } else {
            snprintf(mbuf, sizeof(mbuf), "%s", state_str);
        }
        lv_label_set_text(s_meta_label, mbuf);
    }
}

/* ---------------- Playback helpers ---------------- */

static bool play_track_with_toast(int index)
{
    esp_err_t ret = audio_stream_manager_play(index);
    if (ret == ESP_OK) {
        return true;
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    } else {
        toast_show_duration("Could not play MP3", TOAST_WARN, 1600);
    }
    ESP_LOGW(TAG, "Audio play failed for index %d: %s", index, esp_err_to_name(ret));
    return false;
}

static bool change_track_with_toast(bool next)
{
    esp_err_t ret = next ? audio_stream_manager_next() : audio_stream_manager_prev();
    if (ret == ESP_OK) {
        return true;
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    } else {
        toast_show_duration("Could not play MP3", TOAST_WARN, 1600);
    }
    ESP_LOGW(TAG, "Audio %s failed: %s", next ? "next" : "previous", esp_err_to_name(ret));
    return false;
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_player_update_status();
}

static void audio_player_go_back(void)
{
    s_return_view = NULL;
    display_manager_go_back();
}

/* ---------------- Screen switching ---------------- */

static void show_library(void)
{
    s_screen = SCREEN_LIBRARY;
    if (s_np_cont) lv_obj_add_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_library_cont) lv_obj_clear_flag(s_library_cont, LV_OBJ_FLAG_HIDDEN);
    update_file_list_selection();
}

static void show_nowplaying(void)
{
    s_screen = SCREEN_NOWPLAYING;
    if (s_library_cont) lv_obj_add_flag(s_library_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_np_cont) lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
    update_volume_ui();
    audio_player_update_status();
}

/* ---------------- Construction ---------------- */

static lv_obj_t *make_round_btn(lv_obj_t *parent, int w, int h, const char *sym, lv_color_t txt)
{
    lv_obj_t *btn = lv_btn_create(parent);
    gui_apply_pressed_style(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = create_label(btn, sym, accessibility_get_font_body(), txt);
    lv_obj_center(lbl);
    return btn;
}

/* Bottom touch bar with a centered Back button, styled like the bars on
 * other views. Returns the back button (or NULL on non-touch builds, in
 * which case hardware back input still works as usual). */
static lv_obj_t *create_touch_bar(lv_obj_t *parent)
{
#ifdef CONFIG_USE_TOUCHSCREEN
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES, AUDIO_TOUCH_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, s_bg_color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *back_btn = lv_btn_create(bar);
    gui_apply_pressed_style(back_btn);
    lv_obj_set_size(back_btn, 52, AUDIO_TOUCH_BAR_H - 6);
    lv_obj_align(back_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(back_btn, s_surface_alt_color, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);

    lv_obj_t *label = create_label(back_btn, "Back", accessibility_get_font_small(), s_text_color);
    lv_obj_center(label);
    return back_btn;
#else
    (void)parent;
    return NULL;
#endif
}

static void build_library(int status_bar_h)
{
    int screen_h = LV_VER_RES - status_bar_h;
    int header_h = 30;
    int list_h = screen_h - header_h - AUDIO_TOUCH_BAR_H - 6;

    s_library_cont = lv_obj_create(s_root);
    lv_obj_set_size(s_library_cont, LV_PCT(100), screen_h);
    lv_obj_align(s_library_cont, LV_ALIGN_TOP_MID, 0, status_bar_h);
    lv_obj_set_style_bg_opa(s_library_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_library_cont, 0, 0);
    lv_obj_set_style_pad_all(s_library_cont, 0, 0);
    lv_obj_clear_flag(s_library_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: count + title */

    s_list_hint_label = lv_label_create(s_library_cont);
    lv_obj_set_style_text_color(s_list_hint_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_list_hint_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_list_hint_label, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_label_set_text(s_list_hint_label, "0 tracks");

    lv_obj_t *title = lv_label_create(s_library_cont);
    lv_label_set_text(title, "Library");
    lv_obj_set_style_text_color(title, s_text_color, 0);
    lv_obj_set_style_text_font(title, accessibility_get_font_small(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* File list */
    s_file_list = lv_obj_create(s_library_cont);
    lv_obj_set_size(s_file_list, LV_HOR_RES - 8, list_h);
    lv_obj_align(s_file_list, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_list, 0, 0);
    lv_obj_set_style_pad_all(s_file_list, 4, 0);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_file_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_file_list, 2, 0);
    lv_obj_clear_flag(s_file_list, LV_OBJ_FLAG_CLICKABLE);

    create_file_list();
    update_list_hint();

    /* Bottom touch bar: Back exits the whole view (same as other views). */
    s_lib_back_btn = create_touch_bar(s_library_cont);
}

static void build_nowplaying(int status_bar_h)
{
    int screen_h = LV_VER_RES - status_bar_h;

    s_np_cont = lv_obj_create(s_root);
    lv_obj_set_size(s_np_cont, LV_PCT(100), screen_h);
    lv_obj_align(s_np_cont, LV_ALIGN_TOP_MID, 0, status_bar_h);
    lv_obj_set_style_bg_opa(s_np_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_np_cont, 0, 0);
    lv_obj_set_style_pad_all(s_np_cont, 0, 0);
    lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);

    /* Track title */
    s_np_title = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_np_title, s_text_color, 0);
    lv_obj_set_style_text_font(s_np_title, accessibility_get_font_title(), 0);
    lv_obj_set_width(s_np_title, LV_HOR_RES - 100);
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_np_title, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(s_np_title, "Ready");

    /* Meta line */
    s_meta_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_meta_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_meta_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_meta_label, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_meta_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_meta_label, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_meta_label, "Stopped");

    /* Progress bar */
    s_progress_track = lv_obj_create(s_np_cont);
    lv_obj_set_size(s_progress_track, LV_HOR_RES - 24, 8);
    lv_obj_align(s_progress_track, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(s_progress_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_progress_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_track, 4, 0);
    lv_obj_set_style_border_width(s_progress_track, 0, 0);
    lv_obj_set_style_pad_all(s_progress_track, 0, 0);
    lv_obj_clear_flag(s_progress_track, LV_OBJ_FLAG_SCROLLABLE);

    s_progress_fill = lv_obj_create(s_progress_track);
    lv_obj_set_size(s_progress_fill, 0, 8);
    lv_obj_align(s_progress_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_fill, 4, 0);
    lv_obj_set_style_border_width(s_progress_fill, 0, 0);
    lv_obj_set_style_pad_all(s_progress_fill, 0, 0);
    lv_obj_clear_flag(s_progress_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_time_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_time_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_time_label, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 66);
    lv_label_set_text(s_time_label, "0:00 / 0:00");

    /* Transport row */
    int transport_y = 88;
    int btn_h = (screen_h > 200) ? 42 : 34;
    s_prev_btn = make_round_btn(s_np_cont, 56, btn_h, LV_SYMBOL_PREV, s_accent_color);
    lv_obj_align(s_prev_btn, LV_ALIGN_TOP_MID, -84, transport_y);

    s_play_btn = make_round_btn(s_np_cont, 64, btn_h, LV_SYMBOL_PLAY, s_accent_color);
    lv_obj_align(s_play_btn, LV_ALIGN_TOP_MID, 0, transport_y);

    s_pause_btn = make_round_btn(s_np_cont, 64, btn_h, LV_SYMBOL_PAUSE, s_accent_color);
    lv_obj_align(s_pause_btn, LV_ALIGN_TOP_MID, 0, transport_y);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);

    s_next_btn = make_round_btn(s_np_cont, 56, btn_h, LV_SYMBOL_NEXT, s_accent_color);
    lv_obj_align(s_next_btn, LV_ALIGN_TOP_MID, 84, transport_y);

    /* Volume row: [-]  [====slider====]  [+] with label above.
     * Anchored above the bottom touch bar. */
    s_volume_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_volume_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_volume_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_BOTTOM_MID, 0, -(AUDIO_TOUCH_BAR_H + 40));
    lv_label_set_text(s_volume_label, "Vol 85%");

    s_vol_minus_btn = make_round_btn(s_np_cont, 40, 30, LV_SYMBOL_MINUS, s_text_color);
    lv_obj_align(s_vol_minus_btn, LV_ALIGN_BOTTOM_LEFT, 6, -(AUDIO_TOUCH_BAR_H + 6));

    s_vol_plus_btn = make_round_btn(s_np_cont, 40, 30, LV_SYMBOL_PLUS, s_text_color);
    lv_obj_align(s_vol_plus_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -(AUDIO_TOUCH_BAR_H + 6));

    s_vol_track = lv_obj_create(s_np_cont);
    lv_obj_set_size(s_vol_track, LV_HOR_RES - 8 - 2 * (40 + 12), 12);
    lv_obj_align(s_vol_track, LV_ALIGN_BOTTOM_MID, 0, -(AUDIO_TOUCH_BAR_H + 15));
    lv_obj_set_style_bg_color(s_vol_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_vol_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_track, 6, 0);
    lv_obj_set_style_border_width(s_vol_track, 0, 0);
    lv_obj_set_style_pad_all(s_vol_track, 0, 0);
    lv_obj_clear_flag(s_vol_track, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_fill = lv_obj_create(s_vol_track);
    lv_obj_set_size(s_vol_fill, 0, 12);
    lv_obj_align(s_vol_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_vol_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_vol_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_fill, 6, 0);
    lv_obj_set_style_border_width(s_vol_fill, 0, 0);
    lv_obj_set_style_pad_all(s_vol_fill, 0, 0);
    lv_obj_clear_flag(s_vol_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* Bottom touch bar: Back returns to the library (same as other views'
     * sub-screen back handling; the library's Back exits the view). */
    s_np_back_btn = create_touch_bar(s_np_cont);
}

void audio_player_create(void)
{
    refresh_theme_colors();
    s_screen = SCREEN_LIBRARY;

#ifdef CONFIG_HAS_TLV320DAC_I2C
    ESP_LOGI(TAG, "Resetting TLV320DAC3100 DAC");
    if (tlv320dac3100_is_initialized()) {
        tlv320dac3100_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_err_t reset_ret = io_manager_dac_reset_pulse();
    if (reset_ret != ESP_OK) {
        ESP_LOGW(TAG, "DAC reset pulse failed: %s", esp_err_to_name(reset_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    tlv320dac3100_config_t dac_cfg = TLV320DAC3100_DEFAULT_CONFIG();
    esp_err_t dac_ret = tlv320dac3100_init(&dac_cfg);
    if (dac_ret != ESP_OK) {
        ESP_LOGW(TAG, "TLV320DAC init failed: %s", esp_err_to_name(dac_ret));
    } else {
        s_volume_percent = 85;
    }
#endif

    esp_err_t ret = audio_stream_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio stream manager init failed: %s", esp_err_to_name(ret));
    }

    s_root = gui_screen_create_root(NULL, "Audio Player", s_bg_color, LV_OPA_COVER);
    audio_player_view.root = s_root;
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int status_bar_h = GUI_STATUS_BAR_H;
    display_manager_add_status_bar("Audio Player");

    build_library(status_bar_h);
    build_nowplaying(status_bar_h);

    /* Apply the starting volume to the active backend + slider. */
    apply_volume(s_volume_percent);

    s_update_timer = lv_timer_create(update_timer_cb, 200, NULL);

    check_sd_and_show_toast();
    audio_player_update_status();

    ESP_LOGI(TAG, "Audio player view created (%d files)", s_visible_count);
}

void audio_player_destroy(void)
{
    if (s_update_timer) {
        lvgl_timer_del_safe(&s_update_timer);
    }

    audio_stream_manager_stop();
    audio_stream_manager_deinit();

    lvgl_obj_del_safe(&s_root);
    audio_player_view.root = NULL;

    s_library_cont = NULL;
    s_np_cont = NULL;
    s_file_list = NULL;
    s_list_hint_label = NULL;
    s_lib_back_btn = NULL;
    s_np_title = NULL;
    s_meta_label = NULL;
    s_progress_track = NULL;
    s_progress_fill = NULL;
    s_time_label = NULL;
    s_play_btn = NULL;
    s_pause_btn = NULL;
    s_prev_btn = NULL;
    s_next_btn = NULL;
    s_np_back_btn = NULL;
    s_vol_track = NULL;
    s_vol_fill = NULL;
    s_vol_minus_btn = NULL;
    s_vol_plus_btn = NULL;
    s_volume_label = NULL;
    s_selected_index = 0;
    s_visible_count = 0;
    s_screen = SCREEN_LIBRARY;
    s_vol_dragging = false;
    touch_drag_reset(&s_touch_drag);
}

/* ---------------- Input ---------------- */

static void toggle_play_pause(void)
{
    audio_stream_state_t st = audio_stream_manager_get_state();
    if (st == AUDIO_STREAM_STATE_PLAYING) {
        audio_stream_manager_pause();
    } else if (st == AUDIO_STREAM_STATE_PAUSED) {
        audio_stream_manager_resume();
    } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
        play_track_with_toast(s_selected_index);
    }
    audio_player_update_status();
}

static void handle_library_tap(lv_indev_data_t *d, int dx, int dy)
{
    if (point_in_obj(s_lib_back_btn, d->point.x, d->point.y) && abs(dx) < 15 && abs(dy) < 15) {
        audio_player_go_back();
        return;
    }
    if (abs(dx) < 10 && abs(dy) < 10 && s_file_list) {
        uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(s_file_list, i);
            if (!child) continue;
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if (d->point.x >= a.x1 && d->point.x <= a.x2 &&
                d->point.y >= a.y1 && d->point.y <= a.y2) {
                s_selected_index = (int)i;
                update_file_list_selection();
                if (play_track_with_toast((int)i)) {
                    show_nowplaying();
                }
                audio_player_update_status();
                break;
            }
        }
    }
}

static void handle_nowplaying_tap(lv_indev_data_t *d, int dx, int dy)
{
    if (abs(dx) >= 15 || abs(dy) >= 15) return;
    int px = (int)d->point.x, py = (int)d->point.y;

    if (point_in_obj(s_np_back_btn, px, py)) { show_library(); return; }
    if (point_in_obj(s_vol_minus_btn, px, py)) { adjust_volume(-5); return; }
    if (point_in_obj(s_vol_plus_btn, px, py)) { adjust_volume(+5); return; }
    if (point_in_obj(s_prev_btn, px, py)) {
        change_track_with_toast(false);
        s_selected_index = audio_stream_manager_get_current_index();
        audio_player_update_status();
        return;
    }
    if (point_in_obj(s_next_btn, px, py)) {
        change_track_with_toast(true);
        s_selected_index = audio_stream_manager_get_current_index();
        audio_player_update_status();
        return;
    }
    if (point_in_obj(s_play_btn, px, py) || point_in_obj(s_pause_btn, px, py)) {
        toggle_play_pause();
        return;
    }
    if (point_in_obj(s_vol_track, px, py)) {
        set_volume_from_x(px);
        return;
    }
}

static void audio_player_input_handler(InputEvent *event)
{
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;

        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_drag.started) {
                touch_drag_begin(&s_touch_drag, d);
                /* Start a volume drag if the press landed on the slider. */
                if (s_screen == SCREEN_NOWPLAYING && point_in_obj(s_vol_track, d->point.x, d->point.y)) {
                    s_vol_dragging = true;
                    set_volume_from_x((int)d->point.x);
                }
                return;
            }
            if (s_vol_dragging) {
                set_volume_from_x((int)d->point.x);
                return;
            }
            /* Live list drag (library only) */
            if (s_screen == SCREEN_LIBRARY && s_file_list && lv_obj_is_valid(s_file_list)) {
                lv_area_t list_area;
                lv_obj_get_coords(s_file_list, &list_area);
                bool started_in_list = (s_touch_drag.start_x >= list_area.x1 && s_touch_drag.start_x <= list_area.x2 &&
                                        s_touch_drag.start_y >= list_area.y1 && s_touch_drag.start_y <= list_area.y2);
                if (started_in_list) {
                    touch_drag_update(&s_touch_drag, d, s_file_list);
                }
            }
            return;
        }

        if (d->state == LV_INDEV_STATE_REL) {
            if (s_vol_dragging) {
                set_volume_from_x((int)d->point.x);
                s_vol_dragging = false;
                touch_drag_reset(&s_touch_drag);
                return;
            }
            if (!s_touch_drag.started) return;
            int dx = (int)d->point.x - s_touch_drag.start_x;
            int dy = (int)d->point.y - s_touch_drag.start_y;

            bool was_dragged = touch_drag_release(&s_touch_drag, d);
            if (was_dragged) return;

            if (s_screen == SCREEN_NOWPLAYING) {
                handle_nowplaying_tap(d, dx, dy);
            } else {
                handle_library_tap(d, dx, dy);
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 0) { /* Left -> back (Now Playing -> Library -> exit) */
            if (s_screen == SCREEN_NOWPLAYING) show_library();
            else audio_player_go_back();
        } else if (btn == 2) { /* Up */
            if (s_screen == SCREEN_NOWPLAYING) {
                adjust_volume(+5);
            } else if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (btn == 4) { /* Down */
            if (s_screen == SCREEN_NOWPLAYING) {
                adjust_volume(-5);
            } else if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (btn == 1) { /* Select */
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        } else if (btn == 3) { /* Right -> next track */
            change_track_with_toast(true);
            s_selected_index = audio_stream_manager_get_current_index();
            update_file_list_selection();
            audio_player_update_status();
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        } else {
            adjust_volume(event->data.encoder.direction > 0 ? 5 : -5);
        }
        return;
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (s_screen == SCREEN_NOWPLAYING) show_library();
        else audio_player_go_back();
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == '`') {
            if (s_screen == SCREEN_NOWPLAYING) show_library();
            else audio_player_go_back();
        } else if (key == LV_KEY_UP || key == 'k') {
            if (s_screen == SCREEN_NOWPLAYING) adjust_volume(+5);
            else if (s_selected_index > 0) { s_selected_index--; update_file_list_selection(); }
        } else if (key == LV_KEY_DOWN || key == 'j') {
            if (s_screen == SCREEN_NOWPLAYING) adjust_volume(-5);
            else if (s_selected_index < s_visible_count - 1) { s_selected_index++; update_file_list_selection(); }
        } else if (key == LV_KEY_ENTER || key == 13) {
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        }
        return;
    }
}

static void get_audio_player_callback(void **callback)
{
    *callback = audio_player_input_handler;
}

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_input_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#else /* !CONFIG_HAS_AUDIO_PLAYER */

void audio_player_create(void) {}
void audio_player_destroy(void) {}

static void audio_player_dummy_handler(InputEvent *event) { (void)event; }
static void get_audio_player_callback(void **callback) { *callback = audio_player_dummy_handler; }

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_dummy_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#endif /* CONFIG_HAS_AUDIO_PLAYER */
