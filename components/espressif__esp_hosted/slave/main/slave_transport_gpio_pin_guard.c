/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include "slave_transport_gpio_pin_guard.h"


static inline void add_pin(uint64_t *mask, int pin)
{
	if (pin >= 0 && pin < GPIO_NUM_MAX) {
		*mask |= (1ULL << pin);
	}
}

static uint64_t get_reserved_pin_mask(void)
{
	static bool initialized = false;
	static uint64_t mask = 0;

	if (initialized)
		return mask;

	initialized = true;

	/* ---------- SDIO ---------- */
#ifdef CONFIG_ESP_SDIO_HOST_INTERFACE
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_CMD);
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_CLK);
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_D0);
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_D1);
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_D2);
	add_pin(&mask, CONFIG_ESP_SDIO_PIN_D3);
#endif

	/* ---------- SPI (full duplex) ---------- */
#ifdef CONFIG_ESP_SPI_HOST_INTERFACE
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_MOSI);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_MISO);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_CLK);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_CS);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_HANDSHAKE);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_DATA_READY);
	add_pin(&mask, CONFIG_ESP_SPI_GPIO_RESET);
#endif

	/* ---------- SPI (half duplex) ---------- */
#ifdef CONFIG_ESP_SPI_HD_HOST_INTERFACE
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_CS);
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_CLK);
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_D0);
#if (CONFIG_ESP_SPI_HD_INTERFACE_NUM_DATA_LINES >= 2)
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_D1);
#if CONFIG_ESP_SPI_HD_INTERFACE_NUM_DATA_LINES == 4
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_D2);
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_D3);
#endif
#endif
#ifdef CONFIG_ESP_SPI_HD_DATA_READY_ENABLED
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_DATA_READY);
#endif
	add_pin(&mask, CONFIG_ESP_SPI_HD_GPIO_RESET);
#endif

	/* ---------- UART ---------- */
#ifdef CONFIG_ESP_UART_HOST_INTERFACE
	add_pin(&mask, CONFIG_ESP_UART_PIN_TX);
	add_pin(&mask, CONFIG_ESP_UART_PIN_RX);
	add_pin(&mask, CONFIG_ESP_UART_GPIO_RESET);
#endif

	return mask;
}

uint8_t transport_gpio_pin_guard_is_eligible(gpio_num_t pin)
{
	if (pin < 0 || pin >= GPIO_NUM_MAX) return false;
	uint64_t reserved = get_reserved_pin_mask();
	return ((reserved & (1ULL << pin)) == 0);
}
