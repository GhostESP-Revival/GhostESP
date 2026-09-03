#include "vendor/drivers/crowpanel_epaper.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

#define TAG "CrowPanelEPD"

/* These are the exact pins from Elecrow's EPD_SPI.h. */
#define EPD_SCK_GPIO   12
#define EPD_MOSI_GPIO  11
#define EPD_RES_GPIO   47
#define EPD_DC_GPIO    46
#define EPD_CS_GPIO    45
#define EPD_BUSY_GPIO  48
#define EPD_POWER_GPIO 7

#define EPD_BUSY_TIMEOUT_MS 10000U
#define EPD_PARTIAL_MIN_MS  350U
#define EPD_FULL_AREA_PERCENT 38U
#define EPD_FRAME_DEBOUNCE_MS 90U
#define EPD_REFRESH_TASK_PRIORITY 5

typedef enum {
    EPD_PROTOCOL_UNKNOWN = 0,
    EPD_PROTOCOL_LEGACY_SSD1683,
    EPD_PROTOCOL_REVISED_GREEN,
} epd_protocol_t;

static SemaphoreHandle_t s_epd_lock;
static TaskHandle_t s_refresh_task;
static uint8_t *s_frame;
static uint8_t *s_refresh_frame;
static uint8_t *s_presented;
static bool s_initialized;
static bool s_frame_dirty;
static bool s_first_present = true;
static bool s_logged_first_flush;
static uint32_t s_last_present_ms;
static uint32_t s_partial_refreshes;
static epd_protocol_t s_protocol;
static bool s_revised_panel_primed;

static uint8_t *epd_alloc_framebuffer(const char *name)
{
    /* The panel is GPIO-bit-banged, so these buffers do not need DMA-capable
     * internal RAM. Native S3 Wi-Fi uses a substantial amount of internal
     * memory before display init; keep all 45 KB of EPD state in PSRAM. */
    uint8_t *buffer = heap_caps_malloc(CROWPANEL_EPAPER_FRAME_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        /* Preserve operation on variants without usable PSRAM, if enough
         * ordinary byte-addressable heap remains. */
        buffer = heap_caps_malloc(CROWPANEL_EPAPER_FRAME_BYTES,
                                  MALLOC_CAP_8BIT);
    }
    if (!buffer) {
        ESP_LOGE(TAG, "SSD1683 %s framebuffer allocation failed", name);
    }
    return buffer;
}

static bool epd_find_difference(const uint8_t *frame,
                                uint16_t *xs, uint16_t *ys,
                                uint16_t *xe, uint16_t *ye,
                                size_t *changed_bytes);

static inline uint32_t epd_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t epd_spi_write(const uint8_t *data, size_t len)
{
    /* The factory firmware deliberately bit-bangs the panel.  This is not
     * just an implementation detail: the panel uses GPIO11/12 while the
     * factory SD driver owns HSPI (SPI2) on GPIO40/39/13.  Keeping the EPD
     * off the SPI peripheral lets both interfaces coexist without rebinding
     * the bus or stealing the SD pins. */
    if (len == 0) return ESP_OK;

    for (size_t byte_index = 0; byte_index < len; byte_index++) {
        uint8_t value = data[byte_index];
        gpio_set_level(EPD_CS_GPIO, 0);
        for (uint8_t bit = 0; bit < 8; bit++) {
            gpio_set_level(EPD_SCK_GPIO, 0);
            gpio_set_level(EPD_MOSI_GPIO, (value & 0x80U) != 0);
            /* gpio_set_level() is considerably faster than Arduino's
             * digitalWrite() used by Elecrow. Guarantee data setup/hold and
             * keep this long flying-lead-style GPIO bus comfortably slow. */
            esp_rom_delay_us(1);
            gpio_set_level(EPD_SCK_GPIO, 1);
            esp_rom_delay_us(1);
            value <<= 1;
        }
        gpio_set_level(EPD_CS_GPIO, 1);
    }
    return ESP_OK;
}

static esp_err_t epd_write_command(uint8_t command)
{
    gpio_set_level(EPD_DC_GPIO, 0);
    esp_err_t err = epd_spi_write(&command, 1);
    /* Elecrow's EPD_WR_REG() always restores DC high after the command. */
    gpio_set_level(EPD_DC_GPIO, 1);
    return err;
}

static esp_err_t epd_write_data(const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_DC_GPIO, 1);
    return epd_spi_write(data, len);
}

static esp_err_t epd_write_data_byte(uint8_t data)
{
    return epd_write_data(&data, 1);
}

