/*

SPDX-License-Identifier: MIT

MIT License

Copyright (c) 2021 Rop Gonggrijp. Based on esp_i2c_helper by Mika Tuupola.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <esp_log.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <driver/i2c_master.h>

#include "sdkconfig.h"

#include "i2c_manager.h"
#include "i2c_shared.h"


#if defined __has_include
#if __has_include ("esp_idf_version.h")
		#include "esp_idf_version.h"
		#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 3, 0)
			#define HAS_CLK_FLAGS
		#endif
	#endif
#endif


static const char* TAG = I2C_TAG;

static SemaphoreHandle_t I2C_FN(_local_mutex)[2] = { NULL, NULL };
static SemaphoreHandle_t* I2C_FN(_mutex) = &I2C_FN(_local_mutex)[0];
static i2c_master_bus_handle_t s_i2c_bus[2] = { NULL, NULL };
static bool s_i2c_bus_owned[2] = { false, false };

#if defined (CONFIG_I2C_MANAGER_0_ENABLED)
#define I2C_ZERO 					I2C_NUM_0
	#if defined (CONFIG_I2C_MANAGER_0_PULLUPS)
		#define I2C_MANAGER_0_PULLUPS 	true
	#else
		#define I2C_MANAGER_0_PULLUPS 	false
	#endif

	#define I2C_MANAGER_0_TIMEOUT 		( pdMS_TO_TICKS( CONFIG_I2C_MANAGER_0_TIMEOUT ) )
	#define I2C_MANAGER_0_LOCK_TIMEOUT	( ( pdMS_TO_TICKS( CONFIG_I2C_MANAGER_0_LOCK_TIMEOUT ) )
#endif


#if defined (CONFIG_I2C_MANAGER_1_ENABLED)
#define I2C_ONE 					I2C_NUM_1
	#if defined (CONFIG_I2C_MANAGER_1_PULLUPS)
		#define I2C_MANAGER_1_PULLUPS 	true
	#else
		#define I2C_MANAGER_1_PULLUPS 	false
	#endif

	#define I2C_MANAGER_1_TIMEOUT 		( pdMS_TO_TICKS( CONFIG_I2C_MANAGER_1_TIMEOUT ) )
	#define I2C_MANAGER_1_LOCK_TIMEOUT	( pdMS_TO_TICKS( CONFIG_I2C_MANAGER_1_LOCK_TIMEOUT ) )
#endif

#define ERROR_PORT(port, fail) { \
	ESP_LOGE(TAG, "Invalid port or not configured for I2C Manager: %d", (int)port); \
	return fail; \
}

#if defined(I2C_ZERO) && defined (I2C_ONE)
#define I2C_PORT_CHECK(port, fail) \
		if (port != I2C_NUM_0 && port != I2C_NUM_1) ERROR_PORT(port, fail);
#else
#if defined(I2C_ZERO)
#define I2C_PORT_CHECK(port, fail) \
			if (port != I2C_NUM_0) ERROR_PORT(port, fail);
#elif defined(I2C_ONE)
#define I2C_PORT_CHECK(port, fail) \
			if (port != I2C_NUM_1) ERROR_PORT(port, fail);
#else
#define I2C_PORT_CHECK(port, fail) \
			ERROR_PORT(port, fail);
#endif
#endif

static uint32_t i2c_port_speed(i2c_port_num_t port) {
#if defined (I2C_ZERO)
    if (port == I2C_NUM_0) {
        return CONFIG_I2C_MANAGER_0_FREQ_HZ;
    }
#endif
#if defined (I2C_ONE)
    if (port == I2C_NUM_1) {
        return CONFIG_I2C_MANAGER_1_FREQ_HZ;
    }
#endif
    return 100000;
}

static esp_err_t i2c_manager_get_or_create_bus(i2c_port_num_t port)
{
    if (s_i2c_bus[port]) {
        return ESP_OK;
    }

    gpio_num_t sda = GPIO_NUM_NC;
    gpio_num_t scl = GPIO_NUM_NC;
    bool pullup = true;

#if defined (I2C_ZERO)
    if (port == I2C_NUM_0) {
        sda = CONFIG_I2C_MANAGER_0_SDA;
        scl = CONFIG_I2C_MANAGER_0_SCL;
        pullup = I2C_MANAGER_0_PULLUPS;
    }
#endif
#if defined (I2C_ONE)
    if (port == I2C_NUM_1) {
        sda = CONFIG_I2C_MANAGER_1_SDA;
        scl = CONFIG_I2C_MANAGER_1_SCL;
        pullup = I2C_MANAGER_1_PULLUPS;
    }
#endif

    return i2c_shared_get_or_create_bus(port, sda, scl, pullup, &s_i2c_bus[port], &s_i2c_bus_owned[port]);
}

static esp_err_t i2c_manager_add_device(i2c_port_num_t port, uint16_t addr, i2c_master_dev_handle_t *out_dev)
{
    if (!s_i2c_bus[port] || !out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_addr_bit_len_t addr_len = I2C_ADDR_BIT_LEN_7;
    uint16_t device_addr = addr;

#ifdef I2C_ADDR_BIT_LEN_10
    if (addr & I2C_ADDR_10) {
        addr_len = I2C_ADDR_BIT_LEN_10;
        device_addr = addr & 0x3FF;
    }
#endif

    i2c_device_config_t dev_config = {
        .dev_addr_length = addr_len,
        .device_address = device_addr,
        .scl_speed_hz = i2c_port_speed(port),
        .scl_wait_us = 0,
    };

    return i2c_master_bus_add_device(s_i2c_bus[port], &dev_config, out_dev);
}

esp_err_t I2C_FN(_init)(i2c_port_num_t port) {

    I2C_PORT_CHECK(port, ESP_FAIL);

    esp_err_t ret = ESP_OK;

    if (I2C_FN(_mutex)[port] == 0) {

        ESP_LOGI(TAG, "Starting I2C master at port %d.", (int)port);

        I2C_FN(_mutex)[port] = xSemaphoreCreateMutex();

        ret = i2c_manager_get_or_create_bus(port);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialise I2C port %d.", (int)port);
        } else {
            ESP_LOGI(TAG, "Initialised I2C master bus on port %d", (int)port);
        }

    }

    return ret;
}

esp_err_t I2C_FN(_read)(i2c_port_num_t port, uint16_t addr, uint32_t reg, uint8_t *buffer, uint16_t size) {

    I2C_PORT_CHECK(port, ESP_FAIL);

    esp_err_t result;

    // May seem weird, but init starts with a check if it's needed, no need for that check twice.
    I2C_FN(_init)(port);

    ESP_LOGV(TAG, "Reading port %d, addr 0x%03x, reg 0x%04lx", port, addr, reg);

    TickType_t timeout = 0;
#if defined (I2C_ZERO)
    if (port == I2C_NUM_0) {
			timeout = I2C_MANAGER_0_TIMEOUT;
		}
#endif
#if defined (I2C_ONE)
    if (port == I2C_NUM_1) {
			timeout = I2C_MANAGER_1_TIMEOUT;
		}
#endif

    if (I2C_FN(_lock)((int)port) == ESP_OK) {
        i2c_master_dev_handle_t dev = NULL;
        result = i2c_manager_add_device(port, addr, &dev);
        if (result != ESP_OK) {
            I2C_FN(_unlock)((int)port);
            return result;
        }

        if (!(reg & I2C_NO_REG)) {
            uint8_t reg_buf[2];
            size_t reg_len = 1;
            if (reg & I2C_REG_16) {
                reg_buf[0] = (reg & 0xFF00) >> 8;
                reg_buf[1] = reg & 0xFF;
                reg_len = 2;
            } else {
                reg_buf[0] = reg & 0xFF;
            }
            result = i2c_master_transmit_receive(dev, reg_buf, reg_len, buffer, size, timeout);
        } else {
            result = i2c_master_receive(dev, buffer, size, timeout);
        }
        i2c_master_bus_rm_device(dev);
        I2C_FN(_unlock)((int)port);
    } else {
        ESP_LOGE(TAG, "Lock could not be obtained for port %d.", (int)port);
        return ESP_ERR_TIMEOUT;
    }

    if (result != ESP_OK) {
        ESP_LOGD(TAG, "Error: %d", result);
    }

    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, size, ESP_LOG_VERBOSE);

    return result;
}

esp_err_t I2C_FN(_write)(i2c_port_num_t port, uint16_t addr, uint32_t reg, const uint8_t *buffer, uint16_t size) {

    I2C_PORT_CHECK(port, ESP_FAIL);

    esp_err_t result;

    // May seem weird, but init starts with a check if it's needed, no need for that check twice.
    I2C_FN(_init)(port);

    ESP_LOGV(TAG, "Writing port %d, addr 0x%03x, reg 0x%04lx", port, addr, reg);

    TickType_t timeout = 0;
#if defined (I2C_ZERO)
    if (port == I2C_NUM_0) {
			timeout = pdMS_TO_TICKS( CONFIG_I2C_MANAGER_0_TIMEOUT );
		}
#endif
#if defined (I2C_ONE)
    if (port == I2C_NUM_1) {
			timeout = pdMS_TO_TICKS( CONFIG_I2C_MANAGER_1_TIMEOUT );
		}
#endif

    if (I2C_FN(_lock)((int)port) == ESP_OK) {
        i2c_master_dev_handle_t dev = NULL;
        result = i2c_manager_add_device(port, addr, &dev);
        if (result != ESP_OK) {
            I2C_FN(_unlock)((int)port);
            return result;
        }
        uint8_t tx[258];
        size_t tx_len = 0;
        if (!(reg & I2C_NO_REG)) {
            if (reg & I2C_REG_16) {
                tx[tx_len++] = (reg & 0xFF00) >> 8;
            }
            tx[tx_len++] = reg & 0xFF;
        }
        if (size > 0) {
            memcpy(&tx[tx_len], buffer, size);
            tx_len += size;
        }
        result = i2c_master_transmit(dev, tx, tx_len, timeout);
        i2c_master_bus_rm_device(dev);
        I2C_FN(_unlock)((int)port);
    } else {
        ESP_LOGE(TAG, "Lock could not be obtained for port %d.", (int)port);
        return ESP_ERR_TIMEOUT;
    }

    if (result != ESP_OK) {
        ESP_LOGD(TAG, "Error: %d", result);
    }

    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, size, ESP_LOG_VERBOSE);

    return result;
}

esp_err_t I2C_FN(_close)(i2c_port_num_t port) {
    I2C_PORT_CHECK(port, ESP_FAIL);
    vSemaphoreDelete(I2C_FN(_mutex)[port]);
    I2C_FN(_mutex)[port] = NULL;
    ESP_LOGI(TAG, "Closing I2C master at port %d", port);
    if (s_i2c_bus_owned[port] && s_i2c_bus[port]) {
        esp_err_t ret = i2c_del_master_bus(s_i2c_bus[port]);
        s_i2c_bus[port] = NULL;
        s_i2c_bus_owned[port] = false;
        return ret;
    }
    s_i2c_bus[port] = NULL;
    s_i2c_bus_owned[port] = false;
    return ESP_OK;
}

esp_err_t I2C_FN(_lock)(i2c_port_num_t port) {
    I2C_PORT_CHECK(port, ESP_FAIL);
    ESP_LOGV(TAG, "Mutex lock set for %d.", (int)port);

    TickType_t timeout;
#if defined (I2C_ZERO)
    if (port == I2C_NUM_0) {
			timeout = pdMS_TO_TICKS( CONFIG_I2C_MANAGER_0_LOCK_TIMEOUT );
		}
#endif
#if defined (I2C_ONE)
    if (port == I2C_NUM_1) {
			timeout = pdMS_TO_TICKS( CONFIG_I2C_MANAGER_1_LOCK_TIMEOUT );
		}
#endif

    if (xSemaphoreTake(I2C_FN(_mutex)[port], timeout) == pdTRUE) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Removing stale mutex lock from port %d.", (int)port);
        I2C_FN(_force_unlock)(port);
        return (xSemaphoreTake(I2C_FN(_mutex)[port], timeout) == pdTRUE ? ESP_OK : ESP_FAIL);
    }
}

esp_err_t I2C_FN(_unlock)(i2c_port_num_t port) {
    I2C_PORT_CHECK(port, ESP_FAIL);
    ESP_LOGV(TAG, "Mutex lock removed for %d.", (int)port);
    return (xSemaphoreGive(I2C_FN(_mutex)[port]) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t I2C_FN(_force_unlock)(i2c_port_num_t port) {
    I2C_PORT_CHECK(port, ESP_FAIL);
    if (I2C_FN(_mutex)[port]) {
        vSemaphoreDelete(I2C_FN(_mutex)[port]);
    }
    I2C_FN(_mutex)[port] = xSemaphoreCreateMutex();
    return ESP_OK;
}




#ifdef I2C_OEM

void I2C_FN(_locking)(void* leader) {
    if (leader) {
        ESP_LOGI(TAG, "Now following I2C Manager for locking");
        I2C_FN(_mutex) = (SemaphoreHandle_t*)leader;
    }
}

#else

void* i2c_manager_locking() {
        return (void*)i2c_manager_mutex;
    }

    int32_t i2c_hal_read(void *handle, uint8_t address, uint8_t reg, uint8_t *buffer, uint16_t size) {
        return i2c_manager_read(*(i2c_port_num_t*)handle, address, reg, buffer, size);
    }

    int32_t i2c_hal_write(void *handle, uint8_t address, uint8_t reg, const uint8_t *buffer, uint16_t size) {
        return i2c_manager_write(*(i2c_port_num_t*)handle, address, reg, buffer, size);
    }

	static i2c_port_num_t port_zero = (i2c_port_num_t)0;
	static i2c_port_num_t port_one = (i2c_port_num_t)1;

    static i2c_hal_t _i2c_hal[2] = {
        {&i2c_hal_read, &i2c_hal_write, &port_zero},
        {&i2c_hal_read, &i2c_hal_write, &port_one}
    };

    void* i2c_hal(i2c_port_num_t port) {
		I2C_PORT_CHECK(port, NULL);
        return (void*)&_i2c_hal[port];
    }

#endif
