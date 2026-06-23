#include "managers/views/audio_player_screen.h"
#include "managers/audio_stream_manager.h"
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
#ifdef CONFIG_HAS_TLV320DAC_I2C
#include "io_manager.h"
#include "tlv320dac3100.h"
#endif
#include "freertos/task.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER

static const char *TAG = "AudioPlayer";

/* UI element handles */
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_list_hint_label = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_track = NULL;
static lv_obj_t *s_progress_fill = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_meta_label = NULL;
static lv_obj_t *s_volume_label = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_pause_btn = NULL;
static lv_obj_t *s_prev_btn = NULL;
static lv_obj_t *s_next_btn = NULL;

/* State */
static int s_selected_index = 0;
static int s_visible_count = 0;
static uint8_t s_volume_percent = 85;
static lv_timer_t *s_update_timer = NULL;

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
static void on_play_clicked(lv_event_t *e);
static void on_pause_clicked(lv_event_t *e);
static void on_prev_clicked(lv_event_t *e);
static void on_next_clicked(lv_event_t *e);
static void update_timer_cb(lv_timer_t *timer);
static void return_to_apps(void);
static void adjust_volume(int delta);
static bool play_track_with_toast(int index);
static bool change_track_with_toast(bool next);

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
    uint32_t played_ms = audio_stream_manager_get_playback_ms();
    if (played_ms > 0) return played_ms;

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

static void audio_player_update_status(void)
{
    if (!s_status_label) return;

    audio_stream_state_t state = audio_stream_manager_get_state();
    int current = audio_stream_manager_get_current_index();
    int total = audio_stream_manager_get_file_count();

    if (current != s_last_rendered_playing_index || state != s_last_rendered_state) {
        s_last_rendered_playing_index = current;
        s_last_rendered_state = state;
        update_file_list_selection();
    }

    /* Sync play/pause buttons with current playback state */
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

    char buf[96];
    if (total > 0 && current >= 0 && current < total) {
        const char *fname = audio_stream_manager_get_filename(current);
        snprintf(buf, sizeof(buf), "%d/%d  %s", current + 1, total, fname ? fname : "");
    } else {
        snprintf(buf, sizeof(buf), "%s", total > 0 ? "Select a track" : "No MP3 files found");
    }

    lv_label_set_text(s_status_label, buf);

    /* Update progress bar and time labels */
    size_t pos = audio_stream_manager_get_position();
    size_t total_size = audio_stream_manager_get_total_size();
    uint32_t duration_ms = audio_stream_manager_get_duration_ms();
    uint32_t played_ms = get_estimated_playback_ms(pos, total_size, duration_ms);
    if (duration_ms > 0 && played_ms > duration_ms) played_ms = duration_ms;
    if (s_progress_fill) {
        if (duration_ms > 0) {
            int32_t pct = (int32_t)(((uint64_t)played_ms * 100) / duration_ms);
            if (pct > 100) pct = 100;
            lv_coord_t track_w = s_progress_track ? lv_obj_get_width(s_progress_track) : (LV_HOR_RES - 16);
            lv_obj_set_size(s_progress_fill, (track_w * pct) / 100, 7);
        } else if (total_size > 0) {
            int32_t pct = (int32_t)((pos * 100) / total_size);
            if (pct > 100) pct = 100;
            lv_coord_t track_w = s_progress_track ? lv_obj_get_width(s_progress_track) : (LV_HOR_RES - 16);
            lv_obj_set_size(s_progress_fill, (track_w * pct) / 100, 7);
        } else {
            lv_obj_set_size(s_progress_fill, 0, 7);
        }
    }

    if (s_time_label) {
        char tbuf[32];
        char tbuf2[32];
        format_time(tbuf, sizeof(tbuf), played_ms / 1000);
        format_time(tbuf2, sizeof(tbuf2), duration_ms / 1000);
        char tline[80];
        snprintf(tline, sizeof(tline), "%s / %s", tbuf, tbuf2);
        lv_label_set_text(s_time_label, tline);
    }

    if (s_meta_label) {
        uint16_t bitrate = audio_stream_manager_get_bitrate();
        char mbuf[48];
        if (bitrate > 0) {
            snprintf(mbuf, sizeof(mbuf), "%s  %uk", state_str, (unsigned)bitrate);
        } else {
            snprintf(mbuf, sizeof(mbuf), "%s", state_str);
        }
        lv_label_set_text(s_meta_label, mbuf);
    }

    if (s_volume_label) {
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "Vol %u%%", (unsigned)s_volume_percent);
        lv_label_set_text(s_volume_label, vbuf);
    }
}

