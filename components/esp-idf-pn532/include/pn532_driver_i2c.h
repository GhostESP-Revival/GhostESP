#ifndef PN532_DRIVER_I2C_H
#define PN532_DRIVER_I2C_H

#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "pn532_driver.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t pn532_new_driver_i2c(gpio_num_t sda,
                                   gpio_num_t scl,
                                   gpio_num_t reset,
                                   gpio_num_t irq,
                                   i2c_port_num_t i2c_port_number,
                                   pn532_io_handle_t io_handle);

#ifdef __cplusplus
}
#endif

#endif // PN532_DRIVER_I2C_H