static esp_err_t epd_wait_busy_timeout(const char *phase, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (gpio_get_level(EPD_BUSY_GPIO) != 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG,
                     "SSD1683 BUSY timeout during %s (BUSY=%d PWR=%d RES=%d DC=%d CS=%d SCK=%d)",
                     phase ? phase : "unknown",
                     gpio_get_level(EPD_BUSY_GPIO),
                     gpio_get_level(EPD_POWER_GPIO),
                     gpio_get_level(EPD_RES_GPIO),
                     gpio_get_level(EPD_DC_GPIO),
                     gpio_get_level(EPD_CS_GPIO),
                     gpio_get_level(EPD_SCK_GPIO));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

static esp_err_t epd_wait_busy(const char *phase)
{
    return epd_wait_busy_timeout(phase, EPD_BUSY_TIMEOUT_MS);
}

static esp_err_t epd_reset(void)
{
    /* The 5.79 factory firmware uses a short SSD1683 reset pulse. Keep the
     * longer reset used by the revised 4.2 panel on its own path. */
    gpio_set_level(EPD_RES_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RES_GPIO, 0);
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    vTaskDelay(pdMS_TO_TICKS(10));
#else
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    gpio_set_level(EPD_RES_GPIO, 1);
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    vTaskDelay(pdMS_TO_TICKS(10));
#else
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    return ESP_OK;
}

static esp_err_t epd_detect_protocol(void)
{
    if (s_protocol != EPD_PROTOCOL_UNKNOWN) return ESP_OK;

#if defined(CONFIG_CROWPANEL_EPAPER_579)
    /* The 5.79 factory hardware is always the dual-SSD1683 panel. BUSY level
     * probing can classify it as the unrelated revised 4.2 controller. */
    s_protocol = EPD_PROTOCOL_LEGACY_SSD1683;
    ESP_LOGI(TAG, "Using legacy SSD1683 protocol for CrowPanel 5.79");
    return ESP_OK;
#endif

    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "EPD reset failed during protocol probe");

    /* Original SSD1683 panels return BUSY low after hardware reset. Elecrow's
     * revised/green-sticker controller deliberately remains high until its
     * initialization registers are loaded, so waiting here deadlocks it. */
    const int64_t deadline = esp_timer_get_time() + 500000LL;
    while (gpio_get_level(EPD_BUSY_GPIO) != 0 && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_protocol = gpio_get_level(EPD_BUSY_GPIO) == 0
                     ? EPD_PROTOCOL_LEGACY_SSD1683
                     : EPD_PROTOCOL_REVISED_GREEN;
    ESP_LOGI(TAG, "Detected %s CrowPanel e-paper protocol (BUSY=%d after reset)",
             s_protocol == EPD_PROTOCOL_REVISED_GREEN ? "revised/green-sticker" : "legacy SSD1683",
             gpio_get_level(EPD_BUSY_GPIO));
    return ESP_OK;
}

static esp_err_t epd_set_address(uint16_t xs, uint16_t ys,
                                 uint16_t xe, uint16_t ye)
{
    esp_err_t err = epd_write_command(0x44);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)(xs >> 3))) != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)(xe >> 3))) != ESP_OK) return err;

    if ((err = epd_write_command(0x45)) != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)ys)) != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)(ys >> 8))) != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)ye)) != ESP_OK) return err;
    return epd_write_data_byte((uint8_t)(ye >> 8));
}

static esp_err_t epd_set_cursor(uint16_t xs, uint16_t ys)
{
    esp_err_t err = epd_write_command(0x4E);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)xs)) != ESP_OK) return err;
    if ((err = epd_write_command(0x4F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte((uint8_t)ys)) != ESP_OK) return err;
    return epd_write_data_byte((uint8_t)(ys >> 8));
}

static esp_err_t epd_init_legacy_fast(void)
{
    esp_err_t err = epd_reset();
    if (err != ESP_OK) return err;
    /* Match EPD_Init_Fast() exactly: the factory waits once after the
     * hardware reset and again after issuing software reset. */
    if ((err = epd_wait_busy("hardware reset")) != ESP_OK) return err;
    if ((err = epd_write_command(0x12)) != ESP_OK) return err;
    if ((err = epd_wait_busy("software reset 0x12")) != ESP_OK) return err;

    /* Elecrow's factory fast-init sequence differs between the 4.2 and 5.79
     * panels. The 5.79 values are from EPD_FastMode1Init(). */
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    if ((err = epd_write_command(0x18)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x80)) != ESP_OK) return err;
    if ((err = epd_write_command(0x22)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0xB1)) != ESP_OK) return err;
    if ((err = epd_write_command(0x20)) != ESP_OK) return err;
    if ((err = epd_wait_busy("temperature load")) != ESP_OK) return err;
    if ((err = epd_write_command(0x1A)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x64)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x22)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x91)) != ESP_OK) return err;
    if ((err = epd_write_command(0x20)) != ESP_OK) return err;
    if ((err = epd_wait_busy("temperature load")) != ESP_OK) return err;
    if ((err = epd_write_command(0x3C)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x03)) != ESP_OK) return err;
    if ((err = epd_wait_busy("border waveform setup")) != ESP_OK) return err;
