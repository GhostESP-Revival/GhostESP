#include "core/uart_share.h"

#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>

typedef struct {
    SemaphoreHandle_t mutex;
    uart_share_owner_t owner;
    QueueHandle_t event_queue;
    uart_port_t uart_num;
    bool installed;
    int event_queue_size;
} uart_share_state_t;

static uart_share_state_t s_uart_share[UART_NUM_MAX] = {0};

static uart_share_state_t* uart_share_state_get(uart_port_t uart_num) {
    if (uart_num < 0 || uart_num >= UART_NUM_MAX) {
        return NULL;
    }

    uart_share_state_t* st = &s_uart_share[uart_num];
    if (!st->mutex) {
        static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&init_mux);
        if (!st->mutex) {
            st->mutex = new_mutex;
            new_mutex = NULL;
        }
        portEXIT_CRITICAL(&init_mux);
        if (new_mutex) vSemaphoreDelete(new_mutex);
    }
    st->uart_num = uart_num;
    return st;
}

esp_err_t uart_share_ensure_installed(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int event_queue_size) {
    uart_share_state_t* st = uart_share_state_get(uart_num);
    if (!st || !st->mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(st->mutex, portMAX_DELAY);

    if (!st->installed) {
        if (event_queue_size <= 0) {
            event_queue_size = 16;
        }

        st->event_queue_size = event_queue_size;

        esp_err_t err = uart_driver_install(uart_num, rx_buffer_size, tx_buffer_size, event_queue_size, &st->event_queue, 0);
        if (err != ESP_OK) {
            xSemaphoreGive(st->mutex);
            return err;
        }

        st->owner = UART_SHARE_OWNER_NONE;
        st->installed = true;
    }

    xSemaphoreGive(st->mutex);
    return ESP_OK;
}

esp_err_t uart_share_acquire(uart_port_t uart_num, uart_share_owner_t owner, TickType_t timeout) {
    uart_share_state_t* st = uart_share_state_get(uart_num);
    if (!st || !st->mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t start = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(st->mutex, portMAX_DELAY);

        if (!st->installed) {
            xSemaphoreGive(st->mutex);
            return ESP_ERR_INVALID_STATE;
        }

        if (st->owner == UART_SHARE_OWNER_NONE || st->owner == owner) {
            st->owner = owner;
            xSemaphoreGive(st->mutex);
            return ESP_OK;
        }

        xSemaphoreGive(st->mutex);

        if (timeout == 0) {
            return ESP_ERR_TIMEOUT;
        }

        if (xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t uart_share_release(uart_port_t uart_num, uart_share_owner_t owner) {
    uart_share_state_t* st = uart_share_state_get(uart_num);
    if (!st || !st->mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(st->mutex, portMAX_DELAY);

    if (!st->installed) {
        xSemaphoreGive(st->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (st->owner == owner) {
        st->owner = UART_SHARE_OWNER_NONE;
    }

    xSemaphoreGive(st->mutex);
    return ESP_OK;
}

QueueHandle_t uart_share_get_event_queue(uart_port_t uart_num) {
    uart_share_state_t* st = uart_share_state_get(uart_num);
    if (!st || !st->mutex) {
        return NULL;
    }

    xSemaphoreTake(st->mutex, portMAX_DELAY);
    QueueHandle_t q = st->event_queue;
    xSemaphoreGive(st->mutex);
    return q;
}
