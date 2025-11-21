#ifndef IO_MANAGER_H
#define IO_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Button event structure
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool select;
    bool b1;
    bool b2;
    bool b3;
} btn_event_t;

// IO Manager configuration
typedef struct {
    int sda_pin;           // SDA pin (IO6)
    int scl_pin;           // SCL pin (IO7)
    uint8_t i2c_addr;      // I2C address (0x74)
    int i2c_port;          // I2C port number
} io_manager_config_t;

// Default configuration
#define IO_MANAGER_DEFAULT_CONFIG() { \
    .sda_pin = 6, \
    .scl_pin = 7, \
    .i2c_addr = 0x74, \
    .i2c_port = 0 \
}

// Function prototypes
esp_err_t io_manager_init(const io_manager_config_t *config);
esp_err_t io_manager_deinit(void);
esp_err_t io_manager_scan_buttons(btn_event_t *event);
esp_err_t io_manager_get_button_states(btn_event_t *states);
esp_err_t io_manager_get_cached_button_states(btn_event_t *states);
uint8_t io_manager_get_raw_state(void);
void io_manager_reset_events(void);
void io_manager_debug_states(void);
bool io_manager_is_initialized(void);

bool io_manager_get_encoder_signals(bool *signal_a, bool *signal_b);
bool io_manager_get_encoder_button(void);

#ifdef __cplusplus
}
#endif

#endif // IO_MANAGER_H
