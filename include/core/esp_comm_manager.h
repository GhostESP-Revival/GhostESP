#ifndef ESP_COMM_MANAGER_H
#define ESP_COMM_MANAGER_H

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_BAUD_RATE 115200

typedef enum {
    COMM_STATE_IDLE,
    COMM_STATE_SCANNING,
    COMM_STATE_HANDSHAKE,
    COMM_STATE_CONNECTED,
    COMM_STATE_ERROR
} comm_state_t;

typedef enum {
    COMM_ROLE_MASTER,
    COMM_ROLE_SLAVE
} comm_role_t;

typedef struct {
    uint8_t chip_id[6];
    char chip_name[32];
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    comm_state_t state;
    comm_role_t role;
} comm_peer_t;

typedef void (*comm_command_callback_t)(const char* command, const char* data, void* user_data);

#define COMM_MAX_STREAM_CHANNELS 8
#define COMM_STREAM_CHANNEL_KEYBOARD 1
#define COMM_STREAM_CHANNEL_BADUSB  2

typedef void (*comm_stream_callback_t)(uint8_t channel, const uint8_t* data, size_t length, void* user_data);

void esp_comm_manager_init_with_defaults(void);
void esp_comm_manager_init(gpio_num_t tx_pin, gpio_num_t rx_pin, uint32_t baud_rate);
bool esp_comm_manager_set_pins(gpio_num_t tx_pin, gpio_num_t rx_pin);
bool esp_comm_manager_start_discovery(void);
bool esp_comm_manager_connect_to_peer(const char* peer_name);
bool esp_comm_manager_send_command(const char* command, const char* data);
bool esp_comm_manager_is_connected(void);
comm_state_t esp_comm_manager_get_state(void);
void esp_comm_manager_set_command_callback(comm_command_callback_t callback, void* user_data);
void esp_comm_manager_disconnect(void);
void esp_comm_manager_deinit(void);
bool esp_comm_manager_send_response(const uint8_t* data, size_t length);
void esp_comm_manager_set_remote_command_flag(bool is_remote);
bool esp_comm_manager_is_remote_command(void);

bool esp_comm_manager_send_stream(uint8_t channel, const uint8_t* data, size_t length);
bool esp_comm_manager_register_stream_handler(uint8_t channel, comm_stream_callback_t callback, void* user_data);

#endif // ESP_COMM_MANAGER_H