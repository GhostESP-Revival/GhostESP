/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * SCCB (I2C like) driver.
 * Updated for ESP-IDF v6.0 new I2C master driver.
 */
#include <stdbool.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sccb.h"
#include "sensor.h"
#include <stdio.h>
#include "sdkconfig.h"
#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#else
#include "esp_log.h"
static const char* TAG = "sccb";
#endif

#define LITTLETOBIG(x)          ((x<<8)|(x>>8))

#include "driver/i2c_master.h"

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#define SCCB_FREQ               CONFIG_SCCB_CLK_FREQ

static i2c_master_bus_handle_t sccb_i2c_bus;
static bool sccb_owns_i2c_port;

int SCCB_Init(int pin_sda, int pin_scl)
{
    ESP_LOGI(TAG, "pin_sda %d pin_scl %d", pin_sda, pin_scl);

    sccb_owns_i2c_port = true;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CONFIG_SCCB_HARDWARE_I2C_PORT1 ? 1 : 0,
        .sda_io_num = pin_sda,
        .scl_io_num = pin_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &sccb_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "sccb_i2c_port=%d", bus_config.i2c_port);
    return ESP_OK;
}

int SCCB_Use_Port(int i2c_num)
{
    return ESP_ERR_NOT_SUPPORTED;
}

int SCCB_Deinit(void)
{
    if (!sccb_owns_i2c_port || sccb_i2c_bus == NULL) {
        return ESP_OK;
    }
    sccb_owns_i2c_port = false;
    esp_err_t ret = i2c_del_master_bus(sccb_i2c_bus);
    sccb_i2c_bus = NULL;
    return ret;
}

uint8_t SCCB_Probe(void)
{
    uint8_t slave_addr = 0x0;
    uint8_t dummy = 0;

    for (size_t i = 0; i < CAMERA_MODEL_MAX; i++) {
        if (slave_addr == camera_sensor[i].sccb_addr) {
            continue;
        }
        slave_addr = camera_sensor[i].sccb_addr;

        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = slave_addr,
            .scl_speed_hz = SCCB_FREQ,
        };
        i2c_master_dev_handle_t dev_handle;
        esp_err_t ret = i2c_master_bus_add_device(sccb_i2c_bus, &dev_config, &dev_handle);
        if (ret != ESP_OK) {
            continue;
        }
        ret = i2c_master_transmit(dev_handle, &dummy, 1, 100);
        i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            return slave_addr;
        }
    }
    return 0;
}

static esp_err_t sccb_read_reg(uint8_t slv_addr, const uint8_t *write_buf, size_t write_len, uint8_t *data, size_t read_len)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = slv_addr,
        .scl_speed_hz = SCCB_FREQ,
    };
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(sccb_i2c_bus, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_master_transmit_receive(dev_handle, write_buf, write_len, data, read_len, 1000 / portTICK_RATE_MS);
    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

static esp_err_t sccb_write_reg(uint8_t slv_addr, const uint8_t *write_buf, size_t write_len)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = slv_addr,
        .scl_speed_hz = SCCB_FREQ,
    };
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(sccb_i2c_bus, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2c_master_transmit(dev_handle, write_buf, write_len, 1000 / portTICK_RATE_MS);
    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

uint8_t SCCB_Read(uint8_t slv_addr, uint8_t reg)
{
    uint8_t data = 0;
    esp_err_t ret = sccb_read_reg(slv_addr, &reg, 1, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Read Failed addr:0x%02x, reg:0x%02x, data:0x%02x, ret:%d", slv_addr, reg, data, ret);
    }
    return data;
}

int SCCB_Write(uint8_t slv_addr, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    esp_err_t ret = sccb_write_reg(slv_addr, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Write Failed addr:0x%02x, reg:0x%02x, data:0x%02x, ret:%d", slv_addr, reg, data, ret);
    }
    return ret == ESP_OK ? 0 : -1;
}

uint8_t SCCB_Read16(uint8_t slv_addr, uint16_t reg)
{
    uint8_t data = 0;
    uint16_t reg_htons = LITTLETOBIG(reg);
    esp_err_t ret = sccb_read_reg(slv_addr, (uint8_t *)&reg_htons, 2, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W [%04x]=%02x fail\n", reg, data);
    }
    return data;
}

int SCCB_Write16(uint8_t slv_addr, uint16_t reg, uint8_t data)
{
    uint16_t reg_htons = LITTLETOBIG(reg);
    uint8_t buf[3] = { ((uint8_t *)&reg_htons)[0], ((uint8_t *)&reg_htons)[1], data };
    esp_err_t ret = sccb_write_reg(slv_addr, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W [%04x]=%02x fail\n", reg, data);
    }
    return ret == ESP_OK ? 0 : -1;
}

uint16_t SCCB_Read_Addr16_Val16(uint8_t slv_addr, uint16_t reg)
{
    uint16_t data = 0;
    uint16_t reg_htons = LITTLETOBIG(reg);
    esp_err_t ret = sccb_read_reg(slv_addr, (uint8_t *)&reg_htons, 2, (uint8_t *)&data, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W [%04x]=%04x fail\n", reg, data);
    }
    uint8_t *data_u8 = (uint8_t *)&data;
    uint8_t tmp = data_u8[0];
    data_u8[0] = data_u8[1];
    data_u8[1] = tmp;
    return data;
}

int SCCB_Write_Addr16_Val16(uint8_t slv_addr, uint16_t reg, uint16_t data)
{
    uint16_t reg_htons = LITTLETOBIG(reg);
    uint16_t data_htons = LITTLETOBIG(data);
    uint8_t buf[4];
    memcpy(buf, &reg_htons, 2);
    memcpy(buf + 2, &data_htons, 2);
    esp_err_t ret = sccb_write_reg(slv_addr, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W [%04x]=%04x fail\n", reg, data);
    }
    return ret == ESP_OK ? 0 : -1;
}
