#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "pn532_driver.h"
#include "pn532.h"

const uint8_t ACK_FRAME[]  = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
const uint8_t NACK_FRAME[] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };

esp_err_t pn532_init(pn532_io_handle_t io_handle)
{
    gpio_config_t io_conf;

    if (io_handle == NULL)
        return ESP_ERR_INVALID_ARG;

    if (io_handle->reset != GPIO_NUM_NC) {
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = ((1ULL) << io_handle->reset);
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 1;
        if (gpio_config(&io_conf) != ESP_OK)
            return ESP_FAIL;
    }

    if (io_handle->irq != GPIO_NUM_NC) {
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = ((1ULL) << io_handle->irq);
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 1;
        if (gpio_config(&io_conf) != ESP_OK)
            return ESP_FAIL;
    }

    if (io_handle->reset != GPIO_NUM_NC) {
        gpio_set_level(io_handle->reset, 0);
        vTaskDelay(400 / portTICK_PERIOD_MS);
        gpio_set_level(io_handle->reset, 1);
        vTaskDelay(150 / portTICK_PERIOD_MS);
    }

    io_handle->isSAMConfigDone = false;
    esp_err_t err = io_handle->pn532_init_io(io_handle);
    if (err != ESP_OK)
        return err;

    // Allow device to settle and probe firmware to wake/verify link
    uint32_t fw = 0;
    (void)pn532_get_firmware_version(io_handle, &fw);

    // Try SAM config, retry once with a reset if it fails
    for (int attempt = 0; attempt < 2; ++attempt) {
        err = pn532_SAM_config(io_handle);
        if (err == ESP_OK) break;
        pn532_reset(io_handle);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err != ESP_OK) return err;

    io_handle->isSAMConfigDone = true;
    return err;
}

void pn532_release(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL)
        return;
    if (io_handle->pn532_release_io != NULL) {
        io_handle->pn532_release_io(io_handle);
    }
    if (io_handle->reset != GPIO_NUM_NC) {
        gpio_reset_pin(io_handle->reset);
    }
    if (io_handle->irq != GPIO_NUM_NC) {
        gpio_reset_pin(io_handle->irq);
    }
}

void pn532_delete_driver(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL)
        return;
    if (io_handle->pn532_release_driver != NULL)
        io_handle->pn532_release_driver(io_handle);
}

void pn532_reset(pn532_io_handle_t io_handle)
{
    if (io_handle->reset == GPIO_NUM_NC)
        return;
    gpio_set_level(io_handle->reset, 0);
    vTaskDelay(400 / portTICK_PERIOD_MS);
    gpio_set_level(io_handle->reset, 1);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    io_handle->isSAMConfigDone = false;
}

esp_err_t pn532_write_command(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmdlen, int timeout)
{
    uint8_t checksum = PN532_HOST_TO_PN532;
    uint8_t command[256];
    int idx = 0;
    command[idx++] = 0x00;
    command[idx++] = 0xFF;
    command[idx++] = (cmdlen + 1);
    command[idx++] = 0x100 - (cmdlen + 1);
    command[idx++] = PN532_HOST_TO_PN532;
    for (uint8_t i = 0; i < cmdlen; i++) {
        command[idx++] = cmd[i];
        checksum += cmd[i];
    }
    command[idx++] = ~checksum + 1;
    return io_handle->pn532_write(io_handle, command, idx, timeout);
}

esp_err_t pn532_read_data(pn532_io_handle_t io_handle, uint8_t *buffer, uint8_t length, int32_t timeout)
{
    return io_handle->pn532_read(io_handle, buffer, length, timeout);
}

esp_err_t pn532_wait_ready(pn532_io_handle_t io_handle, int32_t timeout)
{
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout > 0) ? pdMS_TO_TICKS(timeout) : portMAX_DELAY;
    // if IRQ available, poll aggressively with yield (GPIO reads are fast)
    // if no IRQ, use 1ms delay for I2C status checks
    bool has_irq = (io_handle->irq != GPIO_NUM_NC);
    
    while ((xTaskGetTickCount() - start_ticks) <= timeout_ticks) {
        if (io_handle->pn532_is_ready(io_handle) == ESP_OK) return ESP_OK;
        if (has_irq) {
            portYIELD(); // yield to other tasks, no delay for fast GPIO polling
        } else {
            vTaskDelay(pdMS_TO_TICKS(1)); // delay for I2C status checks
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t pn532_SAM_config(pn532_io_handle_t io_handle)
{
    esp_err_t result;
    uint8_t response_buffer[10];
    static const uint8_t sam_config_frame[] = { 0x14, 0x01, 0x00, 0x01 };
    result = pn532_send_command_wait_ack(io_handle, sam_config_frame, sizeof(sam_config_frame), 1000);
    if (ESP_OK != result) return result;
    result = pn532_wait_ready(io_handle, 500);
    if (ESP_OK != result) return result;
    result = pn532_read_data(io_handle, response_buffer, 10, 300);
    if (ESP_OK != result) return result;
    if (response_buffer[6] != 0x15) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t pn532_send_command_wait_ack(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmd_length, int32_t timeout)
{
    esp_err_t result;
    result = pn532_write_command(io_handle, cmd, cmd_length, timeout);
    if (result != ESP_OK) return result;
    result = pn532_wait_ready(io_handle, timeout);
    if (result != ESP_OK) return result;
    return pn532_read_ack(io_handle);
}

esp_err_t pn532_read_ack(pn532_io_handle_t io_handle) {
    uint8_t ack_buffer[6];
    esp_err_t result;
    result = pn532_read_data(io_handle, ack_buffer, sizeof(ACK_FRAME), 100);
    if (result != ESP_OK) return result;
    return (0 == memcmp(ack_buffer, ACK_FRAME, sizeof(ACK_FRAME))) ? ESP_OK : ESP_FAIL;
}


