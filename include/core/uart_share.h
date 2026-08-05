#ifndef UART_SHARE_H
#define UART_SHARE_H

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    UART_SHARE_OWNER_NONE = 0,
    UART_SHARE_OWNER_DUALCOMM,
    UART_SHARE_OWNER_GPS,
    UART_SHARE_OWNER_PLUGIN,
} uart_share_owner_t;

esp_err_t uart_share_ensure_installed(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int event_queue_size);

// Fully tears down the shared UART driver so another subsystem can install its
// own driver on the same port. Safe to call when nothing is installed.
esp_err_t uart_share_uninstall(uart_port_t uart_num);

esp_err_t uart_share_acquire(uart_port_t uart_num, uart_share_owner_t owner, TickType_t timeout);

esp_err_t uart_share_release(uart_port_t uart_num, uart_share_owner_t owner);

QueueHandle_t uart_share_get_event_queue(uart_port_t uart_num);

#endif
