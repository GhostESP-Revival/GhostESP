/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#include <string.h>
#include "esp_log.h"

#include "port_esp_hosted_host_config.h"
#include "esp_hosted_transport_config.h"

static const char *TAG = "esp_hosted_transport_config";

/* Static configurations */
static struct esp_hosted_transport_config s_transport_config = { 0 };

/* Flags to track if configs were set */
static bool esp_hosted_transport_config_set;

bool esp_hosted_transport_is_config_valid(void) {
  return esp_hosted_transport_config_set;
}

esp_err_t esp_hosted_set_default_config(void) {
  return esp_hosted_transport_set_default_config();
}

bool esp_hosted_is_config_valid(void) {
  return esp_hosted_transport_is_config_valid();
}

esp_hosted_transport_err_t esp_hosted_transport_set_default_config(void)
{
	memset(&s_transport_config, 0, sizeof(s_transport_config));

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
	ESP_ERROR_CHECK(esp_hosted_sdio_set_config(NULL));
#elif H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD
	ESP_ERROR_CHECK(esp_hosted_spi_hd_set_config(NULL));
#elif H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
	ESP_ERROR_CHECK(esp_hosted_spi_set_config(NULL));
#elif H_TRANSPORT_IN_USE == H_TRANSPORT_UART
	ESP_ERROR_CHECK(esp_hosted_uart_set_config(NULL));
#else
	return ESP_TRANSPORT_ERR_INVALID_STATE;
#endif

	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_transport_get_config(struct esp_hosted_transport_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_transport_get_reset_config(gpio_pin_t *pin_config)
{
	if (!pin_config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}

	switch(s_transport_config.transport_in_use) {
	case H_TRANSPORT_SDIO:
		pin_config->port = s_transport_config.u.sdio.pin_reset.port;
		pin_config->pin  = s_transport_config.u.sdio.pin_reset.pin;
		break;
	case H_TRANSPORT_SPI_HD:
		pin_config->port = s_transport_config.u.spi_hd.pin_reset.port;
		pin_config->pin  = s_transport_config.u.spi_hd.pin_reset.pin;
		break;
	case H_TRANSPORT_SPI:
		pin_config->port = s_transport_config.u.spi.pin_reset.port;
		pin_config->pin  = s_transport_config.u.spi.pin_reset.pin;
		break;
	case H_TRANSPORT_UART:
		pin_config->port = s_transport_config.u.uart.pin_reset.port;
		pin_config->pin  = s_transport_config.u.uart.pin_reset.pin;
		break;
	case H_TRANSPORT_NONE: // drop through to default case
	default:
		// transport config not yet initialised. Use default Reset pin config
		pin_config->port = H_GPIO_PORT_RESET;
		pin_config->pin  = H_GPIO_PIN_RESET;
		break;
	}

	return ESP_TRANSPORT_OK;
}

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
/* SDIO functions */
esp_hosted_transport_err_t esp_hosted_sdio_get_config(struct esp_hosted_sdio_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config.u.sdio;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_sdio_set_config(struct esp_hosted_sdio_config *config)
{
	struct esp_hosted_sdio_config new_config = config ? *config : INIT_DEFAULT_HOST_SDIO_CONFIG();

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.sdio, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.sdio = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_SDIO;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_sdio_iomux_set_config(struct esp_hosted_sdio_config *config)
{
	struct esp_hosted_sdio_config new_config = config ? *config : INIT_DEFAULT_HOST_SDIO_IOMUX_CONFIG();
	new_config.iomux_enable = true;

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.sdio, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.sdio = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_SDIO;
	return ESP_TRANSPORT_OK;
}
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD
/* SPI Half Duplex functions */
esp_hosted_transport_err_t esp_hosted_spi_hd_get_config(struct esp_hosted_spi_hd_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config.u.spi_hd;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_spi_hd_set_config(struct esp_hosted_spi_hd_config *config)
{
	struct esp_hosted_spi_hd_config new_config = config ? *config : INIT_DEFAULT_HOST_SPI_HD_CONFIG();

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.spi_hd, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.spi_hd = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_SPI_HD;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_spi_hd_2lines_get_config(struct esp_hosted_spi_hd_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config.u.spi_hd;
	s_transport_config.u.spi_hd.num_data_lines = 2;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_spi_hd_2lines_set_config(struct esp_hosted_spi_hd_config *config)
{
	struct esp_hosted_spi_hd_config new_config = config ? *config : INIT_DEFAULT_HOST_SPI_HD_CONFIG();
	new_config.num_data_lines = 2;

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.spi_hd, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.spi_hd = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_SPI_HD;
	return ESP_TRANSPORT_OK;
}
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
/* SPI Full Duplex functions */
esp_hosted_transport_err_t esp_hosted_spi_get_config(struct esp_hosted_spi_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config.u.spi;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_spi_set_config(struct esp_hosted_spi_config *config)
{
	struct esp_hosted_spi_config new_config = config ? *config : INIT_DEFAULT_HOST_SPI_CONFIG();

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.spi, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.spi = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_SPI;
	return ESP_TRANSPORT_OK;
}
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_UART
/* UART functions */
esp_hosted_transport_err_t esp_hosted_uart_get_config(struct esp_hosted_uart_config **config)
{
	if (!config) {
		return ESP_TRANSPORT_ERR_INVALID_ARG;
	}
	*config = &s_transport_config.u.uart;
	return ESP_TRANSPORT_OK;
}

esp_hosted_transport_err_t esp_hosted_uart_set_config(struct esp_hosted_uart_config *config)
{
	struct esp_hosted_uart_config new_config = config ? *config : INIT_DEFAULT_HOST_UART_CONFIG();

	if (esp_hosted_transport_config_set) {
		if (memcmp(&s_transport_config.u.uart, &new_config, sizeof(new_config)) == 0) {
			return ESP_TRANSPORT_OK;
		}
		ESP_LOGE(TAG, "Transport already initialized (through constructor?), reconfiguring not allowed");
		return ESP_TRANSPORT_ERR_ALREADY_SET;
	}

	s_transport_config.u.uart = new_config;
	esp_hosted_transport_config_set = true;
	s_transport_config.transport_in_use = H_TRANSPORT_UART;
	return ESP_TRANSPORT_OK;
}
#endif
