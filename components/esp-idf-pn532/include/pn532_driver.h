#ifndef PN532_DRIVER_H
#define PN532_DRIVER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C"
{
#endif
struct pn532_io_t;
typedef struct pn532_io_t pn532_io_t;
typedef struct pn532_io_t * pn532_io_handle_t;

#define PN532_HOST_TO_PN532                 (0xD4)

// timeouts in milliseconds used by public API calls
#ifndef PN532_WRITE_TIMEOUT
#define PN532_WRITE_TIMEOUT 100
#endif
#ifndef PN532_READ_TIMEOUT
#define PN532_READ_TIMEOUT 100
#endif

extern const uint8_t ACK_FRAME[];
extern const uint8_t NACK_FRAME[];

struct pn532_io_t {
    esp_err_t (*pn532_init_io)(pn532_io_handle_t io_handle);
    void (*pn532_release_io)(pn532_io_handle_t io_handle);
    void (*pn532_release_driver)(pn532_io_handle_t io_handle);
    esp_err_t (*pn532_read)(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
    esp_err_t (*pn532_write)(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);
    esp_err_t (*pn532_init_extra)(pn532_io_handle_t io_handle);
    esp_err_t (*pn532_is_ready)(pn532_io_handle_t io_handle);

    /*
     * High-level NFC frontend ops (vtable). When a backend other than the native
     * PN532 controller (e.g. the ST25R3916 RF transceiver) populates this handle,
     * it fills these pointers so the public pn532_* high-level API dispatches to
     * the alternate frontend. Left NULL for native PN532 handles, in which case
     * the public pn532_* functions fall through to their built-in PN532 logic.
     * This is what lets ST25R3916 and PN532 coexist in one binary while all of
     * GhostESP's high-level NFC code keeps calling the same pn532_* primitives.
     */
    esp_err_t (*hl_list_passive_target)(pn532_io_handle_t io_handle);
    esp_err_t (*hl_read_target_id_ex)(pn532_io_handle_t io_handle, uint8_t baud_rate_and_card_type,
                                      uint8_t *uid, uint8_t *uid_length,
                                      uint16_t *atqa, uint8_t *sak, int32_t timeout);
    esp_err_t (*hl_data_exchange)(pn532_io_handle_t io_handle, const uint8_t *send_buffer,
                                  uint8_t send_buffer_length, uint8_t *response, uint8_t *response_length);
    esp_err_t (*hl_communicate_thru)(pn532_io_handle_t io_handle, const uint8_t *send_buffer,
                                     uint8_t send_buffer_length, uint8_t *response, uint8_t *response_length);
    esp_err_t (*hl_set_passive_activation_retries)(pn532_io_handle_t io_handle, uint8_t maxRetries);
    esp_err_t (*hl_mfc_auth_recover)(pn532_io_handle_t io_handle);

    gpio_num_t reset;
    gpio_num_t irq;
    bool isSAMConfigDone;
#ifdef CONFIG_ENABLE_IRQ_ISR
    QueueHandle_t IRQQueue;
#endif
    void * driver_data;
};

esp_err_t pn532_init(pn532_io_handle_t io_handle);
void pn532_release(pn532_io_handle_t io_handle);
void pn532_delete_driver(pn532_io_handle_t io_handle);
void pn532_reset(pn532_io_handle_t io_handle);
esp_err_t pn532_write_command(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmdlen, int timeout);
esp_err_t pn532_read_data(pn532_io_handle_t io_handle, uint8_t *buffer, uint8_t length, int32_t timeout);
esp_err_t pn532_wait_ready(pn532_io_handle_t io_handle, int32_t timeout);
esp_err_t pn532_SAM_config(pn532_io_handle_t io_handle);
esp_err_t pn532_send_command_wait_ack(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmd_length, int32_t timeout);
esp_err_t pn532_read_ack(pn532_io_handle_t io_handle);

#ifdef __cplusplus
}
#endif

#endif //PN532_DRIVER_H