#else
    /* Elecrow EPD_Init_Fast(Fast_Seconds_1_5s). */
    if ((err = epd_write_command(0x21)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x40)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x3C)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x05)) != ESP_OK) return err;
    if ((err = epd_write_command(0x1A)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x6E)) != ESP_OK) return err;
    if ((err = epd_write_command(0x22)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x91)) != ESP_OK) return err;
    if ((err = epd_write_command(0x20)) != ESP_OK) return err;
    if ((err = epd_wait_busy("temperature load")) != ESP_OK) return err;
#endif

#if defined(CONFIG_CROWPANEL_EPAPER_579)
    /* The 5.79-inch factory firmware uses two SSD1683 memories as a
     * cascaded 800x272 surface. Each controller is addressed as 400x272;
     * the second controller is selected through the SSD1683 secondary-RAM
     * command sequence below. */
    if ((err = epd_write_command(0x11)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x05)) != ESP_OK) return err;
    if ((err = epd_write_command(0x44)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x31)) != ESP_OK) return err;
    if ((err = epd_write_command(0x45)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
#else
    if ((err = epd_write_command(0x11)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x03)) != ESP_OK) return err;
#endif
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    if ((err = epd_set_address(0, 0, 399, CROWPANEL_EPAPER_HEIGHT - 1)) != ESP_OK) return err;
#else
    if ((err = epd_set_address(0, 0,
                               CROWPANEL_EPAPER_WIDTH - 1,
                               CROWPANEL_EPAPER_HEIGHT - 1)) != ESP_OK) return err;
#endif
    return epd_set_cursor(0, 0);
}

static esp_err_t epd_init_revised(void)
{
    esp_err_t err = epd_reset();
    if (err != ESP_OK) return err;

    /* Elecrow's revised 4.2_Example7_Global_Refresh sequence. Do not poll BUSY
     * here: this controller keeps BUSY high until these registers are loaded. */
    static const uint8_t panel_setting[] = {0x3F, 0x4D};
    static const uint8_t power_setting[] = {0x03, 0x10, 0x3F, 0x3F, 0x03};
    static const uint8_t booster[] = {0x96, 0x96, 0x29};
    static const uint8_t resolution[] = {0x01, 0x90, 0x01, 0x2C};

    if ((err = epd_write_command(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data(panel_setting, sizeof(panel_setting))) != ESP_OK) return err;
    if ((err = epd_write_command(0x01)) != ESP_OK) return err;
    if ((err = epd_write_data(power_setting, sizeof(power_setting))) != ESP_OK) return err;
    if ((err = epd_write_command(0x06)) != ESP_OK) return err;
    if ((err = epd_write_data(booster, sizeof(booster))) != ESP_OK) return err;
    if ((err = epd_write_command(0x30)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x09)) != ESP_OK) return err;
    if ((err = epd_write_command(0x61)) != ESP_OK) return err;
    if ((err = epd_write_data(resolution, sizeof(resolution))) != ESP_OK) return err;
    if ((err = epd_write_command(0x82)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x05)) != ESP_OK) return err;
    if ((err = epd_write_command(0x50)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x97)) != ESP_OK) return err;
    if ((err = epd_write_command(0x60)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x22)) != ESP_OK) return err;
    if ((err = epd_write_command(0xE3)) != ESP_OK) return err;
    return epd_write_data_byte(0x88);
}

/* Full-refresh waveforms from Elecrow's revised panel factory driver. Missing
 * entries in each 42-byte row are intentionally zero-filled. */
