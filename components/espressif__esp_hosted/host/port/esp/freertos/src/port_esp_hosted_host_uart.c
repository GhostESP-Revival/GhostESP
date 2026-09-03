/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "transport_drv.h"
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_uart.h"

#include "esp_log.h"
static const char TAG[] = "uart_wrapper";

#if H_UART_START_BITS != 1
#error "UART Start Bits must be 1 to communicate with ESP co-processor"
#endif

#if H_UART_FLOWCTRL
#error "UART Flow Control must be disabled to communicate with ESP co-processor"
#endif

#if CONFIG_ESP_CONSOLE_UART
#if CONFIG_ESP_CONSOLE_UART_NUM == H_UART_PORT
#error "ESP Console UART and Hosted UART are the same. Select another UART port."
#endif
#endif

#define UART_FAIL_IF_NULL_CTX(x) do { \
		if (!x) return ESP_FAIL;      \
	} while (0);

// UART context structure
typedef struct uart_ctx_t {
	int uart_port;
} uart_ctx_t;

static uart_ctx_t * ctx = NULL;

int hosted_uart_read(void * ctx, uint8_t *data, uint16_t size)
{
	uart_ctx_t * pctx;

	UART_FAIL_IF_NULL_CTX(ctx);

	pctx = (uart_ctx_t *)ctx;

	return uart_read_bytes(pctx->uart_port, data, size, portMAX_DELAY);
}

int hosted_uart_write(void * ctx, uint8_t *data, uint16_t size)
{
	uart_ctx_t * pctx;

	UART_FAIL_IF_NULL_CTX(ctx);

	pctx = (uart_ctx_t *)ctx;

	return uart_write_bytes(pctx->uart_port, (const char*)data, size);
}

int hosted_uart_flush_input(void * ctx)
{
	uart_ctx_t * pctx;

	UART_FAIL_IF_NULL_CTX(ctx);

	pctx = (uart_ctx_t *)ctx;

	return uart_flush_input(pctx->uart_port);
}

void * hosted_uart_init(void)
{
	ctx = (uart_ctx_t*)g_h.funcs->_h_malloc(sizeof(uart_ctx_t));
	assert(ctx);

	// initialise bus and device in ctx
	const uart_config_t uart_config = {
		.baud_rate = H_UART_BAUD_RATE,
		.data_bits = H_UART_NUM_DATA_BITS,
		.parity = H_UART_PARITY,
		.stop_bits = H_UART_STOP_BITS,
		.flow_ctrl = H_UART_FLOWCTRL,
		.source_clk = H_UART_CLK_SRC,
	};

	ESP_ERROR_CHECK(uart_driver_install(H_UART_PORT, MAX_UART_BUFFER_SIZE, MAX_UART_BUFFER_SIZE,
			0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(H_UART_PORT, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(H_UART_PORT, H_UART_PIN_TX, H_UART_PIN_RX,
			UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

	ESP_LOGI(TAG, "UART GPIOs: Tx: %"PRIu16 ", Rx: %"PRIu16 ", Baud Rate %i",
			H_UART_PIN_TX, H_UART_PIN_RX, H_UART_BAUD_RATE);

	ctx->uart_port = H_UART_PORT;

	return ctx;
}

esp_err_t hosted_uart_deinit(void *ctx)
{
	esp_err_t ret;
	uart_ctx_t * pctx;

	UART_FAIL_IF_NULL_CTX(ctx);

	pctx = (uart_ctx_t *)ctx;

	ret = uart_flush_input(pctx->uart_port);
	if (ret != ESP_OK)
		ESP_LOGE(TAG, "%s: Failed to flush uart Rx", __func__);
	ret = uart_wait_tx_done(pctx->uart_port, 100); // wait 100 RTOS ticks for Tx to be empty
	if (ret != ESP_OK)
		ESP_LOGE(TAG, "%s: Failed to flush uart Tx", __func__);
	uart_driver_delete(pctx->uart_port);

	g_h.funcs->_h_free(ctx);

	return ESP_OK;
}
