#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_shared_get_or_create_bus(i2c_port_num_t port,
                                       gpio_num_t sda,
                                       gpio_num_t scl,
                                       bool enable_internal_pullup,
                                       i2c_master_bus_handle_t *out_bus,
                                       bool *out_created);

esp_err_t i2c_shared_add_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev);

esp_err_t i2c_shared_transmit_to_addr(i2c_master_bus_handle_t bus,
                                      uint16_t addr,
                                      uint32_t scl_speed_hz,
                                      const uint8_t *data,
                                      size_t len,
                                      int timeout_ms);

esp_err_t i2c_shared_receive_from_addr(i2c_master_bus_handle_t bus,
                                       uint16_t addr,
                                       uint32_t scl_speed_hz,
                                       uint8_t *data,
                                       size_t len,
                                       int timeout_ms);

esp_err_t i2c_shared_transmit_receive_from_addr(i2c_master_bus_handle_t bus,
                                                uint16_t addr,
                                                uint32_t scl_speed_hz,
                                                const uint8_t *tx_data,
                                                size_t tx_len,
                                                uint8_t *rx_data,
                                                size_t rx_len,
                                                int timeout_ms);

#ifdef __cplusplus
}
#endif