static const uint8_t s_revised_full_lut[5][42] = {
    {0x01, 0x14, 0x0A, 0x14, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
};

static esp_err_t epd_load_revised_full_lut(void)
{
    for (uint8_t bank = 0; bank < 5; bank++) {
        esp_err_t err = epd_write_command((uint8_t)(0x20 + bank));
        if (err != ESP_OK) return err;
        if ((err = epd_write_data(s_revised_full_lut[bank], 42)) != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t epd_trigger_revised_update(const char *phase)
{
    esp_err_t err = epd_load_revised_full_lut();
    if (err != ESP_OK) return err;
    if ((err = epd_write_command(0x17)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0xA5)) != ESP_OK) return err;
    return epd_wait_busy_timeout(phase, 30000U);
}

static esp_err_t epd_prime_revised_panel(void)
{
    if (s_revised_panel_primed) return ESP_OK;
    esp_err_t err = epd_init_revised();
    if (err != ESP_OK) return err;

    /* The revised controller's first differential refresh needs both previous
     * and current RAM initialized. On this revision 0 is white, 1 is black. */
    static const uint8_t white_chunk[64] = {0};
    const uint8_t ram_commands[] = {0x10, 0x13};
    for (size_t ram = 0; ram < sizeof(ram_commands); ram++) {
        if ((err = epd_write_command(ram_commands[ram])) != ESP_OK) return err;
        size_t remaining = CROWPANEL_EPAPER_FRAME_BYTES;
        while (remaining > 0) {
            const size_t count = remaining < sizeof(white_chunk) ? remaining : sizeof(white_chunk);
            if ((err = epd_write_data(white_chunk, count)) != ESP_OK) return err;
            remaining -= count;
        }
    }
    if ((err = epd_trigger_revised_update("revised panel prime")) != ESP_OK) return err;
    s_revised_panel_primed = true;
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}

static esp_err_t epd_init_for_protocol(void)
{
    ESP_RETURN_ON_ERROR(epd_detect_protocol(), TAG, "EPD protocol detection failed");
    return s_protocol == EPD_PROTOCOL_REVISED_GREEN
               ? epd_init_revised()
               : epd_init_legacy_fast();
}

static esp_err_t epd_init_partial_window(uint16_t xs, uint16_t ys,
                                         uint16_t xe, uint16_t ye)
{
    /* The factory partial path changes the border waveform and display update
     * control before writing the aligned window. Keep the factory values. */
    esp_err_t err = epd_write_command(0x3C);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x80)) != ESP_OK) return err;
    if ((err = epd_write_command(0x21)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x3C)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x80)) != ESP_OK) return err;
    if ((err = epd_set_address(xs, ys, xe, ye)) != ESP_OK) return err;
    return epd_set_cursor(xs >> 3, ys);
}

static esp_err_t epd_sleep(void)
{
    const bool revised = s_protocol == EPD_PROTOCOL_REVISED_GREEN;
    esp_err_t err = epd_write_command(revised ? 0x07 : 0x10);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(revised ? 0xA5 : 0x01)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t epd_trigger_update(bool partial)
{
    esp_err_t err = epd_write_command(0x22);
    if (err != ESP_OK) return err;
    // The 5.79 factory firmware uses 0xDC for partial updates. Other legacy
    // panels retain their existing 0xFF partial waveform.
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    const uint8_t waveform = partial ? 0xDC : 0xF7;
#else
    const uint8_t waveform = partial ? 0xFF : 0xF7;
#endif
    if ((err = epd_write_data_byte(waveform)) != ESP_OK) return err;
    if ((err = epd_write_command(0x20)) != ESP_OK) return err;
    return epd_wait_busy(partial ? "partial update" : "full update");
}

#if defined(CONFIG_CROWPANEL_EPAPER_579)
static esp_err_t epd_trigger_579_fast_update(void)
{
    esp_err_t err = epd_write_command(0x22);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0xC7)) != ESP_OK) return err;
    if ((err = epd_write_command(0x20)) != ESP_OK) return err;
    return epd_wait_busy("5.79 fast update");
}
#endif

static esp_err_t epd_write_window(const uint8_t *frame,
                                  uint16_t xs, uint16_t ys,
                                  uint16_t xe, uint16_t ye)
{
    const uint16_t width_bytes = (uint16_t)((xe - xs + 1) / 8);
    const uint16_t height = (uint16_t)(ye - ys + 1);
    const uint8_t *row = &frame[(size_t)ys * (CROWPANEL_EPAPER_STORAGE_WIDTH / 8) + (xs / 8)];

    esp_err_t err = epd_write_command(0x24);
    if (err != ESP_OK) return err;
    for (uint16_t y = 0; y < height; y++) {
        if ((err = epd_write_data(row, width_bytes)) != ESP_OK) return err;
        row += CROWPANEL_EPAPER_STORAGE_WIDTH / 8;
    }
    return ESP_OK;
}

