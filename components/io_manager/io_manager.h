#ifndef IO_MANAGER_H
#define IO_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

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
void io_manager_scan_i2c(void);
esp_err_t i2c_write_reg8_direct(uint8_t addr, uint8_t reg, uint8_t val);
esp_err_t i2c_read_reg8_direct(uint8_t addr, uint8_t reg, uint8_t *val);

/**
 * @brief Pulse the DAC RESET line low for 200ms via IO expander.
 *
 * Configures the configured expander pin as output, pulls it low for 200ms,
 * then drives it high. Used to reset the TLV320DAC3100 on startup.
 *
 * @return ESP_OK on success
 */
esp_err_t io_manager_dac_reset_pulse(void);

/**
 * @brief Set the board's status RGB LED, where it is wired to the expander.
 *
 * On the Attaky Core the "RGB LED" is not an addressable pixel: it is three
 * pins on the button expander (@ 0x59), P10 red, P11 green, P12 blue, common
 * anode and therefore active low. The expander is an AW9523B, so those three
 * pins can run as constant-current sinks with a 256-step dimming register per
 * pin, and that is how this drives them - a real 8-bit brightness per channel,
 * not an on/off gate.
 *
 * The first call switches P10-P12 into constant-current mode (a
 * read-modify-write of the mode register, so P13 touch reset and the unused
 * pins keep their GPIO state), zeroes their dimming registers so the switch
 * cannot flash white, and holds their output bits low so brightness is set by
 * the DIM value alone. Later calls write only the channels that changed.
 *
 * @param red    red channel, 0 = dark .. 255 = full brightness
 * @param green  green channel, same scale
 * @param blue   blue channel, same scale
 *
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED when the fitted expander
 *         has no status LED wired to it, ESP_ERR_INVALID_STATE before init.
 */
esp_err_t io_manager_set_status_led(uint8_t red, uint8_t green, uint8_t blue);

#ifdef __cplusplus
}
#endif

#endif // IO_MANAGER_H
