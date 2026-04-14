#include "i2c_shared.h"

static esp_err_t with_temp_device(i2c_master_bus_handle_t bus,
                                  uint16_t addr,
                                  uint32_t scl_speed_hz,
                                  i2c_master_dev_handle_t *out_dev)
{
    if (!bus || !out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
    };

    return i2c_master_bus_add_device(bus, &dev_config, out_dev);
}

esp_err_t i2c_shared_get_or_create_bus(i2c_port_num_t port,
                                       gpio_num_t sda,
                                       gpio_num_t scl,
                                       bool enable_internal_pullup,
                                       i2c_master_bus_handle_t *out_bus,
                                       bool *out_created)
{
    if (!out_bus) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_master_get_bus_handle(port, out_bus);
    if (ret == ESP_OK) {
        if (out_created) {
            *out_created = false;
        }
        return ESP_OK;
    }
    if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = enable_internal_pullup,
    };

    ret = i2c_new_master_bus(&bus_config, out_bus);
    if (ret == ESP_OK && out_created) {
        *out_created = true;
    }
    return ret;
}

esp_err_t i2c_shared_add_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev)
{
    return with_temp_device(bus, addr, scl_speed_hz, out_dev);
}

esp_err_t i2c_shared_transmit_to_addr(i2c_master_bus_handle_t bus,
                                      uint16_t addr,
                                      uint32_t scl_speed_hz,
                                      const uint8_t *data,
                                      size_t len,
                                      int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = with_temp_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_master_transmit(dev, data, len, timeout_ms);
    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t i2c_shared_receive_from_addr(i2c_master_bus_handle_t bus,
                                       uint16_t addr,
                                       uint32_t scl_speed_hz,
                                       uint8_t *data,
                                       size_t len,
                                       int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = with_temp_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_master_receive(dev, data, len, timeout_ms);
    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t i2c_shared_transmit_receive_from_addr(i2c_master_bus_handle_t bus,
                                                uint16_t addr,
                                                uint32_t scl_speed_hz,
                                                const uint8_t *tx_data,
                                                size_t tx_len,
                                                uint8_t *rx_data,
                                                size_t rx_len,
                                                int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = with_temp_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_master_transmit_receive(dev, tx_data, tx_len, rx_data, rx_len, timeout_ms);
    i2c_master_bus_rm_device(dev);
    return ret;
}