#if defined(CONFIG_CROWPANEL_EPAPER_579)
static esp_err_t epd_set_579_primary_area(void)
{
    esp_err_t err = epd_write_command(0x11);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x05)) != ESP_OK) return err;
    if ((err = epd_write_command(0x44)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x31)) != ESP_OK) return err;
    if ((err = epd_write_command(0x45)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    return epd_write_data_byte(0x00);
}

static esp_err_t epd_set_579_secondary_area(void)
{
    esp_err_t err = epd_write_command(0x91);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x04)) != ESP_OK) return err;
    if ((err = epd_write_command(0xC4)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x31)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0xC5)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0xCE)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x31)) != ESP_OK) return err;
    if ((err = epd_write_command(0xCF)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    return epd_write_data_byte(0x01);
}

static esp_err_t epd_clear_579_factory_state(void)
{
    /* Match the factory startup sequence: initialize all four controller RAM
     * planes and perform one full clear before the first application frame. */
    const size_t controller_bytes = 50U * CROWPANEL_EPAPER_HEIGHT;
    uint8_t value;
    esp_err_t err = epd_set_579_primary_area();
    if (err != ESP_OK) return err;
    if ((err = epd_write_command(0x4E)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x4F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_command(0x24)) != ESP_OK) return err;
    value = 0xFF;
    for (size_t i = 0; i < controller_bytes; i++) {
        if ((err = epd_write_data_byte(value)) != ESP_OK) return err;
    }

    if ((err = epd_set_579_primary_area()) != ESP_OK) return err;
    if ((err = epd_write_command(0x4E)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x4F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_command(0x26)) != ESP_OK) return err;
    value = 0x00;
    for (size_t i = 0; i < controller_bytes; i++) {
        if ((err = epd_write_data_byte(value)) != ESP_OK) return err;
    }

    if ((err = epd_set_579_secondary_area()) != ESP_OK) return err;
    if ((err = epd_write_command(0xA4)) != ESP_OK) return err;
    value = 0xFF;
    for (size_t i = 0; i < controller_bytes; i++) {
        if ((err = epd_write_data_byte(value)) != ESP_OK) return err;
    }

    if ((err = epd_set_579_secondary_area()) != ESP_OK) return err;
    if ((err = epd_write_command(0xA6)) != ESP_OK) return err;
    value = 0x00;
    for (size_t i = 0; i < controller_bytes; i++) {
        if ((err = epd_write_data_byte(value)) != ESP_OK) return err;
    }
    if ((err = epd_trigger_update(false)) != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t epd_write_579_frame(const uint8_t *frame)
{
    /* Elecrow's EPD_Display sends each 400-pixel controller column-by-column
     * from the padded 800x272 image, then repeats the operation for the
     * secondary RAM. The 8-pixel hole is the physical cascade boundary. */
    const uint16_t row_bytes = CROWPANEL_EPAPER_STORAGE_WIDTH / 8;
    static bool logged_frame_distribution;
    if (!logged_frame_distribution) {
        size_t primary_nonwhite = 0;
        size_t secondary_nonwhite = 0;
        for (uint16_t y = 0; y < CROWPANEL_EPAPER_HEIGHT; y++) {
            for (uint16_t col = 0; col < 50; col++) {
                if (frame[(size_t)y * row_bytes + col] != 0xFF) primary_nonwhite++;
            }
            for (uint16_t col = 50; col < 100; col++) {
                if (frame[(size_t)y * row_bytes + col] != 0xFF) secondary_nonwhite++;
            }
        }
        ESP_LOGI(TAG, "5.79 frame data: primary=%u secondary=%u non-white bytes",
                 (unsigned)primary_nonwhite, (unsigned)secondary_nonwhite);
        logged_frame_distribution = true;
    }
    esp_err_t err = epd_set_579_primary_area();
    if (err != ESP_OK) return err;
    err = epd_write_command(0x4E);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x00)) != ESP_OK) return err;
    if ((err = epd_write_command(0x4F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x0F)) != ESP_OK) return err;
    if ((err = epd_write_data_byte(0x01)) != ESP_OK) return err;
    if ((err = epd_write_command(0x24)) != ESP_OK) return err;
    for (uint16_t col = 0; col < 50; col++) {
        for (uint16_t y = 0; y < CROWPANEL_EPAPER_HEIGHT; y++) {
            if ((err = epd_write_data_byte(frame[(size_t)y * row_bytes + col])) != ESP_OK) return err;
        }
    }

    if ((err = epd_set_579_secondary_area()) != ESP_OK) return err;
    if ((err = epd_write_command(0xA4)) != ESP_OK) return err;
    for (uint16_t col = 50; col < 100; col++) {
        for (uint16_t y = 0; y < CROWPANEL_EPAPER_HEIGHT; y++) {
            if ((err = epd_write_data_byte(frame[(size_t)y * row_bytes + col])) != ESP_OK) return err;
        }
    }
    return ESP_OK;
}
#endif

