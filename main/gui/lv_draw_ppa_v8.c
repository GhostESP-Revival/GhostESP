#include "gui/lv_draw_ppa_v8.h"

#if CONFIG_GHOSTESP_P4_PPA_RENDER

#include <stdint.h>
#include "driver/ppa.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl/src/draw/sw/lv_draw_sw.h"
#include "lvgl/src/draw/sw/lv_draw_sw_blend.h"
#include "vendor/drivers/crowpanel_p4_display.h"

#define PPA_MIN_PIXELS 256
#define PPA_STATS_POLL_INTERVAL 256

/* PPA output buffers must follow the configured cache-line alignment.  Do not
 * tie this to the PPA burst length: a 128-byte burst does not make a 128-byte
 * aligned buffer necessary.  ESP32-P4 builds can select 64- or 128-byte L2
 * cache lines, so derive the requirement from sdkconfig just like the driver. */
#if CONFIG_CACHE_L1_CACHE_LINE_SIZE > CONFIG_CACHE_L2_CACHE_LINE_SIZE
#define PPA_OUTPUT_ALIGNMENT CONFIG_CACHE_L1_CACHE_LINE_SIZE
#else
#define PPA_OUTPUT_ALIGNMENT CONFIG_CACHE_L2_CACHE_LINE_SIZE
#endif

typedef struct {
    lv_draw_sw_ctx_t sw;
    lv_disp_drv_t *drv;
    ppa_client_handle_t fill;
    ppa_client_handle_t blend;
    uint32_t fill_count;
    uint32_t blend_count;
    uint32_t fallback_count;
    uint32_t error_count;
    uint32_t alignment_fallback_count;
    uint32_t size_fallback_count;
    uint32_t mode_fallback_count;
    uint32_t mask_fallback_count;
    int64_t last_stats_log_us;
    uint16_t stats_poll_count;
    bool alignment_logged;
    void (*sw_init_buf)(lv_draw_ctx_t *draw_ctx);
} lv_draw_ppa_v8_ctx_t;

static const char *TAG = "lv_draw_ppa";

static void ppa_log_stats(lv_draw_ppa_v8_ctx_t *ctx, const lv_draw_ctx_t *draw_ctx)
{
    if (draw_ctx && draw_ctx->buf && !ctx->alignment_logged) {
        uintptr_t address = (uintptr_t)draw_ctx->buf;
        ESP_LOGI(TAG, "P4 framebuffer=%p, %u-byte aligned=%s",
                 draw_ctx->buf, (unsigned)PPA_OUTPUT_ALIGNMENT,
                 (address & (PPA_OUTPUT_ALIGNMENT - 1)) == 0 ? "yes" : "no");
        ctx->alignment_logged = true;
    }

    /* This function is reached for every LVGL blend, which can be hundreds of
     * thousands of calls per minute. Reading the high-resolution timer on each
     * operation adds avoidable CPU overhead just to rate-limit diagnostics. */
    if (ctx->last_stats_log_us != 0 &&
        ++ctx->stats_poll_count < PPA_STATS_POLL_INTERVAL) {
        return;
    }
    ctx->stats_poll_count = 0;

    int64_t now = esp_timer_get_time();
    if (ctx->last_stats_log_us == 0 || now - ctx->last_stats_log_us >= 10000000) {
        ctx->last_stats_log_us = now;
        ESP_LOGI(TAG, "PPA stats: fill=%lu blend=%lu fallback=%lu errors=%lu "
                      "(align=%lu size=%lu mode=%lu mask=%lu)",
                 (unsigned long)ctx->fill_count,
                 (unsigned long)ctx->blend_count,
                 (unsigned long)ctx->fallback_count,
                 (unsigned long)ctx->error_count,
                 (unsigned long)ctx->alignment_fallback_count,
                 (unsigned long)ctx->size_fallback_count,
                 (unsigned long)ctx->mode_fallback_count,
                 (unsigned long)ctx->mask_fallback_count);
    }
}

static void ppa_init_buf(lv_draw_ctx_t *draw_ctx)
{
    lv_draw_ppa_v8_ctx_t *ctx = (lv_draw_ppa_v8_ctx_t *)draw_ctx;
    if (ctx->sw_init_buf) ctx->sw_init_buf(draw_ctx);
    if (draw_ctx->buf_area) {
        crowpanel_p4_display_mark_dirty_rows(draw_ctx->buf_area->y1,
                                              draw_ctx->buf_area->y2);
    }
}