static void create_file_list(void)
{
    int file_count = audio_stream_manager_get_file_count();
    s_visible_count = file_count;
    s_last_rendered_playing_index = audio_stream_manager_get_current_index();
    s_last_rendered_state = audio_stream_manager_get_state();

    if (!s_file_list) return;

    /* Clear existing list */
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
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, LV_VER_RES <= 160 ? 30 : 34);
        style_track_row(btn, selected, playing);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        /* Pass index as event user data */
        lv_obj_add_event_cb(btn, on_play_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

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

    /* Scroll selected into view */
    if (s_selected_index >= 0 && s_selected_index < (int)child_cnt) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, s_selected_index);
        if (btn) lv_obj_scroll_to_view(btn, LV_ANIM_ON);
    }
    update_list_hint();
}

static void on_play_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) {
        if (audio_stream_manager_get_state() == AUDIO_STREAM_STATE_PAUSED) {
            audio_stream_manager_resume();
            audio_player_update_status();
            return;
        }
        /* Controls play button: use current selection */
        idx = s_selected_index;
    }
    if (idx < 0 || idx >= s_visible_count) return;

    s_selected_index = idx;
    update_file_list_selection();

    if (!play_track_with_toast(idx)) {
        audio_player_update_status();
        return;
    }
    audio_player_update_status();

    /* Show play/pause buttons */
    if (s_play_btn) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_pause_btn) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
}

static void on_pause_clicked(lv_event_t *e)
{
    (void)e;
    audio_stream_state_t state = audio_stream_manager_get_state();
    if (state == AUDIO_STREAM_STATE_PLAYING) {
        audio_stream_manager_pause();
        if (s_play_btn) lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_pause_btn) lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (state == AUDIO_STREAM_STATE_PAUSED) {
        audio_stream_manager_resume();
        if (s_play_btn) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_pause_btn) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }
    audio_player_update_status();
}

static void on_prev_clicked(lv_event_t *e)
{
    (void)e;
    change_track_with_toast(false);
    s_selected_index = audio_stream_manager_get_current_index();
    update_file_list_selection();
    audio_player_update_status();
}

static void on_next_clicked(lv_event_t *e)
{
    (void)e;
    change_track_with_toast(true);
    s_selected_index = audio_stream_manager_get_current_index();
    update_file_list_selection();
    audio_player_update_status();
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_player_update_status();
}

static void return_to_apps(void)
{
    display_manager_switch_view(&apps_menu_view);
}

static void adjust_volume(int delta)
{
#ifdef CONFIG_HAS_TLV320DAC_I2C
    int volume = (int)s_volume_percent + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == (int)s_volume_percent) return;

    esp_err_t ret = tlv320dac3100_set_volume((uint8_t)volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(ret));
        return;
    }
    s_volume_percent = (uint8_t)volume;
    ESP_LOGI(TAG, "Audio volume: %u%%", (unsigned)s_volume_percent);
    audio_player_update_status();
#else
    (void)delta;
#endif
}

static bool play_track_with_toast(int index)
{
    esp_err_t ret = audio_stream_manager_play(index);
    if (ret == ESP_OK) {
        return true;
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 bitrate too high (must be under 200kbps)", TOAST_WARN, 2400);
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
        toast_show_duration("MP3 bitrate too high (must be under 200kbps)", TOAST_WARN, 2400);
    } else {
        toast_show_duration("Could not play MP3", TOAST_WARN, 1600);
    }
    ESP_LOGW(TAG, "Audio %s failed: %s", next ? "next" : "previous", esp_err_to_name(ret));
    return false;
}

