#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default I2C address for TLV320DAC3100IRHBT */
#define TLV320DAC3100_I2C_ADDR_DEFAULT 0x18

/* Register page numbers */
#define TLV320DAC3100_PAGE_0 0x00
#define TLV320DAC3100_PAGE_1 0x01
#define TLV320DAC3100_PAGE_3 0x03

/* Page 0 registers */
#define TLV320DAC3100_REG_PAGE_CTRL     0x00
#define TLV320DAC3100_REG_SW_RESET      0x01
#define TLV320DAC3100_REG_CLK_GEN_MUX   0x04
#define TLV320DAC3100_REG_PLL_P_R       0x05
#define TLV320DAC3100_REG_PLL_J         0x06
#define TLV320DAC3100_REG_PLL_D_MSB     0x07
#define TLV320DAC3100_REG_PLL_D_LSB     0x08
#define TLV320DAC3100_REG_NDAC          0x0B
#define TLV320DAC3100_REG_MDAC          0x0C
#define TLV320DAC3100_REG_DOSR_MSB      0x0D
#define TLV320DAC3100_REG_DOSR_LSB      0x0E
#define TLV320DAC3100_REG_AICLK         0x19
#define TLV320DAC3100_REG_CODEC_IF      0x1B
#define TLV320DAC3100_REG_DAC_PROC_BLOCK 0x3C
#define TLV320DAC3100_REG_DAC_DATAPATH  0x3F
#define TLV320DAC3100_REG_DAC_VOL_CTRL  0x40
#define TLV320DAC3100_REG_DAC_VOL_L     0x41
#define TLV320DAC3100_REG_DAC_VOL_R     0x42
#define TLV320DAC3100_REG_HEADSET_DETECT 0x43
#define TLV320DAC3100_REG_DAC_STICKY_FLAGS 0x2C
#define TLV320DAC3100_REG_DAC_INT_FLAGS  0x2E
#define TLV320DAC3100_REG_GPIO1         0x33
#define TLV320DAC3100_REG_VOL_MICDET_ADC_CTRL 0x74
#define TLV320DAC3100_REG_VOL_MICDET_ADC_VALUE 0x75

/* Page 3 timer/oscillator registers */
#define TLV320DAC3100_REG_TIMER_CLOCK_MCLK 0x10

/* Page 1 analog output registers used by DAC3100/AIC31xx family */
#define TLV320DAC3100_REG_HP_DRV        0x1F
#define TLV320DAC3100_REG_SPK_DRV       0x20
#define TLV320DAC3100_REG_DAC_MIXER     0x23
#define TLV320DAC3100_REG_HPL_VOL       0x24
#define TLV320DAC3100_REG_HPR_VOL       0x25
#define TLV320DAC3100_REG_SPL_VOL       0x26
#define TLV320DAC3100_REG_SPR_VOL       0x27
#define TLV320DAC3100_REG_HPL_GAIN      0x28
#define TLV320DAC3100_REG_HPR_GAIN      0x29
#define TLV320DAC3100_REG_SPL_GAIN      0x2A
#define TLV320DAC3100_REG_SPR_GAIN      0x2B
#define TLV320DAC3100_REG_MICBIAS       0x2E

/* Clock source mux values */
#define CLK_MUX_MCLK      0x00
#define CLK_MUX_BCLK      0x01
#define CLK_MUX_PLL       0x03

/* Power/register bit helpers */
#define PLL_EN_MASK       0x80
#define NDAC_EN_MASK      0x80
#define MDAC_EN_MASK      0x80
#define DAC_PDN_MASK      0x80
#define HP_DRV_EN_MASK    0x80
#define SPK_DRV_EN_MASK   0x80

/* Output route values */
#define DAC_ROUTE_NONE    0x00
#define DAC_ROUTE_HP      0x01
#define DAC_ROUTE_SPK     0x02
#define DAC_ROUTE_BOTH    0x03

/**
 * @brief TLV320DAC3100 configuration
 */
typedef struct {
    int i2c_port;
    int sda_pin;
    int scl_pin;
    uint8_t i2c_addr;
} tlv320dac3100_config_t;

#define TLV320DAC3100_DEFAULT_CONFIG() { \
    .i2c_port = 0, \
    .sda_pin = 6, \
    .scl_pin = 7, \
    .i2c_addr = TLV320DAC3100_I2C_ADDR_DEFAULT \
}

/**
 * @brief Initialize the TLV320DAC3100 DAC over I2C.
 *
 * This performs the full initialization sequence:
 * 1. Software reset
 * 2. PLL configuration for target sample rate
 * 3. NDAC/MDAC clock dividers
 * 4. DAC power-up
 * 5. Volume set to 0 dB
 * 6. Route output to speaker (default) + headphone detect
 *
 * @param config Pointer to configuration struct
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_init(const tlv320dac3100_config_t *config);

/**
 * @brief Deinitialize and power down the DAC.
 */
esp_err_t tlv320dac3100_deinit(void);

/**
 * @brief Perform a software reset of the DAC.
 */
esp_err_t tlv320dac3100_software_reset(void);

/**
 * @brief Set the DAC sample rate.
 *
 * Configures PLL and dividers for the requested rate.
 * Supported rates: 44100, 48000 Hz.
 *
 * @param sample_rate Sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_set_sample_rate(uint32_t sample_rate);

/**
 * @brief Set DAC output volume.
 *
 * @param volume Volume from 0 (mute) to 100 (max, 0 dB)
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_set_volume(uint8_t volume);

/**
 * @brief Mute or unmute the DAC output.
 *
 * @param mute true to mute, false to unmute
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_set_mute(bool mute);

/**
 * @brief Select output route: speaker, headphone, or both.
 *
 * @param route 0=none, 1=headphone, 2=speaker, 3=both
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_set_output_route(uint8_t route);

/**
 * @brief Read the TLV320DAC3100 headset insertion state.
 *
 * This uses the internal VOL/MICDET headset-detection block, not an ESP GPIO.
 *
 * @param inserted Output flag, true when a headset/jack insertion is detected
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_is_headphone_inserted(bool *inserted);

/**
 * @brief Enable or suspend the TLV headset detection block and poll task.
 *
 * Suspending detection is useful during active headphone playback because the
 * detect block can disturb the analog headphone path on some boards.
 *
 * @param enabled true to enable detection, false to suspend it
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_set_headphone_detection_enabled(bool enabled);

/**
 * @brief Detect headphone state once, apply the matching output route, then
 * suspend the headset-detect block.
 *
 * This avoids periodic detect polling during playback.
 *
 * @return ESP_OK on success
 */
esp_err_t tlv320dac3100_detect_headphone_once(void);

/**
 * @brief Power down the DAC (low power mode).
 */
esp_err_t tlv320dac3100_power_down(void);

/**
 * @brief Power up the DAC from low power mode.
 */
esp_err_t tlv320dac3100_power_up(void);

/**
 * @brief Check if the DAC driver is initialized.
 */
bool tlv320dac3100_is_initialized(void);

/**
 * @brief Register dump for debugging (logs page 0 registers).
 */
void tlv320dac3100_debug_dump(void);

#ifdef __cplusplus
}
#endif

#endif // TLV320DAC3100_H
