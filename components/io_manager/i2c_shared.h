#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max distinct (addr, scl_speed_hz) device handles cached per bus. */
#define I2C_SHARED_MAX_CACHED_DEVICES 8

esp_err_t i2c_shared_get_or_create_bus(i2c_port_num_t port,
                                       gpio_num_t sda,
                                       gpio_num_t scl,
                                       bool enable_internal_pullup,
                                       i2c_master_bus_handle_t *out_bus,
                                       bool *out_created);

/* Remove a shared bus and cached devices so another peripheral can temporarily
 * claim the same GPIOs. The bus can be recreated afterward. */
esp_err_t i2c_shared_release_bus(i2c_port_num_t port);

/* Create a persistent device handle. The caller owns it and is responsible
 * for removing it (e.g. at deinit). Not cached. */
esp_err_t i2c_shared_add_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev);

/* Get a cached persistent device handle keyed by (addr, scl_speed_hz).
 * Handles are created on first use and kept forever; they are shared by the
 * *_from_addr helpers below and by any caller that keeps the returned handle.
 * Never remove a cached handle with i2c_master_bus_rm_device(). */
esp_err_t i2c_shared_get_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev);

/* Take/release the per-port shared bus lock (recursive). All i2c_shared
 * users and the IO manager take this lock, so wrapping a multi-transaction
 * sequence (e.g. a touch sample) makes it atomic w.r.t. the rest of the bus
 * traffic. Returns false if the lock could not be acquired in time. */
bool i2c_shared_bus_lock(i2c_master_bus_handle_t bus, int timeout_ms);
void i2c_shared_bus_unlock(i2c_master_bus_handle_t bus);

/* Transaction helpers: internally take the shared bus lock, use a cached
 * persistent device handle (no add/remove churn), then release the lock.
 * They are safe to call from multiple tasks on the same bus. */
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
