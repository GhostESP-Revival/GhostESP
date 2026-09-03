/* CrowPanel Advance 7 / ESP32-S3 only. Included by the board-local RGB driver.
 * Settings derived from Elecrow's shared LovyanGFX Bus_RGB.cpp and
 * platforms/esp32/common.cpp calcClockDiv(), at the factory 21 MHz setting.
 */
#pragma once

#include "hal/lcd_hal.h"
#include "hal/lcd_ll.h"
#include "hal/gdma_ll.h"
#include "soc/gdma_struct.h"
#include "esp_timer.h"

static inline uint32_t crowpanel_cal_pclk_freq(lcd_hal_context_t *hal,
                                               uint32_t src_hz, uint32_t pclk_hz,
                                               hal_utils_clk_div_t *divider)
{
    if (src_hz == 240000000U && pclk_hz == 21000000U) {
        // Factory: 240 MHz / (2 + 18/63) = 105 MHz, then /5 = 21 MHz.
        // IDF's default /2 prescaler gives the same average PCLK using a
        // different LCD module clock. Preserve the factory divider chain.
        divider->integer = 2;
        divider->denominator = 63;
        divider->numerator = 18;
        lcd_ll_set_pixel_clock_prescale(hal->dev, 5);
        return 21000000U;
    }
    // Keep the existing diagnostic clock command usable at other frequencies.
    return lcd_hal_cal_pclk_freq(hal, src_hz, pclk_hz, divider);
}

static inline __attribute__((always_inline)) void crowpanel_restart_dma(
    int channel, uintptr_t restart_addr)
{
    // The channel and descriptor were obtained from IDF during panel creation.
    // Same four register writes as factory, with no driver locks/callbacks
    // between VSYNC detection and restart. Do not reset the LCD FIFO here.
    GDMA.channel[channel].out.conf0.out_rst = 1;
    GDMA.channel[channel].out.conf0.out_rst = 0;
    GDMA.channel[channel].out.link.addr = restart_addr;
    GDMA.channel[channel].out.link.start = 1;
}
