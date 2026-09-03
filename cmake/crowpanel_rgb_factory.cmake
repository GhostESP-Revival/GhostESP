# Board-local adaptation of IDF's RGB implementation. Do not edit the shared SDK.
# Reference: Elecrow's shared LovyanGFX Bus_RGB.cpp (not the lesson-04 variant).
# Factory descriptors carry 4032 bytes and VSYNC resets GDMA only: the 17-pixel
# restart offset accounts for data retained in the LCD FIFO.
function(crowpanel_patch_rgb_source input output)
    file(READ "${input}" rgb_source)

    # Fail closed when IDF changes these internals, rather than silently applying
    # only part of a coupled alignment/restart fix.
    set(alignment_old "    gdma_get_alignment_constraints(rgb_panel->dma_chan, &rgb_panel->int_mem_align, &rgb_panel->ext_mem_align);")
    set(alignment_new "${alignment_old}\n    // CrowPanel factory uses 64-byte-aligned PSRAM DMA nodes (4032 bytes).\n    if (rgb_panel->flags.fb_in_psram && !rgb_panel->bb_size) {\n        rgb_panel->ext_mem_align = MAX(rgb_panel->ext_mem_align, 64);\n    }")
    # Keep SRAM RGB565 DMA payloads burst- and pixel-aligned too.
    string(APPEND alignment_new "\n    if (rgb_panel->bb_size) {\n        rgb_panel->int_mem_align = MAX(rgb_panel->int_mem_align, 64);\n    }")
    set(bounce_length_old ".length = MIN(LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE, rgb_panel->bb_size) - restart_skip_bytes,")
    set(bounce_length_new ".length = MIN(LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE & ~(buffer_alignment - 1), rgb_panel->bb_size) - restart_skip_bytes,\n            .flags.bypass_buffer_align_check = true, // Intentional 17-pixel FIFO offset")
    set(length_old ".length = MIN(LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE, rgb_panel->fb_size) - restart_skip_bytes,")
    set(length_new ".length = MIN(LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE & ~(buffer_alignment - 1), rgb_panel->fb_size) - restart_skip_bytes,")
    set(restart_old "    lcd_ll_fifo_reset(panel->hal.dev);\n    gdma_reset(panel->dma_chan);")
    set(restart_new "    // Preserve the LCD FIFO: dma_restart_link skips its 17 retained pixels.\n    // Resetting this FIFO here discards pixels assumed present by that link.\n    gdma_reset(panel->dma_chan);")
    set(helper_old "#include \"hal/lcd_hal.h\"")
    set(helper_new "${helper_old}\n#include \"crowpanel_rgb_factory_helpers.h\"")
    set(clock_old "lcd_hal_cal_pclk_freq(")
    set(clock_new "crowpanel_cal_pclk_freq(")
    set(eof_old ".eof_till_data_popped = false,")
    set(eof_new ".eof_till_data_popped = true, // Factory out_eof_mode=1")
    set(priority_old "    gdma_connect(rgb_panel->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));")
    set(priority_new "${priority_old}\n#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD\n    // RGB consumes data continuously. Give its TX channel precedence over\n    // opportunistic DMA traffic so Wi-Fi/storage bursts cannot empty the LCD FIFO.\n    ESP_RETURN_ON_ERROR(gdma_set_priority(rgb_panel->dma_chan, GDMA_LL_CHANNEL_MAX_PRIORITY),\n                        TAG, \"set CrowPanel DMA priority failed\");\n#endif")
    set(fields_old "    gdma_channel_handle_t dma_chan; // DMA channel handle")
    set(fields_new "${fields_old}\n    int crowpanel_dma_channel; // Cached hardware channel for immediate VSYNC restart\n    gdma_link_list_handle_t crowpanel_restart_links[ESP_RGB_LCD_PANEL_MAX_FB_NUM];\n    uintptr_t crowpanel_restart_addrs[ESP_RGB_LCD_PANEL_MAX_FB_NUM];\n    volatile uint8_t crowpanel_scanout_fb_index;")
    string(APPEND fields_new "\n    volatile uint32_t crowpanel_copies, crowpanel_cache_skips, crowpanel_copy_max_us;\n    volatile uint32_t crowpanel_restarts, crowpanel_short_eof, crowpanel_last_eof;\n    uint32_t crowpanel_eof_window; // ISR-owned count, independent of IDF buffer parity")
    set(cache_old "    ESP_GOTO_ON_ERROR(lcd_rgb_panel_init_trans_link(rgb_panel), err, TAG, \"init DMA link failed\");")
    set(cache_new "${cache_old}\n    ESP_GOTO_ON_ERROR(gdma_get_channel_id(rgb_panel->dma_chan, &rgb_panel->crowpanel_dma_channel), err, TAG, \"get DMA channel failed\");\n#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD\n    if (!rgb_panel->bb_size) {\n        for (size_t i = 0; i < rgb_panel->num_fbs; ++i) {\n            rgb_panel->crowpanel_restart_addrs[i] = gdma_link_get_head_addr(rgb_panel->crowpanel_restart_links[i]);\n        }\n    }\n    rgb_panel->crowpanel_scanout_fb_index = 0;\n    if (rgb_panel->flags.fb_in_psram) {\n        // Factory GDMA out_ext_mem_bk_size = 64B (GDMA_LL_EXT_MEM_BK_SIZE_64B).\n        // IDF's default is often 32B; the 64B burst matches the 4032-byte (63*64) nodes\n        // and reduces PSRAM read stalls that appear as horizontal tearing.\n        GDMA.channel[rgb_panel->crowpanel_dma_channel].out.conf1.out_ext_mem_bk_size = GDMA_LL_EXT_MEM_BK_SIZE_64B;\n    }\n#endif")
    set(fast_old "static IRAM_ATTR void lcd_rgb_panel_try_restart_transmission(esp_rgb_panel_t *panel)\n{")
    set(fast_new "${fast_old}\n#if CONFIG_LCD_RGB_RESTART_IN_VSYNC\n    if (!panel->bb_size && panel->num_fbs > 0) {\n        // draw_bitmap publishes cur_fb_index only after its dirty cache lines\n        // are in PSRAM. Take the same lock so VSYNC can never select a\n        // half-published framebuffer from the other core.\n        portENTER_CRITICAL_ISR(&panel->spinlock);\n        const uint8_t next_fb = panel->cur_fb_index;\n        portEXIT_CRITICAL_ISR(&panel->spinlock);\n        // Match Elecrow exactly: preserve the LCD FIFO and enter the selected\n        // framebuffer through its dedicated 17-pixel restart link. Resetting\n        // the FIFO on each LVGL swap disturbs the entire RGB stream.\n        crowpanel_restart_dma(panel->crowpanel_dma_channel, panel->crowpanel_restart_addrs[next_fb]);\n        panel->crowpanel_scanout_fb_index = next_fb;\n        return;\n    }\n#endif")
    set(isr_remove_old [=[#if RGB_LCD_NEEDS_SEPARATE_RESTART_LINK
        if (rgb_panel->flags.stream_mode) {
            // check whether to restart the transmission
            lcd_rgb_panel_try_restart_transmission(rgb_panel);
        }
#endif]=])
    set(isr_remove_new "")
    set(isr_add_old "    // VSYNC event happened\n    if (intr_status & LCD_LL_EVENT_VSYNC_END) {")
    set(isr_add_new "${isr_add_old}\n        // Restart first, like factory. Statistics and clock checks must not\n        // consume the restart window at the beginning of vertical blanking.\n${isr_remove_old}\n        if (rgb_panel->bb_size) {\n            rgb_panel->crowpanel_last_eof = rgb_panel->crowpanel_eof_window;\n            if (rgb_panel->crowpanel_eof_window < rgb_panel->expect_eof_count) ++rgb_panel->crowpanel_short_eof;\n            rgb_panel->crowpanel_eof_window = 0;\n        }")
    set(eof_irq_old "        .on_trans_eof = lcd_rgb_panel_eof_handler,")
    set(eof_irq_new "        // CrowPanel direct scanout hands buffers over in the LCD VSYNC IRQ.\n        // No per-frame GDMA EOF callback is needed when bounce mode is off.\n        .on_trans_eof = (rgb_panel->flags.stream_mode && !rgb_panel->bb_size) ? NULL : lcd_rgb_panel_eof_handler,")
    set(fb_link_mount_old [=[            ESP_RETURN_ON_ERROR(gdma_link_mount_buffers(rgb_panel->dma_fb_links[i], 0, &mount_cfg, 1, NULL),
                                TAG, "mount DMA frame buffer failed");]=])
    set(fb_link_mount_new [=[            ESP_RETURN_ON_ERROR(gdma_link_mount_buffers(rgb_panel->dma_fb_links[i], 0, &mount_cfg, 1, NULL),
                                TAG, "mount DMA frame buffer failed");
#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD
            // Elecrow leaves the scanout chain circular from the moment it is
            // created. Do this once for every framebuffer before transmission;
            // rewriting a live link on every LVGL flush can tear the scanout.
            if (rgb_panel->flags.stream_mode) {
                gdma_link_concat(rgb_panel->dma_fb_links[i], -1,
                                 rgb_panel->dma_fb_links[i], 0);
            }
#endif]=])
    set(fb_link_concat_old "                gdma_link_concat(rgb_panel->dma_fb_links[i], -1, rgb_panel->dma_fb_links[rgb_panel->cur_fb_index], 0);")
    set(fb_link_concat_new [=[#if !CONFIG_CROWPANEL_ADVANCE_RGB_LCD
                gdma_link_concat(rgb_panel->dma_fb_links[i], -1, rgb_panel->dma_fb_links[rgb_panel->cur_fb_index], 0);
#else
                // Every CrowPanel framebuffer has its own immutable circular
                // chain. VSYNC chooses a restart chain; never edit a live link.
                (void)i;
#endif]=])
    set(extra_restart_old "        gdma_link_concat(rgb_panel->dma_restart_link, 0, rgb_panel->dma_fb_links[0], 1);")
    set(extra_restart_new [=[        gdma_link_concat(rgb_panel->dma_restart_link, 0, rgb_panel->dma_fb_links[0], 1);
#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD
        rgb_panel->crowpanel_restart_links[0] = rgb_panel->dma_restart_link;
        for (size_t i = 1; i < rgb_panel->num_fbs; ++i) {
            ESP_RETURN_ON_ERROR(gdma_new_link_list(&restart_link_cfg, &rgb_panel->crowpanel_restart_links[i]),
                                TAG, "create CrowPanel framebuffer restart link failed");
            restart_buffer_mount_cfg.buffer = rgb_panel->fbs[i] + restart_skip_bytes;
            ESP_RETURN_ON_ERROR(gdma_link_mount_buffers(rgb_panel->crowpanel_restart_links[i], 0,
                                &restart_buffer_mount_cfg, 1, NULL),
                                TAG, "mount CrowPanel framebuffer restart link failed");
            gdma_link_concat(rgb_panel->crowpanel_restart_links[i], 0,
                             rgb_panel->dma_fb_links[i], 1);
        }
#endif]=])
    set(extra_restart_free_old [=[#if RGB_LCD_NEEDS_SEPARATE_RESTART_LINK
    if (rgb_panel->dma_restart_link) {]=])
    set(extra_restart_free_new [=[#if RGB_LCD_NEEDS_SEPARATE_RESTART_LINK
#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD
    for (size_t i = 1; i < ESP_RGB_LCD_PANEL_MAX_FB_NUM; ++i) {
        if (rgb_panel->crowpanel_restart_links[i]) {
            gdma_del_link_list(rgb_panel->crowpanel_restart_links[i]);
        }
    }
#endif
    if (rgb_panel->dma_restart_link) {]=])
    set(fifo_threshold_old "    lcd_ll_set_blank_cycles(rgb_panel->hal.dev, 1, 1); // RGB panel always has a front and back blank (porch region)")
    set(fifo_threshold_new "${fifo_threshold_old}\n#if CONFIG_CROWPANEL_ADVANCE_RGB_LCD\n    // Factory writes lcd_misc.val = 0 before enabling blank/next-frame.\n    // IDF's default FIFO threshold is 11; the factory restart link assumes 0.\n    rgb_panel->hal.dev->lcd_misc.lcd_afifo_threshold_num = 0;\n#endif")
    set(bounce_isr_old "        memcpy(buffer, &panel->fbs[panel->bb_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);")
    set(bounce_isr_new [=[        // XIP keeps cache enabled for normal flash writes. A disabled cache
        // is still unsafe to read; count the fallback instead of hiding it.
        if (!cache_hal_is_cache_enabled(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA)) {
            ++panel->crowpanel_cache_skips;
            return need_yield;
        }
        const int64_t copy_start = esp_timer_get_time();
        memcpy(buffer, &panel->fbs[panel->bb_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
        const uint32_t copy_us = (uint32_t)(esp_timer_get_time() - copy_start);
        ++panel->crowpanel_copies;
        if (copy_us > panel->crowpanel_copy_max_us) panel->crowpanel_copy_max_us = copy_us;]=])
    set(eof_count_old "        // in bounce buffer mode, the DMA EOF means time to fill the finished bounce buffer")
    set(eof_count_new "${eof_count_old}\n        ++rgb_panel->crowpanel_eof_window;")
    set(restart_count_old "    if (!do_restart) {\n        return;\n    }\n\n    if (panel->bb_size) {")
    set(restart_count_new "${restart_count_old}\n        ++panel->crowpanel_restarts;")
    set(cache_sync_old [=[        if (!rgb_panel->bb_size && rgb_panel->flags.fb_behind_cache) {
            esp_cache_msync(flush_ptr, bytes_to_flush, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }]=])
    set(cache_sync_new [=[        if (!rgb_panel->bb_size && rgb_panel->flags.fb_behind_cache) {
            // The factory driver writes back only the dirty rectangle. The
            // stock IDF path flushes the complete 800x480 framebuffer here,
            // which needlessly competes with RGB scanout for PSRAM bandwidth.
            if (copy_bytes_per_line == bytes_per_line) {
                esp_cache_msync(flush_ptr, bytes_to_flush, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            } else {
                // Coalesce nearby rows like LovyanGFX's Cache_WriteBack_Addr;
                // dirty rows in a normal LVGL update are usually close enough
                // to share one writeback operation.
                uintptr_t cache_start = (uintptr_t)-1;
                uintptr_t cache_end = 0;
                for (int y = y_start; y < y_end; y++) {
                    uintptr_t row_start = (uintptr_t)(fb + (y * h_res + x_start) * bytes_per_pixel);
                    uintptr_t row_end = row_start + copy_bytes_per_line;
                    if (cache_start < cache_end &&
                        (row_start + copy_bytes_per_line + 4096 < cache_start ||
                         row_start > cache_end + 4096)) {
                        esp_cache_msync((void *)cache_start, cache_end - cache_start,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                        cache_start = row_start;
                        cache_end = row_end;
                    } else {
                        if (row_start < cache_start) cache_start = row_start;
                        if (row_end > cache_end) cache_end = row_end;
                    }
                }
                if (cache_start < cache_end) {
                    esp_cache_msync((void *)cache_start, cache_end - cache_start,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                }
            }
        }]=])
    set(direct_cache_old [=[        if (!rgb_panel->bb_size && rgb_panel->flags.fb_behind_cache) {
            uint8_t *cache_sync_start = rgb_panel->fbs[draw_buf_fb_index] + (y_start * h_res) * bytes_per_pixel;
            size_t cache_sync_size = (y_end - y_start) * bytes_per_line;
            esp_cache_msync(cache_sync_start, cache_sync_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }]=])
    set(direct_cache_new [=[        if (!rgb_panel->bb_size && rgb_panel->flags.fb_behind_cache) {
            // Direct-mode buffers are full framebuffers, but only the dirty
            // rectangle was rendered. Avoid writing unrelated PSRAM cache lines.
            if (x_start == 0 && x_end == h_res) {
                uint8_t *start = rgb_panel->fbs[draw_buf_fb_index] + y_start * bytes_per_line;
                esp_cache_msync(start, (y_end - y_start) * bytes_per_line,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            } else {
                for (int y = y_start; y < y_end; ++y) {
                    uint8_t *start = rgb_panel->fbs[draw_buf_fb_index] +
                                     (y * h_res + x_start) * bytes_per_pixel;
                    esp_cache_msync(start, (x_end - x_start) * bytes_per_pixel,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                }
            }
        }
        // Publication is the ownership boundary. IDF normally changes this
        // index before cache writeback, allowing a concurrent VSYNC to expose
        // partially written PSRAM. Publish only after every dirty line above
        // is DMA-visible, under the same lock used by the VSYNC ISR.
        portENTER_CRITICAL(&rgb_panel->spinlock);
        rgb_panel->cur_fb_index = draw_buf_fb_index;
        portEXIT_CRITICAL(&rgb_panel->spinlock);]=])
    set(direct_select_old [=[        // the new frame buffer index is changed
        rgb_panel->cur_fb_index = draw_buf_fb_index;
        // when this function is called, the frame buffer already reflects the draw buffer changes]=])
    set(direct_select_new [=[        // The framebuffer index is published after cache synchronization below.
        // when this function is called, the frame buffer already reflects the draw buffer changes]=])
    string(REPLACE "\r\n" "\n" rgb_source "${rgb_source}")
    foreach(edit alignment length bounce_length restart helper clock eof priority fields cache fast isr_remove isr_add eof_irq fifo_threshold bounce_isr eof_count restart_count cache_sync direct_cache direct_select fb_link_mount fb_link_concat extra_restart extra_restart_free)
        string(FIND "${rgb_source}" "${${edit}_old}" match_offset)
        if(match_offset EQUAL -1)
            message(FATAL_ERROR "CrowPanel RGB: unsupported IDF source (${edit} anchor missing); review factory DMA adaptation")
        endif()
        string(REPLACE "${${edit}_old}" "${${edit}_new}" rgb_source "${rgb_source}")
    endforeach()
    # GDMA ext-mem block size is channel-specific and only known after gdma_get_channel_id;
    # verify the helper header now exposes GDMA_LL_EXT_MEM_BK_SIZE_64B.
    # Read-only diagnostic snapshots in task context. Never clear DMA status,
    # pop FIFOs, or change scanout while collecting these registers.
    string(APPEND rgb_source [=[

// Read only from on_frame_buf_complete, after fill_bounce_buffer latches
// bb_fb_index. VSYNC alone cannot acknowledge a queued framebuffer switch.
bool IRAM_ATTR crowpanel_rgb_bounce_uses_buffer(esp_lcd_panel_handle_t panel, const void *buffer)
{
    if (!panel || !buffer) return false;
    esp_rgb_panel_t *rgb = __containerof(panel, esp_rgb_panel_t, base);
    return rgb->bb_size && rgb->num_fbs == 2 && rgb->fbs[rgb->bb_fb_index] == buffer;
}

// The LCD VSYNC ISR calls the user callback only after selecting this chain.
// This lets LVGL recycle the old direct framebuffer without racing scanout.
bool IRAM_ATTR crowpanel_rgb_scanout_uses_buffer(esp_lcd_panel_handle_t panel, const void *buffer)
{
    if (!panel || !buffer) return false;
    esp_rgb_panel_t *rgb = __containerof(panel, esp_rgb_panel_t, base);
    return !rgb->bb_size && rgb->num_fbs > 1 &&
           rgb->fbs[rgb->crowpanel_scanout_fb_index] == buffer;
}

// Diagnostic lifetime counters. Each word is a single 32-bit read, but the
// whole snapshot is asynchronous. No IRQ logging or counter resets here.
esp_err_t crowpanel_rgb_read_bounce_stats(esp_lcd_panel_handle_t panel, uint32_t stats[7])
{
    if (!panel || !stats) return ESP_ERR_INVALID_ARG;
    esp_rgb_panel_t *rgb = __containerof(panel, esp_rgb_panel_t, base);
    stats[0] = rgb->crowpanel_copies;
    stats[1] = rgb->crowpanel_cache_skips;
    stats[2] = rgb->crowpanel_copy_max_us;
    stats[3] = rgb->crowpanel_restarts;
    stats[4] = rgb->crowpanel_short_eof;
    stats[5] = rgb->crowpanel_last_eof;
    uint32_t lines = rgb->bb_size / (rgb->timings.h_res * (rgb->fb_bits_per_pixel / 8));
    uint32_t htotal = rgb->timings.h_res + rgb->timings.hsync_back_porch +
                      rgb->timings.hsync_front_porch + rgb->timings.hsync_pulse_width;
    stats[6] = rgb->timings.pclk_hz ?
        (uint64_t)lines * htotal * 1000000 / rgb->timings.pclk_hz : 0;
    return ESP_OK;
}

esp_err_t crowpanel_rgb_read_registers(esp_lcd_panel_handle_t panel, uint32_t regs[16])
{
    if (!panel || !regs) return ESP_ERR_INVALID_ARG;
    esp_rgb_panel_t *rgb = __containerof(panel, esp_rgb_panel_t, base);
    lcd_cam_dev_t *dev = rgb->hal.dev;
    int ch = rgb->crowpanel_dma_channel;
    regs[0] = dev->lcd_clock.val;
    regs[1] = dev->lcd_user.val;
    regs[2] = dev->lcd_ctrl.val;
    regs[3] = dev->lcd_ctrl1.val;
    regs[4] = dev->lcd_ctrl2.val;
    regs[5] = dev->lcd_dly_mode.val;
    regs[6] = dev->lcd_data_dout_mode.val;
    regs[7] = ch;
    regs[8] = GDMA.channel[ch].out.conf0.val;
    regs[9] = GDMA.channel[ch].out.conf1.val;
    regs[10] = GDMA.channel[ch].out.int_raw.val;
    regs[11] = GDMA.channel[ch].out.outfifo_status.val;
    regs[12] = GDMA.channel[ch].out.dscr;
    regs[13] = GDMA.channel[ch].out.state.val;
    regs[14] = GDMA.channel[ch].out.link.val;
    regs[15] = dev->lcd_misc.val;
    return ESP_OK;
}
]=])
    # configure_file preserves mtime if content is unchanged on reconfigure.
    file(WRITE "${output}.in" "${rgb_source}")
    configure_file("${output}.in" "${output}" COPYONLY)
endfunction()

# Also callable in script mode for regression tests without configuring IDF.
if(CMAKE_SCRIPT_MODE_FILE)
    crowpanel_patch_rgb_source("${RGB_INPUT}" "${RGB_OUTPUT}")
    return()
endif()

if(NOT CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
    return()
endif()
if(NOT CONFIG_IDF_TARGET_ESP32S3)
    message(FATAL_ERROR "CrowPanel Advance RGB adaptation is ESP32-S3 only")
endif()

idf_component_get_property(rgb_component_dir esp_lcd COMPONENT_DIR)
idf_component_get_property(rgb_component_lib esp_lcd COMPONENT_LIB)
set(rgb_original "${rgb_component_dir}/rgb/esp_lcd_panel_rgb.c")
# Keep the object basename: IDF's linker fragment names esp_lcd_panel_rgb.*
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/crowpanel_rgb")
set(rgb_generated "${CMAKE_BINARY_DIR}/crowpanel_rgb/esp_lcd_panel_rgb.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rgb_original}")
crowpanel_patch_rgb_source("${rgb_original}" "${rgb_generated}")
get_target_property(rgb_sources ${rgb_component_lib} SOURCES)
set(rgb_replaced FALSE)
set(rgb_patched_sources "")
foreach(rgb_source IN LISTS rgb_sources)
    get_filename_component(rgb_filename "${rgb_source}" NAME)
    if(rgb_filename STREQUAL "esp_lcd_panel_rgb.c")
        list(APPEND rgb_patched_sources "${rgb_generated}")
        set(rgb_replaced TRUE)
    else()
        list(APPEND rgb_patched_sources "${rgb_source}")
    endif()
endforeach()
if(NOT rgb_replaced)
    message(FATAL_ERROR "CrowPanel RGB: cannot locate RGB component source to replace")
endif()
set_property(TARGET ${rgb_component_lib} PROPERTY SOURCES "${rgb_patched_sources}")
# The original source uses a sibling private header for software rotation.
target_include_directories(${rgb_component_lib} PRIVATE "${rgb_component_dir}/rgb" "${CMAKE_CURRENT_LIST_DIR}")
idf_component_get_property(crowpanel_timer_lib esp_timer COMPONENT_LIB)
target_link_libraries(${rgb_component_lib} PRIVATE ${crowpanel_timer_lib})
idf_component_get_property(crowpanel_main_lib main COMPONENT_LIB)
target_compile_definitions(${crowpanel_main_lib} PRIVATE GHOST_CROWPANEL_RGB_FACTORY_DMA=1)
message(STATUS "CrowPanel RGB: factory clock, immediate VSYNC restart, EOF mode 1")
