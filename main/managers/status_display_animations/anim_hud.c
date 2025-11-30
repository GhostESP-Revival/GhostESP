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
    bool sd_ok;
} HudStats;

static bool hud_get_wifi_ssid(char *out, size_t len)
{
    if (!out || len == 0) return false;
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return false;
    size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
    if (slen == 0) return false;
    if (slen >= len) slen = len - 1;
    memcpy(out, ap.ssid, slen);
    out[slen] = '\0';
    return true;
}

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

    out->sd_ok = sd_card_manager.is_initialized;
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

    HudStats stats;
    hud_collect_stats(&stats);

    char buf[32];
    uint32_t hud_phase = (uint32_t)frame;
    uint32_t top_mode = (hud_phase / 8U) % 3U;

    if (top_mode == 0) {
        char ssid[20];
        if (hud_get_wifi_ssid(ssid, sizeof(ssid))) {
            snprintf(buf, sizeof(buf), "AP  %.16s", ssid);
        } else {
            snprintf(buf, sizeof(buf), "AP Not connected");
        }
    } else if (top_mode == 1) {
        if (!stats.sd_ok) {
            snprintf(buf, sizeof(buf), "SD FAIL");
        } else {
            snprintf(buf, sizeof(buf), "SD OK");
        }
    } else {
        bool ap_enabled = false;
        bool server_running = false;
        int sta_count = hud_get_webui_sta_count(&ap_enabled, &server_running);
        if (!ap_enabled) {
            snprintf(buf, sizeof(buf), "WebUI: OFF");
        } else if (!server_running) {
            snprintf(buf, sizeof(buf), "WebUI: STOP");
        } else {
            snprintf(buf, sizeof(buf), "WebUI: %d STA", sta_count);
        }
    }
    gfx->draw_text(gfx->user, 2, 2, buf);

    snprintf(buf, sizeof(buf), "RAM %3d%%", stats.ram_used_pct);
    gfx->draw_text(gfx->user, 2, 18, buf);

    snprintf(buf, sizeof(buf), "CPU %3d%%", stats.cpu_used_pct);
    gfx->draw_text(gfx->user, 2, 34, buf);

    draw_bar(gfx, 8, gfx->height - 8, gfx->width - 16, 5, stats.cpu_used_pct);

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
