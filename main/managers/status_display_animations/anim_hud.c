#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"
#include "managers/status_anim_utils.h"

#include <string.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "managers/sd_card_manager.h"
#include "managers/ap_manager.h"

typedef struct {
    int ram_used_pct;
    int ram_free_kb;
    int ram_total_kb;
    int cpu_used_pct;
    int sd_used_pct;
    bool sd_ok;
} HudStats;

static HudStats s_cached_stats;
static bool s_stats_valid;
static bool s_cached_ap_enabled;
static bool s_cached_server_running;
static int s_cached_sta_count;

static void hud_collect_stats(HudStats *out)
{
    if (!out) return;
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_bytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    out->ram_free_kb = (int)(free_bytes / 1024);
    out->ram_total_kb = (int)(total_bytes / 1024);
    int used_pct = 0;
    if (total_bytes > 0 && free_bytes <= total_bytes) {
        used_pct = (int)(((total_bytes - free_bytes) * 100) / total_bytes);
    }
    if (used_pct < 0) used_pct = 0;
    if (used_pct > 100) used_pct = 100;
    out->ram_used_pct = used_pct;

    size_t ifree_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t itotal_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int cpu_pct = 0;
    if (itotal_bytes > 0 && ifree_bytes <= itotal_bytes) {
        cpu_pct = (int)(((itotal_bytes - ifree_bytes) * 100) / itotal_bytes);
    }
    if (cpu_pct < 0) cpu_pct = 0;
    if (cpu_pct > 100) cpu_pct = 100;
    out->cpu_used_pct = cpu_pct;

    // SD card used space stats (use cached value from last mount)
    sd_card_cached_stats_t sd_stats;
    sd_card_get_cached_stats(&sd_stats);
    out->sd_ok = sd_stats.valid;
    out->sd_used_pct = sd_stats.valid ? sd_stats.used_pct : 0;
}

static int hud_get_webui_sta_count(bool *ap_enabled, bool *server_running)
{
    if (ap_enabled) {
        *ap_enabled = settings_get_ap_enabled(&G_Settings);
    }
    bool srv = false;
    ap_manager_get_status(&srv, NULL, NULL);
    if (server_running) {
        *server_running = srv;
    }
    if (!srv) return 0;

    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof(sta_list));
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

static void draw_bar(const StatusAnimGfx *gfx, int x, int y, int w, int h, int pct)
{
    status_anim_draw_progress_bar(gfx, x, y, w, h, pct);
}

void status_anim_hud_reset(void)
{
}

