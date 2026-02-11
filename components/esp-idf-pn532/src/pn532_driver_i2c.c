#if defined(__has_include)
#  if __has_include(<string.h>)
#    include <string.h>
#  else
#    include <stddef.h>
     // minimal declarations for lint-only environments
     void *memcpy(void *dest, const void *src, size_t n);
     size_t strlen(const char *s);
#  endif
#else
#  include <string.h>
#endif
#include <stdbool.h>
#include "pn532_driver.h"
#include "pn532_driver_i2c.h"
#include "esp_log.h"
#include "i2c_bus_lock.h"

static const char TAG[] = "pn532_driver_i2c_legacy";

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    i2c_port_t i2c_port_number;
    uint8_t frame_buffer[256];
    bool owns_i2c_driver;
} pn532_i2c_driver_config;

static esp_err_t pn532_init_io(pn532_io_handle_t io_handle);
static void pn532_release_driver(pn532_io_handle_t io_handle);
static void pn532_release_io(pn532_io_handle_t io_handle);
static esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
static esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);
static esp_err_t pn532_is_ready(pn532_io_handle_t io_handle);

esp_err_t pn532_new_driver_i2c(gpio_num_t sda,
                               gpio_num_t scl,
                               gpio_num_t reset,
                               gpio_num_t irq,
                               i2c_port_t i2c_port_number,
                               pn532_io_handle_t io_handle)
{
    if (io_handle == NULL)
        return ESP_ERR_INVALID_ARG;

    pn532_i2c_driver_config *dev_config = heap_caps_calloc(1, sizeof(pn532_i2c_driver_config), MALLOC_CAP_DEFAULT);
    if (dev_config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    io_handle->reset = reset;
    io_handle->irq = irq;

    dev_config->i2c_port_number = i2c_port_number;
    dev_config->scl = scl;
    dev_config->sda = sda;
    io_handle->driver_data = dev_config;

    io_handle->pn532_init_io = pn532_init_io;
    io_handle->pn532_release_io = pn532_release_io;
    io_handle->pn532_release_driver = pn532_release_driver;
    io_handle->pn532_read = pn532_read;
    io_handle->pn532_write = pn532_write;
    io_handle->pn532_init_extra = NULL;
    io_handle->pn532_is_ready = pn532_is_ready;

#ifdef CONFIG_ENABLE_IRQ_ISR
    io_handle->IRQQueue = NULL;
#endif

    return ESP_OK;
}

void pn532_release_driver(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL)
        return;

    // Ensure IO is released (safe if already released)
    pn532_release_io(io_handle);
    free(io_handle->driver_data);
    io_handle->driver_data = NULL;
}

esp_err_t pn532_init_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = driver_config->sda,
        .scl_io_num = driver_config->scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };

    driver_config->owns_i2c_driver = false;

    esp_err_t r = i2c_driver_install(driver_config->i2c_port_number, conf.mode, 0, 0, 0);
    if (r == ESP_OK) {
        driver_config->owns_i2c_driver = true;
        r = i2c_param_config(driver_config->i2c_port_number, &conf);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2c_param_config failed: 0x%x", (int)r);
            i2c_driver_delete(driver_config->i2c_port_number);
            driver_config->owns_i2c_driver = false;
            return r;
        }
    } else if (r == ESP_ERR_INVALID_STATE || r == ESP_FAIL) {
        driver_config->owns_i2c_driver = false;
        ESP_LOGW(TAG, "i2c_driver_install not owned by PN532 (already installed), proceeding with shared bus");
    } else {
        ESP_LOGE(TAG, "i2c_driver_install failed: 0x%x", (int)r);
        return r;
    }
    return ESP_OK;
}

void pn532_release_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return;
    }
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (driver_config->owns_i2c_driver) {
        esp_err_t r = i2c_driver_delete(driver_config->i2c_port_number);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "i2c_driver_delete failed: 0x%x", (int)r);
        }
        driver_config->owns_i2c_driver = false;
    }
}

esp_err_t pn532_is_ready(pn532_io_handle_t io_handle)
{
    // Prefer IRQ when present; if IRQ not asserted, also check status byte over I2C
    if (io_handle->irq != GPIO_NUM_NC) {
        int level = gpio_get_level(io_handle->irq);
        if (level == 0) return ESP_OK;
        // Fallback: read PN532 status byte (0x01 means ready)
    }
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    uint8_t status = 0;
    bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
    if (!locked) return ESP_ERR_TIMEOUT;
    esp_err_t res = i2c_master_read_from_device(driver_config->i2c_port_number, 0x24, &status, 1, 30 / portTICK_PERIOD_MS);
    i2c_bus_unlock((int)driver_config->i2c_port_number);
    if (res != ESP_OK) return res;
    return (status == 0x01) ? ESP_OK : ESP_FAIL;
}

esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms)
{
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (!driver_config) return ESP_ERR_INVALID_ARG;
    uint8_t rx_buffer[256];
    if (read_size + 1 > sizeof(rx_buffer)) return ESP_ERR_INVALID_SIZE;
    int timeout = (xfer_timeout_ms > 0) ? xfer_timeout_ms / portTICK_PERIOD_MS : 100 / portTICK_PERIOD_MS;
    bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
    if (!locked) return ESP_ERR_TIMEOUT;
    esp_err_t res = i2c_master_read_from_device(driver_config->i2c_port_number, 0x24, rx_buffer, read_size + 1, timeout);
    i2c_bus_unlock((int)driver_config->i2c_port_number);
    if (res != ESP_OK) return res;
    if (rx_buffer[0] != 0x01) return ESP_ERR_TIMEOUT;
    memcpy(read_buffer, rx_buffer + 1, read_size);
    return ESP_OK;
}

esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms)
{
    pn532_i2c_driver_config *driver_config = (pn532_i2c_driver_config *)io_handle->driver_data;
    if (!driver_config) return ESP_ERR_INVALID_ARG;
    // Ensure we don't overflow the local frame buffer (prefix + payload + suffix)
    if (write_size + 2 > sizeof(driver_config->frame_buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    driver_config->frame_buffer[0] = 0;
    memcpy(driver_config->frame_buffer + 1, write_buffer, write_size);
    driver_config->frame_buffer[write_size + 1] = 0;
    int timeout = (xfer_timeout_ms > 0) ? xfer_timeout_ms / portTICK_PERIOD_MS : 100 / portTICK_PERIOD_MS;
    bool locked = i2c_bus_lock((int)driver_config->i2c_port_number, 300);
    if (!locked) return ESP_ERR_TIMEOUT;
    esp_err_t r = i2c_master_write_to_device(driver_config->i2c_port_number, 0x24, driver_config->frame_buffer, write_size + 2, timeout);
    i2c_bus_unlock((int)driver_config->i2c_port_number);
    return r;
}