void audio_player_create(void)
{
    refresh_theme_colors();

#ifdef CONFIG_HAS_TLV320DAC_I2C
    /* Force full reset of TLV320DAC3100 on each app entry */
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

    /* Initialize audio stream manager (scans SD card for MP3s) */
    esp_err_t ret = audio_stream_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio stream manager init failed: %s", esp_err_to_name(ret));
    }

    /* Create root container */
    s_root = gui_screen_create_root(NULL, "Audio Player", s_bg_color, LV_OPA_COVER);
    audio_player_view.root = s_root;
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int status_bar_h = GUI_STATUS_BAR_H;
    int screen_h = LV_VER_RES - status_bar_h;
    int controls_h = (screen_h > 200) ? 50 : 44;
    int now_playing_h = (screen_h > 200) ? 64 : 56;
    int list_hint_h = 16;
    int list_h = screen_h - controls_h - now_playing_h - list_hint_h - 8;

    display_manager_add_status_bar("Audio Player");

    /* File list container */
    s_file_list = lv_obj_create(s_root);
    lv_obj_set_size(s_file_list, LV_HOR_RES - 8, list_h);
    lv_obj_align(s_file_list, LV_ALIGN_TOP_MID, 0, status_bar_h + 4);
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

    s_list_hint_label = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_list_hint_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_list_hint_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_list_hint_label, LV_HOR_RES - 12);
    lv_obj_set_style_text_align(s_list_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_list_hint_label, LV_ALIGN_TOP_MID, 0, status_bar_h + 4 + list_h + 1);
    lv_label_set_text(s_list_hint_label, "0 tracks");
    update_list_hint();

    /* Now playing panel */
    lv_obj_t *now_playing = lv_obj_create(s_root);
    lv_obj_set_size(now_playing, LV_HOR_RES - 8, now_playing_h);
    lv_obj_align(now_playing, LV_ALIGN_BOTTOM_MID, 0, -controls_h - 2);
    lv_obj_set_style_bg_color(now_playing, s_surface_color, 0);
    lv_obj_set_style_bg_opa(now_playing, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(now_playing, s_surface_alt_color, 0);
    lv_obj_set_style_border_width(now_playing, 1, 0);
    lv_obj_set_style_radius(now_playing, 12, 0);
    lv_obj_set_style_pad_all(now_playing, 7, 0);
    lv_obj_clear_flag(now_playing, LV_OBJ_FLAG_SCROLLABLE);

    /* Controls container */
    lv_obj_t *controls = lv_obj_create(s_root);
    lv_obj_set_size(controls, LV_HOR_RES - 8, controls_h);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 4, 0);
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    /* Previous button */
    s_prev_btn = lv_btn_create(controls);
    lv_obj_set_size(s_prev_btn, 48, 34);
    lv_obj_align(s_prev_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(s_prev_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_prev_btn, 10, 0);
    lv_obj_set_style_border_width(s_prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_prev_btn, 0, 0);
    lv_obj_add_event_cb(s_prev_btn, on_prev_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_lbl = create_label(s_prev_btn, LV_SYMBOL_LEFT, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(prev_lbl);

    /* Play button */
    s_play_btn = lv_btn_create(controls);
    lv_obj_set_size(s_play_btn, 52, 34);
    lv_obj_align(s_play_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_play_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_play_btn, 10, 0);
    lv_obj_set_style_border_width(s_play_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_play_btn, 0, 0);
    lv_obj_add_event_cb(s_play_btn, on_play_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *play_lbl = create_label(s_play_btn, LV_SYMBOL_PLAY, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(play_lbl);

    /* Pause button (initially hidden) */
    s_pause_btn = lv_btn_create(controls);
    lv_obj_set_size(s_pause_btn, 52, 34);
    lv_obj_align(s_pause_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pause_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_pause_btn, 10, 0);
    lv_obj_set_style_border_width(s_pause_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_pause_btn, 0, 0);
    lv_obj_add_event_cb(s_pause_btn, on_pause_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *pause_lbl = create_label(s_pause_btn, "II", accessibility_get_font_body(), s_accent_color);
    lv_obj_center(pause_lbl);

    /* Next button */
    s_next_btn = lv_btn_create(controls);
    lv_obj_set_size(s_next_btn, 48, 34);
    lv_obj_align(s_next_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_next_btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(s_next_btn, 10, 0);
    lv_obj_set_style_border_width(s_next_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_next_btn, 0, 0);
    lv_obj_add_event_cb(s_next_btn, on_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_lbl = create_label(s_next_btn, LV_SYMBOL_RIGHT, accessibility_get_font_body(), s_accent_color);
    lv_obj_center(next_lbl);

    /* Status label */
    s_status_label = lv_label_create(now_playing);
    lv_obj_set_style_text_color(s_status_label, s_text_color, 0);
    lv_obj_set_style_text_font(s_status_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_status_label, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(s_status_label, "Ready");

    /* Progress bar + time/volume row (custom to avoid LVGL bar dependency) */
    s_progress_track = lv_obj_create(now_playing);
    lv_obj_set_size(s_progress_track, LV_HOR_RES - 24, 7);
    lv_obj_align(s_progress_track, LV_ALIGN_TOP_MID, 0, 21);
    lv_obj_set_style_bg_color(s_progress_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_progress_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_track, 4, 0);
    lv_obj_set_style_border_width(s_progress_track, 0, 0);
    lv_obj_set_style_pad_all(s_progress_track, 0, 0);
    lv_obj_clear_flag(s_progress_track, LV_OBJ_FLAG_SCROLLABLE);

    s_progress_fill = lv_obj_create(s_progress_track);
    lv_obj_set_size(s_progress_fill, 0, 7);
    lv_obj_align(s_progress_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_fill, 4, 0);
    lv_obj_set_style_border_width(s_progress_fill, 0, 0);
    lv_obj_set_style_pad_all(s_progress_fill, 0, 0);
    lv_obj_clear_flag(s_progress_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(now_playing);
    lv_obj_set_style_text_color(s_time_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_time_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_time_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(s_time_label, "0:00 / 0:00");

    s_meta_label = lv_label_create(now_playing);
    lv_obj_set_style_text_color(s_meta_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_meta_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_meta_label, LV_HOR_RES / 2);
    lv_obj_set_style_text_align(s_meta_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_meta_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(s_meta_label, "Stopped");

    s_volume_label = lv_label_create(now_playing);
    lv_obj_set_style_text_color(s_volume_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_volume_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(s_volume_label, "Vol 85%");

    /* Update timer */
    s_update_timer = lv_timer_create(update_timer_cb, 200, NULL);

    /* Check SD card and show toast if needed */
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

    s_file_list = NULL;
    s_list_hint_label = NULL;
    s_status_label = NULL;
    s_progress_track = NULL;
    s_progress_fill = NULL;
    s_time_label = NULL;
    s_meta_label = NULL;
    s_volume_label = NULL;
    s_play_btn = NULL;
    s_pause_btn = NULL;
    s_prev_btn = NULL;
    s_next_btn = NULL;
    s_selected_index = 0;
    s_visible_count = 0;
    touch_drag_reset(&s_touch_drag);
}

static void audio_player_input_handler(InputEvent *event)
{
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_drag.started) {
                touch_drag_begin(&s_touch_drag, d);
            } else if (s_file_list && lv_obj_is_valid(s_file_list)) {
                // Move event - apply live drag or remember target for release
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
            if (!s_touch_drag.started) return;
            int dx = (int)d->point.x - s_touch_drag.start_x;
            int dy = (int)d->point.y - s_touch_drag.start_y;

            // Let the shared touch_drag helper handle release-on-release
            // (it applies a single scroll when the live setting is off) and
            // tell us if a drag was in progress so we can skip tap handling.
            bool was_dragged = touch_drag_release(&s_touch_drag, d);
            if (was_dragged) return;

            /* Check control button hit areas first (bottom bar) */
            int status_bar_h = GUI_STATUS_BAR_H;
            int screen_h = LV_VER_RES - status_bar_h;
            int controls_h = (screen_h > 200) ? 50 : 44;
            int controls_bottom = LV_VER_RES;
            int controls_top = controls_bottom - controls_h;
            bool in_controls = (d->point.y >= controls_top && d->point.y <= controls_bottom);

            if (in_controls && abs(dx) < 15 && abs(dy) < 15) {
                /* Tap on control buttons */
                int cx = d->point.x;
                int btn_w = 48;
                int btn_spacing = 8;
                int center_x = LV_HOR_RES / 2;

                /* Prev button: left side */
                int prev_left = btn_spacing;
                int prev_right = prev_left + btn_w;
                if (cx >= prev_left && cx <= prev_right) {
                    change_track_with_toast(false);
                    s_selected_index = audio_stream_manager_get_current_index();
                    update_file_list_selection();
                    audio_player_update_status();
                    return;
                }

                /* Play/Pause button: center */
                int play_left = center_x - btn_w / 2;
                int play_right = center_x + btn_w / 2;
                if (cx >= play_left && cx <= play_right) {
                    audio_stream_state_t st = audio_stream_manager_get_state();
                    if (st == AUDIO_STREAM_STATE_PLAYING) {
                        audio_stream_manager_pause();
                    } else {
                        if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                            play_track_with_toast(s_selected_index);
                        }
                    }
                    audio_player_update_status();
                    return;
                }

                /* Next button: right side */
                int next_right = LV_HOR_RES - btn_spacing;
                int next_left = next_right - btn_w;
                if (cx >= next_left && cx <= next_right) {
                    change_track_with_toast(true);
                    s_selected_index = audio_stream_manager_get_current_index();
                    update_file_list_selection();
                    audio_player_update_status();
                    return;
                }
                return;
            }

            if (abs(dx) < 10 && abs(dy) < 10) {
                /* Tap on list item */
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
                        play_track_with_toast((int)i);
                        audio_player_update_status();
                        break;
                    }
                }
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        /* Map joystick: 0=left, 1=select, 2=up, 3=right, 4=down */
        if (btn == 0) { /* Left -> exit */
            return_to_apps();
        } else if (btn == 2) { /* Up */
            if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (btn == 4) { /* Down */
            if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (btn == 1) { /* Select */
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                play_track_with_toast(s_selected_index);
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
            /* Encoder button press = play selected */
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                play_track_with_toast(s_selected_index);
                audio_player_update_status();
            }
        } else {
            adjust_volume(event->data.encoder.direction > 0 ? 5 : -5);
        }
        return;
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        return_to_apps();
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == '`') {
            return_to_apps();
        } else if (key == LV_KEY_UP || key == 'k') {
            if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (key == LV_KEY_DOWN || key == 'j') {
            if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (key == LV_KEY_ENTER || key == 13) {
            if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                play_track_with_toast(s_selected_index);
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
