/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted aligned SIMD loops from esp-bsp/esp_lvgl_port/src/lvgl9/simd:
 *   lv_color_blend_to_rgb565_esp32s3.S
 *   lv_rgb565_blend_normal_to_rgb565_esp32s3.S
 * CrowPanel changes: native packed RGB565 ABI, aligned blocks only, IRAM.
 * Scalar edges are handled by C; no vector overread or rounded-down stores.
 * Called only by tasks: IDF saves CP3 state on preemption. Do NOT disable
 * interrupts around these loops or call them from the RGB refill ISR.
 */
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_ESP32S3
#error "CrowPanel SIMD assembly requires ESP32-S3"
#endif

    .section .iram1, "ax"
    .align 4
    .global ghost_s3_fill_blocks
    .type ghost_s3_fill_blocks, @function
/* a2: 16-byte-aligned dst; a3: two packed RGB565 pixels; a4: 16-byte blocks */
ghost_s3_fill_blocks:
    entry a1, 32
    ee.movi.32.q q0, a3, 0
    ee.movi.32.q q0, a3, 1
    ee.movi.32.q q0, a3, 2
    ee.movi.32.q q0, a3, 3
    loopnez a4, .Lghost_fill_end
        ee.vst.128.ip q0, a2, 16
.Lghost_fill_end:
    retw.n
    .size ghost_s3_fill_blocks, .-ghost_s3_fill_blocks

    .align 4
    .global ghost_s3_copy_blocks
    .type ghost_s3_copy_blocks, @function
/* a2: aligned dst; a3: aligned src; a4: 16-byte blocks; no overlap */
ghost_s3_copy_blocks:
    entry a1, 32
    loopnez a4, .Lghost_copy_end
        ee.vld.128.ip q0, a3, 16
        ee.vst.128.ip q0, a2, 16
.Lghost_copy_end:
    retw.n
    .size ghost_s3_copy_blocks, .-ghost_s3_copy_blocks