static esp_err_t epd_write_revised_frame(const uint8_t *frame)
{
    esp_err_t err = epd_write_command(0x50);
    if (err != ESP_OK) return err;
    if ((err = epd_write_data_byte(0xD7)) != ESP_OK) return err;
    if ((err = epd_write_command(0x13)) != ESP_OK) return err;

    /* GhostESP stores 1=white like the legacy SSD1683; revised RAM is the
     * opposite polarity. Invert in small chunks without another framebuffer. */
    uint8_t inverted[64];
    for (size_t offset = 0; offset < CROWPANEL_EPAPER_FRAME_BYTES; offset += sizeof(inverted)) {
        const size_t count = CROWPANEL_EPAPER_FRAME_BYTES - offset < sizeof(inverted)
                                 ? CROWPANEL_EPAPER_FRAME_BYTES - offset
                                 : sizeof(inverted);
        for (size_t i = 0; i < count; i++) inverted[i] = (uint8_t)~frame[offset + i];
        if ((err = epd_write_data(inverted, count)) != ESP_OK) return err;
    }
    return ESP_OK;
}

static void epd_present_frame(const uint8_t *frame)
{
    uint16_t xs, ys, xe, ye;
    size_t changed_bytes = 0;
    const bool changed = epd_find_difference(frame, &xs, &ys, &xe, &ye, &changed_bytes);
    if (!changed) return;

    const size_t total_bytes = CROWPANEL_EPAPER_FRAME_BYTES;
    const bool first_present = s_first_present;
    const bool large_change = first_present ||
                              (changed_bytes * 100U >= total_bytes * EPD_FULL_AREA_PERCENT);
    const uint32_t now_ms = epd_now_ms();
    const uint32_t elapsed = now_ms - s_last_present_ms;
    bool use_partial = !large_change && elapsed >= EPD_PARTIAL_MIN_MS;
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    use_partial = false;
#endif

    esp_err_t err = epd_detect_protocol();
    if (err == ESP_OK && s_protocol == EPD_PROTOCOL_REVISED_GREEN) {
        /* The current factory sequence supplied by Elecrow is full-refresh
         * only. Do not send legacy partial-window commands to this controller. */
        use_partial = false;
        err = epd_prime_revised_panel();
    }
    if (err == ESP_OK) err = epd_init_for_protocol();
#if defined(CONFIG_CROWPANEL_EPAPER_579)
    if (err == ESP_OK && first_present) {
        err = epd_clear_579_factory_state();
        /* Factory setup runs EPD_FastMode1Init() again after the initial
         * clear and before writing its first application image. */
        if (err == ESP_OK) err = epd_init_legacy_fast();
    }
#endif
    if (err == ESP_OK) {
        if (s_protocol == EPD_PROTOCOL_REVISED_GREEN) {
            vTaskDelay(pdMS_TO_TICKS(300));
        } else if (use_partial) {
            err = epd_init_partial_window(xs, ys, xe, ye);
        }
#if !defined(CONFIG_CROWPANEL_EPAPER_579)
        else {
            xs = 0;
            ys = 0;
            xe = CROWPANEL_EPAPER_WIDTH - 1;
            ye = CROWPANEL_EPAPER_HEIGHT - 1;
            err = epd_set_address(xs, ys, xe, ye);
            if (err == ESP_OK) err = epd_set_cursor(xs, ys);
        }
#endif
    }
    if (err == ESP_OK) {
        err = s_protocol == EPD_PROTOCOL_REVISED_GREEN
                  ? epd_write_revised_frame(frame)
#if defined(CONFIG_CROWPANEL_EPAPER_579)
                  : epd_write_579_frame(frame);
#else
                  : epd_write_window(frame, xs, ys, xe, ye);
#endif
    }
    if (err == ESP_OK) {
#if defined(CONFIG_CROWPANEL_EPAPER_579)
        err = epd_trigger_579_fast_update();
#else
        err = s_protocol == EPD_PROTOCOL_REVISED_GREEN
                  ? epd_trigger_revised_update("revised full update")
                  : epd_trigger_update(use_partial);
#endif
    }
    if (err == ESP_OK && s_protocol == EPD_PROTOCOL_REVISED_GREEN) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err == ESP_OK) err = epd_sleep();

    if (err == ESP_OK) {
        memcpy(s_presented, frame, CROWPANEL_EPAPER_FRAME_BYTES);
        s_first_present = false;
        s_last_present_ms = epd_now_ms();
        if (use_partial) s_partial_refreshes++;
        if (first_present) {
            ESP_LOGI(TAG, "First SSD1683 physical refresh complete (%s)",
                     use_partial ? "partial" : "full");
        }
        ESP_LOGD(TAG, "%s refresh: %u changed bytes, window %u,%u-%u,%u",
                 use_partial ? "partial" : "full", (unsigned)changed_bytes,
                 xs, ys, xe, ye);
    } else {
        ESP_LOGE(TAG, "SSD1683 refresh failed: %s", esp_err_to_name(err));
    }
}

