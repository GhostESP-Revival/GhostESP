/**
 * PARLIO-backed single-data-line display transport for the Banshee ESP32-C5.
 *
 * The PARLIO TX driver provides the SPI wire protocol while using the
 * independent PARLIO peripheral. Commands and register data are synchronous;
 * color transfers are queued so LVGL can render into the other draw buffer
 * while PARLIO sends the current one.
 */

#include "banshee_c5_parlio.h"

#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/gpio.h"
#include "esp_private/parlio_tx_private.h"
#include "driver/gpio.h"
#include "driver/parlio_tx.h"

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#include "lvgl_spi_conf.h"

#ifndef CONFIG_CUSTOM_DISPLAY_BUFFER_BYTES
#define CONFIG_CUSTOM_DISPLAY_BUFFER_BYTES (CONFIG_TFT_WIDTH * 10 * sizeof(uint16_t))
#endif

#define TAG "banshee_c5_parl"
#define PARLIO_DMA_BURST_SIZE 32
#define PARLIO_TRANS_QUEUE_DEPTH 4

static parlio_tx_unit_handle_t s_tx_unit = NULL;
static size_t s_int_mem_align = 4;
static void * volatile s_color_flush_driver = NULL;

static bool IRAM_ATTR banshee_c5_parlio_on_trans_done(
        parlio_tx_unit_handle_t tx_unit,
        const parlio_tx_done_event_data_t *edata,
        void *user_ctx) {
    (void)tx_unit;
    (void)edata;
    (void)user_ctx;

    /* There is one in-flight LVGL color transfer at a time. Keep the exact
     * driver passed to flush_cb instead of relying on LVGL's global display. */
    void *flush_driver = s_color_flush_driver;
    s_color_flush_driver = NULL;
    if (flush_driver) {
        lv_disp_flush_ready((lv_disp_drv_t *)flush_driver);
    }
    return false;
}

esp_err_t banshee_c5_parlio_init(void) {
    if (s_tx_unit) {
        return ESP_OK;
    }

    gpio_func_sel(CONFIG_LV_DISP_PIN_DC, PIN_FUNC_GPIO);
    gpio_output_enable(CONFIG_LV_DISP_PIN_DC);
    gpio_set_level(CONFIG_LV_DISP_PIN_DC, 0);
    gpio_set_drive_capability(CONFIG_LV_DISP_SPI_MOSI, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(CONFIG_LV_DISP_SPI_CLK, GPIO_DRIVE_CAP_3);

    const parlio_tx_unit_config_t tx_config = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .data_width = 1,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = CONFIG_C5_PARLIO_CLOCK_HZ,
        .data_gpio_nums = { CONFIG_LV_DISP_SPI_MOSI },
        .clk_out_gpio_num = CONFIG_LV_DISP_SPI_CLK,
        .valid_gpio_num = CONFIG_LV_DISP_SPI_CS,
        .trans_queue_depth = PARLIO_TRANS_QUEUE_DEPTH,
        .max_transfer_size = CONFIG_CUSTOM_DISPLAY_BUFFER_BYTES,
        .dma_burst_size = PARLIO_DMA_BURST_SIZE,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB,
        .flags = {
            .invert_valid_out = true,
        },
    };

    esp_err_t ret = parlio_new_tx_unit(&tx_config, &s_tx_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PARLIO display transport: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = parlio_tx_get_alignment_constraints(s_tx_unit, &s_int_mem_align, NULL);
    if (ret != ESP_OK || s_int_mem_align == 0) {
        ESP_LOGE(TAG, "Failed to get PARLIO DMA alignment: %s",
                 esp_err_to_name(ret));
        parlio_del_tx_unit(s_tx_unit);
        s_tx_unit = NULL;
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    const parlio_tx_event_callbacks_t callbacks = {
        .on_trans_done = banshee_c5_parlio_on_trans_done,
    };
    ret = parlio_tx_unit_register_event_callbacks(s_tx_unit, &callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PARLIO completion callback: %s",
                 esp_err_to_name(ret));
        parlio_del_tx_unit(s_tx_unit);
        s_tx_unit = NULL;
        return ret;
    }

    ret = parlio_tx_unit_enable(s_tx_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable PARLIO TX unit: %s", esp_err_to_name(ret));
        parlio_del_tx_unit(s_tx_unit);
        s_tx_unit = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "PARLIO display ready: DATA0=%d CLK=%d CS=%d DC=%d PCLK=%dHz shift=NEG pack=MSB",
             CONFIG_LV_DISP_SPI_MOSI, CONFIG_LV_DISP_SPI_CLK,
             CONFIG_LV_DISP_SPI_CS, CONFIG_LV_DISP_PIN_DC,
             CONFIG_C5_PARLIO_CLOCK_HZ);
    return ESP_OK;
}

void banshee_c5_parlio_wait(void) {
    if (s_tx_unit) {
        parlio_tx_unit_wait_all_done(s_tx_unit, -1);
    }
}

static esp_err_t banshee_c5_parlio_send_sync(const void *data, size_t length) {
    if (!s_tx_unit) {
        return ESP_ERR_INVALID_STATE;
    }

    void *aligned_data = NULL;
    const void *transfer_data = data;
    size_t alignment = s_int_mem_align > 1 ? s_int_mem_align : 1;
    if (((uintptr_t)data & (alignment - 1)) != 0 ||
        (((length * 8) & (alignment - 1)) != 0)) {
        aligned_data = heap_caps_aligned_alloc(
            alignment, length + alignment - 1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!aligned_data) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(aligned_data, data, length);
        transfer_data = aligned_data;
    }

    banshee_c5_parlio_wait();
    const parlio_transmit_config_t tx_config = {.idle_value = 0};
    esp_err_t ret = parlio_tx_unit_transmit(
        s_tx_unit, transfer_data, length * 8, &tx_config);
    if (ret == ESP_OK) {
        ret = parlio_tx_unit_wait_all_done(s_tx_unit, -1);
    }
    free(aligned_data);
    return ret;
}

esp_err_t banshee_c5_parlio_send_cmd(uint8_t command) {
    if (!s_tx_unit) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(CONFIG_LV_DISP_PIN_DC, 0);
    esp_err_t ret = banshee_c5_parlio_send_sync(&command, sizeof(command));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Synchronous PARLIO command transfer failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t banshee_c5_parlio_send_data(const void *data, size_t length) {
    if (!s_tx_unit) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || length == 0) {
        return ESP_OK;
    }

    gpio_set_level(CONFIG_LV_DISP_PIN_DC, 1);
    esp_err_t ret = banshee_c5_parlio_send_sync(data, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Synchronous PARLIO data transfer failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t banshee_c5_parlio_send_color(const void *data, size_t length,
                                       void *flush_driver) {
    if (!s_tx_unit) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || length == 0) {
        return ESP_OK;
    }

    banshee_c5_parlio_wait();
    gpio_set_level(CONFIG_LV_DISP_PIN_DC, 1);

    s_color_flush_driver = flush_driver;
    const parlio_transmit_config_t tx_config = {.idle_value = 0};
    esp_err_t ret = parlio_tx_unit_transmit(
        s_tx_unit, data, length * 8, &tx_config);
    if (ret != ESP_OK) {
        s_color_flush_driver = NULL;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Asynchronous PARLIO color transfer failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

void *banshee_c5_parlio_alloc_draw_buffer(size_t size) {
    if (!s_tx_unit) {
        return NULL;
    }
    size_t alignment = s_int_mem_align > 1 ? s_int_mem_align : 1;
    return heap_caps_aligned_calloc(
        alignment, 1, size,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}
