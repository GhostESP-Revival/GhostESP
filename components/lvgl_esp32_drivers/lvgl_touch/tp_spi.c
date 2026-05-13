/**
 * @file tp_spi.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "tp_spi.h"
#include "touch_driver.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <string.h>

#include "../lvgl_helpers.h"
#include "../lvgl_spi_conf.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/


/**********************
 *  STATIC PROTOTYPES
 **********************/
static spi_device_handle_t spi;
static bool tp_spi_use_nm_cyd_c5_xpt2046_path(void);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void tp_spi_add_device_config(spi_host_device_t host, spi_device_interface_config_t *devcfg)
{
	esp_err_t ret=spi_bus_add_device(host, devcfg, &spi);
	assert(ret==ESP_OK);
}

void tp_spi_add_device(spi_host_device_t host)
{
	bool use_nm_cyd_c5_path = tp_spi_use_nm_cyd_c5_xpt2046_path();
	spi_device_interface_config_t devcfg={
                .clock_speed_hz = SPI_TOUCH_CLOCK_SPEED_HZ,
                .mode = SPI_TOUCH_SPI_MODE,
		.spics_io_num=TP_SPI_CS,               //CS pin
		.queue_size=1,
		.pre_cb=NULL,
		.post_cb=NULL,
		.command_bits = 8,
		.address_bits = 0,
		.dummy_bits = 0,
		.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
	};
	if (use_nm_cyd_c5_path) {
		devcfg.command_bits = 0;
		devcfg.address_bits = 0;
		devcfg.dummy_bits = 0;
		devcfg.flags = 0;
	}
	
	//Attach the Touch controller to the SPI bus
	tp_spi_add_device_config(host, &devcfg);
}

void tp_spi_xchg(uint8_t* data_send, uint8_t* data_recv, uint8_t byte_count)
{
	spi_transaction_t t = {
		.length = byte_count * 8, // SPI transaction length is in bits
		.tx_buffer = data_send,
		.rx_buffer = data_recv};
	
	esp_err_t ret = spi_device_transmit(spi, &t);
	assert(ret == ESP_OK);
}

void tp_spi_write_reg(uint8_t* data, uint8_t byte_count)
{
	spi_transaction_t t = {
	    .length = byte_count * 8,
	    .tx_buffer = data,
	    .flags = 0
	};
	
	esp_err_t ret = spi_device_transmit(spi, &t);
	assert(ret == ESP_OK);
}

void tp_spi_read_reg(uint8_t reg, uint8_t* data, uint8_t byte_count)
{
	if (tp_spi_use_nm_cyd_c5_xpt2046_path()) {
	uint8_t tx[3] = {reg, 0, 0};
	uint8_t rx[3] = {0};
	spi_transaction_t t = {
	    .length = 24,
	    .tx_buffer = tx,
	    .rx_buffer = rx,
	    .flags = 0
	};

	esp_err_t ret = spi_device_transmit(spi, &t);
	assert(ret == ESP_OK);

	if (byte_count > 0) data[0] = rx[1];
	if (byte_count > 1) data[1] = rx[2];
	} else {
	spi_transaction_t t = {	
	    .length = 0,
	    .rxlength = byte_count * 8,
	    .cmd = reg,
	    .rx_buffer = data,
	    .flags = 0
	};
	
	// Read - send first byte as command
	esp_err_t ret = spi_device_transmit(spi, &t);
	assert(ret == ESP_OK);
	}
}

static bool tp_spi_use_nm_cyd_c5_xpt2046_path(void)
{
#if defined(CONFIG_LV_TOUCH_CONTROLLER_XPT2046) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
	return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0;
#else
	return false;
#endif
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