static void epd_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* A key press, view transition, and status-bar update can invalidate
         * the canvas in the same LVGL tick. Wait for that burst to settle and
         * submit one physical refresh instead of flashing the paper once per
         * invalidation. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EPD_FRAME_DEBOUNCE_MS)) > 0) {
        }

        bool should_refresh = false;
        if (xSemaphoreTake(s_epd_lock, portMAX_DELAY) == pdTRUE) {
            if (s_frame_dirty) {
                /* Snapshot LVGL's staging buffer, then release the mutex
                 * before touching the slow physical panel. LVGL must remain
                 * free to render while a multi-second e-paper update runs. */
                memcpy(s_refresh_frame, s_frame, CROWPANEL_EPAPER_FRAME_BYTES);
                s_frame_dirty = false;
                should_refresh = true;
            }
            xSemaphoreGive(s_epd_lock);
        }
        if (should_refresh) {
            epd_present_frame(s_refresh_frame);
        }
    }
}

static bool epd_pixel_is_white(lv_color_t color)
{
    /* lv_color_to32 handles LV_COLOR_16_SWAP and the LVGL bitfield layout. */
    lv_color32_t rgb = { .full = lv_color_to32(color) };
    const uint32_t luminance = (299U * rgb.ch.red +
                                587U * rgb.ch.green +
                                114U * rgb.ch.blue) / 1000U;
    return luminance >= 128U;
}

static void epd_update_frame_from_lvgl(const lv_area_t *area,
                                       const lv_color_t *color_p)
{
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= CROWPANEL_EPAPER_WIDTH) x2 = CROWPANEL_EPAPER_WIDTH - 1;
    if (y2 >= CROWPANEL_EPAPER_HEIGHT) y2 = CROWPANEL_EPAPER_HEIGHT - 1;

    const int32_t width = area->x2 - area->x1 + 1;
    for (int32_t y = y1; y <= y2; y++) {
        for (int32_t x = x1; x <= x2; x++) {
            const int32_t source_x = x - area->x1;
            const int32_t source_y = y - area->y1;
            const lv_color_t color = color_p[source_y * width + source_x];
            int32_t storage_x = x;
#if defined(CONFIG_CROWPANEL_EPAPER_579)
            if (storage_x >= 396) storage_x += 8;
            /* Factory source uses Rotation 180: transform after inserting
             * the physical 8-pixel cascade gap. */
            storage_x = (CROWPANEL_EPAPER_STORAGE_WIDTH - 1) - storage_x;
            const int32_t storage_y = (CROWPANEL_EPAPER_HEIGHT - 1) - y;
#else
            const int32_t storage_y = y;
#endif
            uint8_t *pixel_byte = &s_frame[(size_t)storage_y * (CROWPANEL_EPAPER_STORAGE_WIDTH / 8) + (storage_x / 8)];
            const uint8_t pixel_mask = (uint8_t)(0x80U >> (storage_x & 7));
            if (epd_pixel_is_white(color)) {
                *pixel_byte |= pixel_mask;
            } else {
                *pixel_byte &= (uint8_t)~pixel_mask;
            }
        }
    }
}