void status_anim_hud_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    if (!gfx || !gfx->draw_text || !gfx->draw_char_rot90_right) return;

    // heap walks and the WiFi STA list are mutex-protected and slow-moving:
    // collect stats at most every 20 frames (~3s) instead of every frame
    HudStats stats;
    if (!s_stats_valid || (frame % 20) == 0) {
        hud_collect_stats(&stats);
        s_cached_stats = stats;
        s_stats_valid = true;
    } else {
        stats = s_cached_stats;
    }

    char buf[32], buf1[32], buf2[32];
    
    // AP and WebUI status
    bool ap_enabled = false;
    bool server_running = false;
    int sta_count = 0;
    if (!s_stats_valid || (frame % 20) == 0) {
        sta_count = hud_get_webui_sta_count(&ap_enabled, &server_running);
    } else {
        ap_enabled = s_cached_ap_enabled;
        server_running = s_cached_server_running;
        sta_count = s_cached_sta_count;
    }
    s_cached_ap_enabled = ap_enabled;
    s_cached_server_running = server_running;
    s_cached_sta_count = sta_count;
    
    if (ap_enabled) {
        snprintf(buf1, sizeof(buf1), "AP:ON");
    } else {
        snprintf(buf1, sizeof(buf1), "AP:OFF");
    }
    if (!ap_enabled) {
        snprintf(buf2, sizeof(buf2), "WebUI:OFF");
    } else if (!server_running) {
        snprintf(buf2, sizeof(buf2), "WebUI:--");
    } else {
        snprintf(buf2, sizeof(buf2), "WebUI:%d", sta_count);
    }
    
    // Draw both on the same line, splitting the width
    // Left side starts at x=2, right side positioned to avoid vertical scrolling text
    gfx->draw_text(gfx->user, 2, 2, buf1);
    
    // Calculate right side position - vertical text starts at width-8 (x=120)
    // Position right side text to end before the vertical text area
    // Each character is ~6 pixels wide
    int right_text_len = (int)strlen(buf2);
    int right_text_width = right_text_len * 6;
    int vertical_text_start = gfx->width - 8; // Vertical text starts here
    int right_text_x = vertical_text_start - right_text_width - 4; // Leave 4px gap
    if (right_text_x < 2) right_text_x = 2; // Safety check
    gfx->draw_text(gfx->user, right_text_x, 2, buf2);

    // RAM text and bar
    snprintf(buf, sizeof(buf), "RAM %3d%%", stats.ram_used_pct);
    int ram_text_y = 18;
    gfx->draw_text(gfx->user, 2, ram_text_y, buf);
    // Bar graph next to RAM percentage (text is ~48 pixels wide, bar starts at x=50)
    // Center bar vertically with text, adjusted down by half character height
    int ram_bar_x = 50;
    int ram_bar_w = 60;
    int ram_bar_h = 10;
    int ram_text_center_y = ram_text_y + gfx->font_char_height / 2;
    int ram_bar_y = ram_text_center_y - ram_bar_h / 2 + 3; // Move down by ~half character height
    draw_bar(gfx, ram_bar_x, ram_bar_y, ram_bar_w, ram_bar_h, stats.ram_used_pct);

    // CPU text and bar
    snprintf(buf, sizeof(buf), "CPU %3d%%", stats.cpu_used_pct);
    int cpu_text_y = 34;
    gfx->draw_text(gfx->user, 2, cpu_text_y, buf);
    // Bar graph next to CPU percentage
    // Center bar vertically with text, adjusted down by half character height
    int cpu_bar_x = 50;
    int cpu_bar_w = 60;
    int cpu_bar_h = 10;
    int cpu_text_center_y = cpu_text_y + gfx->font_char_height / 2;
    int cpu_bar_y = cpu_text_center_y - cpu_bar_h / 2 + 3; // Move down by ~half character height
    draw_bar(gfx, cpu_bar_x, cpu_bar_y, cpu_bar_w, cpu_bar_h, stats.cpu_used_pct);

    // SD card used space text and bar
    if (stats.sd_ok) {
        snprintf(buf, sizeof(buf), "SD %3d%%", stats.sd_used_pct);
    } else {
        snprintf(buf, sizeof(buf), "SD   N/A");
    }
    int sd_text_y = 50;
    gfx->draw_text(gfx->user, 2, sd_text_y, buf);
    // Bar graph next to SD card used space (showing percentage of used space)
    // Center bar vertically with text, adjusted down by half character height
    int sd_bar_x = 50; // Aligned with RAM and CPU bars
    int sd_bar_w = 60; // Same width as RAM and CPU bars
    int sd_bar_h = 10;
    int sd_text_center_y = sd_text_y + gfx->font_char_height / 2;
    int sd_bar_y = sd_text_center_y - sd_bar_h / 2 + 3; // Move down by ~half character height
    if (stats.sd_ok) {
        draw_bar(gfx, sd_bar_x, sd_bar_y, sd_bar_w, sd_bar_h, stats.sd_used_pct);
    }

    const char *vert = "GhostESP: Revival";
    int len = (int)strlen(vert);
    int char_h = gfx->font_char_width * gfx->scale_y;
    int letter_gap = 1;
    int seq_gap = char_h / 2;
    int step = char_h + letter_gap;
    int total_h = len * step + seq_gap;
    if (total_h <= 0) return;
    int base_x = gfx->width - 8;
    int scroll = (frame * 2) % total_h;
    for (int repeat = 0; repeat < 2; ++repeat) {
        int start = -scroll + repeat * total_h;
        for (int i = 0; i < len; ++i) {
            int y = start + i * step;
            if (y > gfx->height - 1) break;
            if (y + char_h < 0) continue;
            gfx->draw_char_rot90_right(gfx->user, base_x, y, vert[i]);
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY
