/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_hosted_transport_config.h"
#include "port_esp_hosted_host_config.h"

#ifdef CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
struct esp_hosted_sdio_config esp_hosted_get_default_sdio_config(void)
{
    return (struct esp_hosted_sdio_config) {
        .clock_freq_khz = H_SDIO_CLOCK_FREQ_KHZ,
        .bus_width = H_SDIO_BUS_WIDTH,
        .slot = H_SDMMC_HOST_SLOT,
        .pin_clk = {.port = H_SDIO_PORT_CLK, .pin = H_SDIO_PIN_CLK},
        .pin_cmd = {.port = H_SDIO_PORT_CMD, .pin = H_SDIO_PIN_CMD},
        .pin_d0  = {.port = H_SDIO_PORT_D0,  .pin = H_SDIO_PIN_D0},
        .pin_d1  = {.port = H_SDIO_PORT_D1,  .pin = H_SDIO_PIN_D1},
        .pin_d2  = {.port = H_SDIO_PORT_D2,  .pin = H_SDIO_PIN_D2},
        .pin_d3  = {.port = H_SDIO_PORT_D3,  .pin = H_SDIO_PIN_D3},
        .pin_reset = {.port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET},
        .rx_mode = H_SDIO_HOST_RX_MODE,
        .block_mode = H_SDIO_TX_BLOCK_ONLY_XFER && H_SDIO_RX_BLOCK_ONLY_XFER,
        .iomux_enable = false,
        .tx_queue_size = H_SDIO_TX_Q,
        .rx_queue_size = H_SDIO_RX_Q,
    };
}


struct esp_hosted_sdio_config esp_hosted_get_default_sdio_iomux_config(void)
{
    return (struct esp_hosted_sdio_config) {
        .clock_freq_khz = H_SDIO_CLOCK_FREQ_KHZ,
        .bus_width = H_SDIO_BUS_WIDTH,
        .slot = H_SDMMC_HOST_SLOT,
        .rx_mode = H_SDIO_HOST_RX_MODE,
        .block_mode = H_SDIO_TX_BLOCK_ONLY_XFER && H_SDIO_RX_BLOCK_ONLY_XFER,
        .iomux_enable = true,
        .tx_queue_size = H_SDIO_TX_Q,
        .rx_queue_size = H_SDIO_RX_Q,
    };
}
#endif

#ifdef CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE
struct esp_hosted_spi_hd_config esp_hosted_get_default_spi_hd_config(void)
{
    return (struct esp_hosted_spi_hd_config) {
        .num_data_lines = H_SPI_HD_HOST_NUM_DATA_LINES,
        .pin_cs = {.port = H_SPI_HD_PORT_CS, .pin = H_SPI_HD_PIN_CS},
        .pin_clk = {.port = H_SPI_HD_PORT_CLK, .pin = H_SPI_HD_PIN_CLK},
        .pin_data_ready = {.port = H_SPI_HD_PORT_DATA_READY, .pin = H_SPI_HD_PIN_DATA_READY},
        .pin_d0 = {.port = H_SPI_HD_PORT_D0, .pin = H_SPI_HD_PIN_D0},
        .pin_d1 = {.port = H_SPI_HD_PORT_D1, .pin = H_SPI_HD_PIN_D1},
        .pin_d2 = {.port = H_SPI_HD_PORT_D2, .pin = H_SPI_HD_PIN_D2},
        .pin_d3 = {.port = H_SPI_HD_PORT_D3, .pin = H_SPI_HD_PIN_D3},
        .pin_reset = {.port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET},
        .clk_mhz = H_SPI_HD_CLK_MHZ,
        .mode = H_SPI_HD_MODE,
        .tx_queue_size = H_SPI_HD_TX_QUEUE_SIZE,
        .rx_queue_size = H_SPI_HD_RX_QUEUE_SIZE,
        .checksum_enable = H_SPI_HD_CHECKSUM,
        .num_command_bits = H_SPI_HD_NUM_COMMAND_BITS,
        .num_address_bits = H_SPI_HD_NUM_ADDRESS_BITS,
        .num_dummy_bits = H_SPI_HD_NUM_DUMMY_BITS,
    };
}
#endif

#ifdef CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE
struct esp_hosted_spi_config esp_hosted_get_default_spi_config(void)
{
    return (struct esp_hosted_spi_config) {
        .pin_mosi = {.port = H_GPIO_MOSI_Port, .pin = H_GPIO_MOSI_Pin},
        .pin_miso = {.port = H_GPIO_MISO_Port, .pin = H_GPIO_MISO_Pin},
        .pin_sclk = {.port = H_GPIO_SCLK_Port, .pin = H_GPIO_SCLK_Pin},
        .pin_cs   = {.port = H_GPIO_CS_Port,   .pin = H_GPIO_CS_Pin},
        .pin_handshake = {.port = H_GPIO_HANDSHAKE_Port, .pin = H_GPIO_HANDSHAKE_Pin},
        .pin_data_ready = {.port = H_GPIO_DATA_READY_Port, .pin = H_GPIO_DATA_READY_Pin},
        .pin_reset = {.port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET},
        .tx_queue_size = H_SPI_TX_Q,
        .rx_queue_size = H_SPI_RX_Q,
        .mode = H_SPI_MODE,
        .clk_mhz = H_SPI_FD_CLK_MHZ,
    };
}
#endif

#ifdef CONFIG_ESP_HOSTED_UART_HOST_INTERFACE
struct esp_hosted_uart_config esp_hosted_get_default_uart_config(void)
{
    return (struct esp_hosted_uart_config) {
        .port = H_UART_PORT,
        .pin_tx = {.port = H_UART_PORT_TX, .pin = H_UART_PIN_TX},
        .pin_rx = {.port = H_UART_PORT_RX, .pin = H_UART_PIN_RX},
        .pin_reset = {.port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET},
        .num_data_bits = H_UART_NUM_DATA_BITS,
        .parity = H_UART_PARITY,
        .stop_bits = H_UART_STOP_BITS,
        .flow_ctrl = H_UART_FLOWCTRL,
        .clk_src = H_UART_CLK_SRC,
        .checksum_enable = H_UART_CHECKSUM,
        .baud_rate = H_UART_BAUD_RATE,
        .tx_queue_size = H_UART_TX_QUEUE_SIZE,
        .rx_queue_size = H_UART_RX_QUEUE_SIZE,
    };
}
#endif