static bool epd_find_difference(const uint8_t *frame,
                                uint16_t *xs, uint16_t *ys,
                                uint16_t *xe, uint16_t *ye,
                                size_t *changed_bytes)
{
    const uint16_t row_bytes = CROWPANEL_EPAPER_STORAGE_WIDTH / 8;
    uint16_t min_x = row_bytes;
    uint16_t max_x = 0;
    uint16_t min_y = CROWPANEL_EPAPER_HEIGHT;
    uint16_t max_y = 0;
    size_t changed = 0;

    for (uint16_t y = 0; y < CROWPANEL_EPAPER_HEIGHT; y++) {
        for (uint16_t xb = 0; xb < row_bytes; xb++) {
            const size_t index = (size_t)y * row_bytes + xb;
            if (frame[index] == s_presented[index]) continue;
            changed++;
            if (xb < min_x) min_x = xb;
            if (xb > max_x) max_x = xb;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }

    if (changed == 0) return false;
    *xs = (uint16_t)(min_x * 8);
    *xe = (uint16_t)(max_x * 8 + 7);
    *ys = min_y;
    *ye = max_y;
    if (changed_bytes) *changed_bytes = changed;
    return true;
}

esp_err_t crowpanel_epaper_init(void)
{
    if (s_initialized) return ESP_OK;

    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << EPD_RES_GPIO) |
                        (1ULL << EPD_DC_GPIO) |
                        (1ULL << EPD_CS_GPIO) |
                        (1ULL << EPD_SCK_GPIO) |
                        (1ULL << EPD_MOSI_GPIO),
        /* Input remains enabled so diagnostics read the actual pad level. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "EPD output GPIO setup failed");

    /* app_main owns/reserves GPIO7. Reassert it without reconfiguring it;
     * configuring the already-reserved rail again triggers IDF 6.1's GPIO
     * ownership warning. */
    gpio_set_level(EPD_POWER_GPIO, 1);
    gpio_set_level(EPD_RES_GPIO, 1);
    gpio_set_level(EPD_SCK_GPIO, 1);
    gpio_set_level(EPD_MOSI_GPIO, 0);
    gpio_set_level(EPD_CS_GPIO, 1);
    gpio_set_level(EPD_DC_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << EPD_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_config), TAG, "EPD BUSY GPIO setup failed");
    ESP_LOGI(TAG, "EPD GPIO ready (BUSY=%d PWR=%d RES=%d DC=%d CS=%d SCK=%d)",
             gpio_get_level(EPD_BUSY_GPIO),
             gpio_get_level(EPD_POWER_GPIO),
             gpio_get_level(EPD_RES_GPIO),
             gpio_get_level(EPD_DC_GPIO),
             gpio_get_level(EPD_CS_GPIO),
             gpio_get_level(EPD_SCK_GPIO));

    s_frame = epd_alloc_framebuffer("LVGL staging");
    s_refresh_frame = epd_alloc_framebuffer("refresh snapshot");
    s_presented = epd_alloc_framebuffer("presented-state");
    s_epd_lock = xSemaphoreCreateMutex();
    if (!s_frame || !s_refresh_frame || !s_presented || !s_epd_lock) {
        ESP_LOGE(TAG, "SSD1683 framebuffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(epd_refresh_task, "EPDRefresh", 4096, NULL,
                    EPD_REFRESH_TASK_PRIORITY, &s_refresh_task) != pdPASS) {
        ESP_LOGE(TAG, "SSD1683 refresh task creation failed");
        return ESP_ERR_NO_MEM;
    }

    memset(s_frame, 0xFF, CROWPANEL_EPAPER_FRAME_BYTES);
    memset(s_refresh_frame, 0xFF, CROWPANEL_EPAPER_FRAME_BYTES);
    memset(s_presented, 0xFF, CROWPANEL_EPAPER_FRAME_BYTES);
    s_initialized = true;
    ESP_LOGI(TAG, "CrowPanel SSD1683 initialized (%ux%u, factory GPIO-SPI, buffers in %s)",
             CROWPANEL_EPAPER_WIDTH, CROWPANEL_EPAPER_HEIGHT,
             esp_ptr_external_ram(s_frame) &&
             esp_ptr_external_ram(s_refresh_frame) &&
             esp_ptr_external_ram(s_presented) ? "PSRAM" : "mixed heap");
    return ESP_OK;
}

void crowpanel_epaper_flush_cb(lv_disp_drv_t *drv,
                               const lv_area_t *area,
                               lv_color_t *color_p)
{
    if (!drv || !area || !color_p || !s_initialized) {
        if (drv) lv_disp_flush_ready(drv);
        return;
    }

    if (xSemaphoreTake(s_epd_lock, portMAX_DELAY) != pdTRUE) {
        lv_disp_flush_ready(drv);
        return;
    }

    epd_update_frame_from_lvgl(area, color_p);
    if (!s_logged_first_flush) {
        ESP_LOGI(TAG, "First LVGL flush received (area %ld,%ld-%ld,%ld)",
                 (long)area->x1, (long)area->y1,
                 (long)area->x2, (long)area->y2);
        s_logged_first_flush = true;
    }
    s_frame_dirty = true;
    xSemaphoreGive(s_epd_lock);
    if (s_refresh_task) xTaskNotifyGive(s_refresh_task);
    lv_disp_flush_ready(drv);
}