static bool ppa_full_framebuffer(const lv_draw_ppa_v8_ctx_t *ctx, const lv_draw_ctx_t *draw_ctx)
{
    if (!ctx || !ctx->drv || !draw_ctx->buf || !draw_ctx->buf_area) return false;
    if (((uintptr_t)draw_ctx->buf & (PPA_OUTPUT_ALIGNMENT - 1)) != 0) return false;
    return draw_ctx->buf_area->x1 == 0 && draw_ctx->buf_area->y1 == 0 &&
           lv_area_get_width(draw_ctx->buf_area) == ctx->drv->hor_res &&
           lv_area_get_height(draw_ctx->buf_area) == ctx->drv->ver_res;
}

static void ppa_fallback(lv_draw_ppa_v8_ctx_t *ctx, lv_draw_ctx_t *draw_ctx,
                         const lv_draw_sw_blend_dsc_t *dsc)
{
    ctx->fallback_count++;
    lv_draw_sw_blend_basic(draw_ctx, dsc);
}

static esp_err_t ppa_fill_area(lv_draw_ppa_v8_ctx_t *ctx, lv_draw_ctx_t *draw_ctx,
                               const lv_area_t *area, lv_color_t color)
{
    const uint32_t width = ctx->drv->hor_res;
    const uint32_t height = ctx->drv->ver_res;
    const size_t buffer_size = (size_t)width * height * sizeof(lv_color_t);
    ppa_fill_oper_config_t config = {
        .out = {
            .buffer = draw_ctx->buf,
            .buffer_size = buffer_size,
            .pic_w = width,
            .pic_h = height,
            .block_offset_x = area->x1,
            .block_offset_y = area->y1,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = lv_area_get_width(area),
        .fill_block_h = lv_area_get_height(area),
        .fill_argb_color.val = lv_color_to32(color),
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(ctx->fill, &config);
}

static esp_err_t ppa_blend_area(lv_draw_ppa_v8_ctx_t *ctx, lv_draw_ctx_t *draw_ctx,
                                const lv_draw_sw_blend_dsc_t *dsc, const lv_area_t *area)
{
    const uint32_t width = ctx->drv->hor_res;
    const uint32_t height = ctx->drv->ver_res;
    const uint32_t block_w = lv_area_get_width(area);
    const uint32_t block_h = lv_area_get_height(area);
    const uint32_t src_w = lv_area_get_width(dsc->blend_area);
    const uint32_t src_h = lv_area_get_height(dsc->blend_area);
    const uint32_t src_x = area->x1 - dsc->blend_area->x1;
    const uint32_t src_y = area->y1 - dsc->blend_area->y1;
    const size_t buffer_size = (size_t)width * height * sizeof(lv_color_t);
    ppa_blend_oper_config_t config = {
        .in_bg = {
            .buffer = draw_ctx->buf,
            .pic_w = width,
            .pic_h = height,
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = area->x1,
            .block_offset_y = area->y1,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = dsc->src_buf,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = src_x,
            .block_offset_y = src_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = draw_ctx->buf,
            .buffer_size = buffer_size,
            .pic_w = width,
            .pic_h = height,
            .block_offset_x = area->x1,
            .block_offset_y = area->y1,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 255 - dsc->opa,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = dsc->opa,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_blend(ctx->blend, &config);
}

static void ppa_blend(lv_draw_ctx_t *draw_ctx, const lv_draw_sw_blend_dsc_t *dsc)
{
    lv_draw_ppa_v8_ctx_t *ctx = (lv_draw_ppa_v8_ctx_t *)draw_ctx;
    lv_area_t area;
    if (!_lv_area_intersect(&area, dsc->blend_area, draw_ctx->clip_area)) return;

    /* In direct mode LVGL passes the full framebuffer as the flush area.
     * Track the actual draw area here instead, so presentation can remain
     * row-limited when only part of the screen changed. */
    crowpanel_p4_display_mark_dirty_rows(area.y1, area.y2);
    ppa_log_stats(ctx, draw_ctx);

    bool aligned = draw_ctx->buf &&
                   (((uintptr_t)draw_ctx->buf & (PPA_OUTPUT_ALIGNMENT - 1)) == 0);
    bool large_enough = lv_area_get_size(&area) >= PPA_MIN_PIXELS;
    bool normal_mode = dsc->blend_mode == LV_BLEND_MODE_NORMAL;
    bool supported_mask = !(dsc->mask_buf && dsc->mask_res != LV_DRAW_MASK_RES_FULL_COVER);
    if (!aligned) ctx->alignment_fallback_count++;
    if (!large_enough) ctx->size_fallback_count++;
    if (!normal_mode) ctx->mode_fallback_count++;
    if (!supported_mask) ctx->mask_fallback_count++;
    if (!ppa_full_framebuffer(ctx, draw_ctx) || !large_enough || !normal_mode || !supported_mask) {
        ppa_fallback(ctx, draw_ctx, dsc);
        return;
    }

    esp_err_t err;
    if (dsc->src_buf && ctx->blend) {
        err = ppa_blend_area(ctx, draw_ctx, dsc, &area);
        if (err == ESP_OK) {
            ctx->blend_count++;
            return;
        }
    } else if (!dsc->src_buf && dsc->opa >= LV_OPA_MAX && ctx->fill) {
        err = ppa_fill_area(ctx, draw_ctx, &area, dsc->color);
        if (err == ESP_OK) {
            ctx->fill_count++;
            return;
        }
    } else {
        ppa_fallback(ctx, draw_ctx, dsc);
        return;
    }

    ctx->error_count++;
    if (ctx->error_count <= 3) {
        ESP_LOGW(TAG, "PPA transaction failed: %s; using software", esp_err_to_name(err));
    }
    ppa_fallback(ctx, draw_ctx, dsc);
}

static void ppa_ctx_init(lv_disp_drv_t *drv, lv_draw_ctx_t *draw_ctx)
{
    lv_draw_ppa_v8_ctx_t *ctx = (lv_draw_ppa_v8_ctx_t *)draw_ctx;

    /* LVGL allocates draw_ctx with lv_mem_alloc(), which does not guarantee
     * zeroed memory. lv_draw_sw_init_ctx() only clears its lv_draw_sw_ctx_t
     * prefix, leaving our handles, counters, timestamp, and callback pointer
     * uninitialized. Besides producing garbage statistics, non-NULL garbage
     * handles could be passed to the PPA driver or deinitializer. Clear the
     * complete extended context before installing the software callbacks. */
    lv_memset_00(ctx, sizeof(*ctx));
    lv_draw_sw_init_ctx(drv, draw_ctx);
    ctx->drv = drv;
    ctx->sw_init_buf = draw_ctx->init_buf;
    draw_ctx->init_buf = ppa_init_buf;

    const ppa_client_config_t fill_config = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    const ppa_client_config_t blend_config = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t fill_err = ppa_register_client(&fill_config, &ctx->fill);
    esp_err_t blend_err = ppa_register_client(&blend_config, &ctx->blend);
    if (fill_err != ESP_OK || blend_err != ESP_OK) {
        ESP_LOGW(TAG, "PPA registration incomplete (fill=%s blend=%s)",
                 esp_err_to_name(fill_err), esp_err_to_name(blend_err));
    }
    ctx->sw.blend = ppa_blend;
    ESP_LOGI(TAG, "LVGL 8 PPA backend active (RGB565 fill + blend; stats every 10s)");
}

static void ppa_ctx_deinit(lv_disp_drv_t *drv, lv_draw_ctx_t *draw_ctx)
{
    lv_draw_ppa_v8_ctx_t *ctx = (lv_draw_ppa_v8_ctx_t *)draw_ctx;
    ESP_LOGI(TAG, "PPA stats: fill=%lu blend=%lu fallback=%lu errors=%lu",
             (unsigned long)ctx->fill_count, (unsigned long)ctx->blend_count,
             (unsigned long)ctx->fallback_count, (unsigned long)ctx->error_count);
    if (ctx->blend) ppa_unregister_client(ctx->blend);
    if (ctx->fill) ppa_unregister_client(ctx->fill);
    lv_draw_sw_deinit_ctx(drv, draw_ctx);
}

bool lv_draw_ppa_v8_install(lv_disp_drv_t *drv)
{
    if (!drv || LV_COLOR_DEPTH != 16 || sizeof(lv_color_t) != 2 ||
        drv->screen_transp || drv->set_px_cb) return false;
    drv->draw_ctx_size = sizeof(lv_draw_ppa_v8_ctx_t);
    drv->draw_ctx_init = ppa_ctx_init;
    drv->draw_ctx_deinit = ppa_ctx_deinit;
    return true;
}

#else

bool lv_draw_ppa_v8_install(lv_disp_drv_t *drv)
{
    (void)drv;
    return false;
}

#endif
