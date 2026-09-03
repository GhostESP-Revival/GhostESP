/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_TRANSPORT_CONFIG_H__
#define __ESP_HOSTED_TRANSPORT_CONFIG_H__

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
	ESP_TRANSPORT_OK = ESP_OK,
	ESP_TRANSPORT_ERR_INVALID_ARG = ESP_ERR_INVALID_ARG,
	ESP_TRANSPORT_ERR_ALREADY_SET = ESP_ERR_NOT_ALLOWED,
	ESP_TRANSPORT_ERR_INVALID_STATE = ESP_ERR_INVALID_STATE,
} esp_hosted_transport_err_t;

/* GPIO pin configuration structure */
typedef struct {
	void *port;
	int pin;
} gpio_pin_t;

/* New Configuration Structures */
struct esp_hosted_sdio_config {
	uint32_t clock_freq_khz;
	uint8_t bus_width;
	uint8_t slot;
	gpio_pin_t pin_clk;
	gpio_pin_t pin_cmd;
	gpio_pin_t pin_d0;
	gpio_pin_t pin_d1;
	gpio_pin_t pin_d2;
	gpio_pin_t pin_d3;
	gpio_pin_t pin_reset;
	uint8_t rx_mode;
	bool block_mode;
	bool iomux_enable;
	uint16_t tx_queue_size;
	uint16_t rx_queue_size;
};

struct esp_hosted_spi_hd_config {
	/* Number of lines used */
	uint8_t num_data_lines;

	/* SPI HD pins */
	gpio_pin_t pin_cs;
	gpio_pin_t pin_clk;
	gpio_pin_t pin_data_ready;
	gpio_pin_t pin_d0;
	gpio_pin_t pin_d1;
	gpio_pin_t pin_d2;
	gpio_pin_t pin_d3;
	gpio_pin_t pin_reset;

	/* SPI HD configuration */
	uint32_t clk_mhz;
	uint8_t mode;
	uint16_t tx_queue_size;
	uint16_t rx_queue_size;
	bool checksum_enable;
	uint8_t num_command_bits;
	uint8_t num_address_bits;
	uint8_t num_dummy_bits;
};

struct esp_hosted_spi_config {
	/* SPI Full Duplex pins */
	gpio_pin_t pin_mosi;
	gpio_pin_t pin_miso;
	gpio_pin_t pin_sclk;
	gpio_pin_t pin_cs;
	gpio_pin_t pin_handshake;
	gpio_pin_t pin_data_ready;
	gpio_pin_t pin_reset;

	/* SPI Full Duplex configuration */
	uint16_t tx_queue_size;
	uint16_t rx_queue_size;
	uint8_t mode;
	uint32_t clk_mhz;
};

struct esp_hosted_uart_config {
	/* UART bus number */
	uint8_t port;

	/* UART pins */
	gpio_pin_t pin_tx;
	gpio_pin_t pin_rx;
	gpio_pin_t pin_reset;

	/* UART configuration */
	uint8_t num_data_bits;
	uint8_t parity;
	uint8_t stop_bits;
	uint8_t flow_ctrl;
	uint8_t clk_src;
	bool checksum_enable;
	uint32_t baud_rate;
	uint16_t tx_queue_size;
	uint16_t rx_queue_size;
};

struct esp_hosted_transport_config {
	uint8_t transport_in_use;
	union {
		struct esp_hosted_sdio_config sdio;
		struct esp_hosted_spi_hd_config spi_hd;
		struct esp_hosted_spi_config spi;
		struct esp_hosted_uart_config uart;
	} u;
};

/* Default configuration functions - implemented by port layer */
struct esp_hosted_sdio_config esp_hosted_get_default_sdio_config(void);
struct esp_hosted_sdio_config esp_hosted_get_default_sdio_iomux_config(void);
struct esp_hosted_spi_hd_config esp_hosted_get_default_spi_hd_config(void);
struct esp_hosted_spi_config esp_hosted_get_default_spi_config(void);
struct esp_hosted_uart_config esp_hosted_get_default_uart_config(void);

/* Convenience macros for backward compatibility and ease of use */
#define INIT_DEFAULT_HOST_SDIO_CONFIG() esp_hosted_get_default_sdio_config()
#define INIT_DEFAULT_HOST_SDIO_IOMUX_CONFIG() esp_hosted_get_default_sdio_iomux_config()
#define INIT_DEFAULT_HOST_SPI_HD_CONFIG() esp_hosted_get_default_spi_hd_config()
#define INIT_DEFAULT_HOST_SPI_CONFIG() esp_hosted_get_default_spi_config()
#define INIT_DEFAULT_HOST_UART_CONFIG() esp_hosted_get_default_uart_config()

/***
 * Generic Transport APIs
 ***/
esp_err_t esp_hosted_set_default_config(void);
bool esp_hosted_is_config_valid(void);

/* Configuration get/set functions */
esp_hosted_transport_err_t esp_hosted_transport_set_default_config(void);
esp_hosted_transport_err_t esp_hosted_transport_get_config(struct esp_hosted_transport_config **config);
esp_hosted_transport_err_t esp_hosted_transport_get_reset_config(gpio_pin_t *pin_config);

bool esp_hosted_transport_is_config_valid(void);

/***
 * Transport dependent APIs.
 * Can only be used with the configured host transport
 ***/
/* SDIO functions */
esp_hosted_transport_err_t esp_hosted_sdio_get_config(struct esp_hosted_sdio_config **config);
esp_hosted_transport_err_t esp_hosted_sdio_set_config(struct esp_hosted_sdio_config *config) __attribute__((warn_unused_result));

esp_hosted_transport_err_t esp_hosted_sdio_iomux_set_config(struct esp_hosted_sdio_config *config) __attribute__((warn_unused_result));

/* SPI Half Duplex functions */
esp_hosted_transport_err_t esp_hosted_spi_hd_get_config(struct esp_hosted_spi_hd_config **config);
esp_hosted_transport_err_t esp_hosted_spi_hd_set_config(struct esp_hosted_spi_hd_config *config) __attribute__((warn_unused_result));

esp_hosted_transport_err_t esp_hosted_spi_hd_2lines_get_config(struct esp_hosted_spi_hd_config **config);
esp_hosted_transport_err_t esp_hosted_spi_hd_2lines_set_config(struct esp_hosted_spi_hd_config *config) __attribute__((warn_unused_result));

/* SPI Full Duplex functions */
esp_hosted_transport_err_t esp_hosted_spi_get_config(struct esp_hosted_spi_config **config);
esp_hosted_transport_err_t esp_hosted_spi_set_config(struct esp_hosted_spi_config *config) __attribute__((warn_unused_result));

/* UART functions */
esp_hosted_transport_err_t esp_hosted_uart_get_config(struct esp_hosted_uart_config **config);
esp_hosted_transport_err_t esp_hosted_uart_set_config(struct esp_hosted_uart_config *config) __attribute__((warn_unused_result));

#endif /* __ESP_HOSTED_TRANSPORT_CONFIG_H__ */
