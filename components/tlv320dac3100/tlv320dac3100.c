#include "tlv320dac3100.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef CONFIG_HAS_TLV320DAC_I2C

static const char *TAG = "TLV320DAC";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static StaticSemaphore_t s_i2c_mutex_buffer;
static SemaphoreHandle_t s_i2c_mutex = NULL;
static bool s_initialized = false;
static bool s_i2c_bus_owned = false;
static tlv320dac3100_config_t s_config = {0};
static uint32_t s_current_sample_rate = 44100;
static bool s_muted = false;
static uint8_t s_current_volume = 100;
static uint8_t s_requested_route = DAC_ROUTE_BOTH;
static uint8_t s_applied_route = DAC_ROUTE_NONE;
static bool s_headphone_detect_enabled = false;
static bool s_headphone_inserted = false;
static bool s_headphone_detect_stop = false;
static TaskHandle_t s_headphone_detect_task = NULL;
static uint8_t s_headphone_sticky_flags = 0;
static uint8_t s_headphone_flags = 0;
static uint8_t s_headphone_detect_reg = 0;
static uint8_t s_vol_micdet_adc = 0;

#define TLV320DAC3100_HEADSET_DETECT_ENABLE 0x80
#define TLV320DAC3100_HEADSET_DETECT_64MS   0x08
#define TLV320DAC3100_HEADSET_STATUS_BUTTON 0x20
#define TLV320DAC3100_HEADSET_STATUS_INSERTED 0x10
#define TLV320DAC3100_HEADSET_TYPE_MASK     0x60
#define TLV320DAC3100_MICBIAS_ALWAYS_ON_2V5 0x0A
#define TLV320DAC3100_VOL_MICDET_ADC_ENABLE 0x83

/* Forward declarations */
static esp_err_t tlv320dac3100_write_reg(uint8_t page, uint8_t reg, uint8_t val);
static esp_err_t tlv320dac3100_read_reg(uint8_t page, uint8_t reg, uint8_t *val);
static esp_err_t tlv320dac3100_set_page(uint8_t page);
static esp_err_t tlv320dac3100_get_or_create_bus(void);
static esp_err_t tlv320dac3100_add_device(void);
static void tlv320dac3100_cleanup_handles(void);
static void tlv320dac3100_log_key_registers(void);
static esp_err_t tlv320dac3100_apply_output_route(uint8_t route);
static esp_err_t tlv320dac3100_apply_effective_route(void);
static esp_err_t tlv320dac3100_enable_headphone_detect(void);
static void tlv320dac3100_start_headphone_detect_task(void);
static void tlv320dac3100_stop_headphone_detect_task(void);
static void tlv320dac3100_headphone_detect_task(void *arg);
static uint8_t tlv320dac3100_volume_to_reg(uint8_t volume);
static esp_err_t tlv320dac3100_apply_volume(uint8_t volume);
static esp_err_t tlv320dac3100_sample_vol_micdet_adc(void);

static void tlv320dac3100_i2c_lock(void)
{
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
}

static void tlv320dac3100_i2c_unlock(void)
{
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

static esp_err_t tlv320dac3100_write_reg(uint8_t page, uint8_t reg, uint8_t val)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    tlv320dac3100_i2c_lock();
    esp_err_t ret = tlv320dac3100_set_page(page);
    if (ret != ESP_OK) {
        tlv320dac3100_i2c_unlock();
        return ret;
    }

    uint8_t buf[2] = {reg, val};
    ret = i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
    tlv320dac3100_i2c_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C write failed (page=%d reg=0x%02X val=0x%02X): %s",
                 page, reg, val, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t tlv320dac3100_read_reg(uint8_t page, uint8_t reg, uint8_t *val)
{
    if (!s_dev || !val) return ESP_ERR_INVALID_ARG;

    tlv320dac3100_i2c_lock();
    esp_err_t ret = tlv320dac3100_set_page(page);
    if (ret != ESP_OK) {
        tlv320dac3100_i2c_unlock();
        return ret;
    }

    ret = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
    tlv320dac3100_i2c_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed (page=%d reg=0x%02X): %s",
                 page, reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t tlv320dac3100_set_page(uint8_t page)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {TLV320DAC3100_REG_PAGE_CTRL, page};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t tlv320dac3100_get_or_create_bus(void)
{
    if (s_i2c_bus) return ESP_OK;

    /* Try to get existing bus first (e.g., from io_manager) */
    esp_err_t ret = i2c_master_get_bus_handle((i2c_port_num_t)s_config.i2c_port, &s_i2c_bus);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Using existing I2C bus on port %d", s_config.i2c_port);
        s_i2c_bus_owned = false;
        return ESP_OK;
    }
    if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    /* No existing bus, create one */
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = (i2c_port_num_t)s_config.i2c_port,
        .scl_io_num = s_config.scl_pin,
        .sda_io_num = s_config.sda_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        s_i2c_bus_owned = false;
    } else {
        s_i2c_bus_owned = true;
    }
    return ret;
}

static esp_err_t tlv320dac3100_add_device(void)
{
    if (!s_i2c_bus) return ESP_ERR_INVALID_STATE;
    if (s_dev) return ESP_OK;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_config.i2c_addr,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify device is actually present on the bus */
    ret = i2c_master_probe(s_i2c_bus, s_config.i2c_addr, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Device probe failed at 0x%02X: %s", s_config.i2c_addr, esp_err_to_name(ret));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Device probed OK at 0x%02X", s_config.i2c_addr);
    return ESP_OK;
}

static void tlv320dac3100_cleanup_handles(void)
{
    s_headphone_detect_enabled = false;
    s_headphone_inserted = false;
    s_requested_route = DAC_ROUTE_BOTH;
    s_applied_route = DAC_ROUTE_NONE;

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_i2c_bus) {
        if (s_i2c_bus_owned) {
            i2c_del_master_bus(s_i2c_bus);
        }
        s_i2c_bus = NULL;
        s_i2c_bus_owned = false;
    }
    s_initialized = false;
}

static void tlv320dac3100_log_reg(uint8_t page, uint8_t reg, const char *name)
{
    uint8_t val = 0;
    if (tlv320dac3100_read_reg(page, reg, &val) == ESP_OK) {
        ESP_LOGI(TAG, "  P%d R0x%02X %-12s = 0x%02X", page, reg, name, val);
    }
}

static void tlv320dac3100_log_key_registers(void)
{
    ESP_LOGI(TAG, "TLV320DAC key register readback:");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_CLK_GEN_MUX, "CLKMUX");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_PLL_P_R, "PLLPR");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_PLL_J, "PLLJ");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_NDAC, "NDAC");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_MDAC, "MDAC");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_DOSR_LSB, "DOSR_LSB");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_CODEC_IF, "IFACE1");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_DAC_DATAPATH, "DACSETUP");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_DAC_VOL_CTRL, "DACMUTE");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_HEADSET_DETECT, "HPDETECT");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_DAC_STICKY_FLAGS, "STICKY");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_DAC_INT_FLAGS, "DACFLAGS");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_VOL_MICDET_ADC_CTRL, "MICADCCTL");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_0, TLV320DAC3100_REG_VOL_MICDET_ADC_VALUE, "MICADC");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_3, TLV320DAC3100_REG_TIMER_CLOCK_MCLK, "TIMERCLK");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_1, TLV320DAC3100_REG_HP_DRV, "HPDRV");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_1, TLV320DAC3100_REG_SPK_DRV, "SPKAMP");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_1, TLV320DAC3100_REG_DAC_MIXER, "MIXER");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_1, TLV320DAC3100_REG_MICBIAS, "MICBIAS");
    tlv320dac3100_log_reg(TLV320DAC3100_PAGE_1, TLV320DAC3100_REG_SPL_GAIN, "SPLGAIN");
}

esp_err_t tlv320dac3100_init(const tlv320dac3100_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    if (!config) return ESP_ERR_INVALID_ARG;

    s_config = *config;

    esp_err_t ret = tlv320dac3100_get_or_create_bus();
    if (ret != ESP_OK) return ret;

    ret = tlv320dac3100_add_device();
    if (ret != ESP_OK) {
        tlv320dac3100_cleanup_handles();
        return ret;
    }

    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutexStatic(&s_i2c_mutex_buffer);
        if (!s_i2c_mutex) {
            tlv320dac3100_cleanup_handles();
            return ESP_ERR_NO_MEM;
        }
    }

    /* Mark initialized so that configuration functions work.
     * If any critical step below fails we call deinit() to clean up. */
    s_initialized = true;

    /* Step 1: Software reset */
    ret = tlv320dac3100_software_reset();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Software reset failed: %s", esp_err_to_name(ret));
        tlv320dac3100_cleanup_handles();
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 2: Configure sample rate (default 44.1kHz) */
    ret = tlv320dac3100_set_sample_rate(s_current_sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sample rate config failed: %s", esp_err_to_name(ret));
        tlv320dac3100_cleanup_handles();
        return ret;
    }

    /* Step 3: Leave digital headroom; small speakers distort easily at 0 dBFS. */
    ret = tlv320dac3100_set_volume(85);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(ret));
        tlv320dac3100_cleanup_handles();
        return ret;
    }

    /* Step 4: Route DAC to speaker (default) + headphone */
    ret = tlv320dac3100_set_output_route(DAC_ROUTE_BOTH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Output route set failed: %s", esp_err_to_name(ret));
        tlv320dac3100_cleanup_handles();
        return ret;
    }

    ret = tlv320dac3100_detect_headphone_once();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "One-shot headphone detection failed: %s", esp_err_to_name(ret));
    }

    tlv320dac3100_log_key_registers();

    ESP_LOGI(TAG, "TLV320DAC3100 initialized at I2C addr 0x%02X", s_config.i2c_addr);
    return ESP_OK;
}

esp_err_t tlv320dac3100_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    s_initialized = false;
    tlv320dac3100_stop_headphone_detect_task();
    tlv320dac3100_power_down();

    tlv320dac3100_cleanup_handles();
    ESP_LOGI(TAG, "TLV320DAC3100 deinitialized");
    return ESP_OK;
}

esp_err_t tlv320dac3100_software_reset(void)
{
    return tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_SW_RESET, 0x01);
}

esp_err_t tlv320dac3100_set_sample_rate(uint32_t sample_rate)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Only 44.1kHz and 48kHz are well-supported; default to 44.1kHz */
    if (sample_rate != 44100 && sample_rate != 48000) {
        ESP_LOGW(TAG, "Unsupported sample rate %lu, defaulting to 44100", (unsigned long)sample_rate);
        sample_rate = 44100;
    }
    s_current_sample_rate = sample_rate;

    /* S3 provides BCLK/WCLK/DIN only. Use BCLK as PLL input and PLL as CODEC_CLKIN. */
    esp_err_t ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                               TLV320DAC3100_REG_CLK_GEN_MUX,
                                               (CLK_MUX_BCLK << 2) | CLK_MUX_PLL);
    if (ret != ESP_OK) return ret;

    /* BCLK is 32 * Fs. PLL = BCLK * 64 = 2048 * Fs using P=1, R=2, J=32. */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_PLL_P_R,
                                   0x92); /* PLL enabled, P=1, R=2 */
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_PLL_J,
                                   32);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_PLL_D_MSB,
                                   0x00);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_PLL_D_LSB,
                                   0x00);
    if (ret != ESP_OK) return ret;

    /* Fs = PLL / (NDAC * MDAC * DOSR) = (2048 * Fs) / (8 * 2 * 128). */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_NDAC, 0x88);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_MDAC, 0x82);
    if (ret != ESP_OK) return ret;

    /* DOSR = 128 */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_DOSR_MSB, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_DOSR_LSB, 0x80);
    if (ret != ESP_OK) return ret;

    /* I2S interface: I2S, 16-bit, codec as BCLK/WCLK slave */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_CODEC_IF,
                                   0x00);
    if (ret != ESP_OK) return ret;

    /* DAC processing block PRB_P1 */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_DAC_PROC_BLOCK,
                                   0x01);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Configured for %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t tlv320dac3100_set_volume(uint8_t volume)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (volume > 100) volume = 100;
    s_current_volume = volume;

    uint8_t reg_val = tlv320dac3100_volume_to_reg(volume);
    esp_err_t ret = tlv320dac3100_apply_volume(volume);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Volume set to %d%% (reg=0x%02X)", volume, reg_val);
    return ESP_OK;
}

static uint8_t tlv320dac3100_volume_to_reg(uint8_t volume)
{
    if (volume > 100) volume = 100;

    /* DAC digital volume uses signed 0.5 dB steps; 0x00 is 0 dB.
     * Positive values add gain, so use two's-complement negative attenuation.
     */
    if (volume == 0) {
        return 0x80; /* mute-level attenuation */
    }

    const int max_gain_half_db = 0; /* 100% = 0 dB; avoid digital boost/clipping. */
    int level_half_db = max_gain_half_db - (((100 - volume) * (127 + max_gain_half_db)) / 100);
    return (uint8_t)level_half_db;
}

static esp_err_t tlv320dac3100_apply_volume(uint8_t volume)
{
    uint8_t reg_val = tlv320dac3100_volume_to_reg(volume);

    esp_err_t ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                            TLV320DAC3100_REG_DAC_VOL_L,
                                            reg_val);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_DAC_VOL_R,
                                   reg_val);
    return ret;
}

esp_err_t tlv320dac3100_set_mute(bool mute)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_muted = mute;

    /* Volume control register bit 7 = mute for both channels */
    uint8_t vol_ctrl = mute ? 0xC0 : 0x00;
    return tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                    TLV320DAC3100_REG_DAC_VOL_CTRL,
                                    vol_ctrl);
}

static esp_err_t tlv320dac3100_apply_output_route(uint8_t route)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t route_val = route & 0x03;

    /* Power up left/right DAC datapaths and route left/right I2S data to DACs. */
    esp_err_t ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                             TLV320DAC3100_REG_DAC_DATAPATH,
                                             0xD4);
    if (ret != ESP_OK) return ret;

    /* Unmute DAC left/right digital volume control. */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                   TLV320DAC3100_REG_DAC_VOL_CTRL,
                                   0x00);
    if (ret != ESP_OK) return ret;

    /* Route DAC outputs into the analog output mixers. */
    uint8_t mixer = 0x00;
    if (route_val & DAC_ROUTE_HP) {
        mixer |= 0x44; /* Left DAC -> left mixer, right DAC -> right mixer */
    }
    if (route_val & DAC_ROUTE_SPK) {
        mixer |= 0x44;
    }
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                  TLV320DAC3100_REG_DAC_MIXER,
                                  mixer);
    if (ret != ESP_OK) return ret;

    /* Enable HP driver if headphone selected */
    if (route_val & DAC_ROUTE_HP) {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                       TLV320DAC3100_REG_HP_DRV,
                                       0xD0); /* HPL/HPR on, common-mode 1.65V */
    } else {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                       TLV320DAC3100_REG_HP_DRV,
                                       0x00);
    }
    if (ret != ESP_OK) return ret;

    /* Enable speaker driver if speaker selected */
    if (route_val & DAC_ROUTE_SPK) {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                       TLV320DAC3100_REG_SPK_DRV,
                                       0x80); /* mono class-D speaker driver on */
    } else {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                       TLV320DAC3100_REG_SPK_DRV,
                                       0x00);
    }
    if (ret != ESP_OK) return ret;

    if (route_val & DAC_ROUTE_HP) {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_HPL_VOL,
                                      0x80); /* path on, 0 dB analog volume */
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_HPR_VOL,
                                      0x80);
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_HPL_GAIN,
                                      0x04); /* driver path on, 0 dB */
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_HPR_GAIN,
                                      0x04);
        if (ret != ESP_OK) return ret;
    }
    if (route_val & DAC_ROUTE_SPK) {
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_SPL_VOL,
                                      0x80); /* path on, 0 dB analog volume */
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_SPR_VOL,
                                      0x80);
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_SPL_GAIN,
                                      0x04); /* speaker path on, minimum class-D gain */
        if (ret != ESP_OK) return ret;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                      TLV320DAC3100_REG_SPR_GAIN,
                                      0x04);
        if (ret != ESP_OK) return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "Output route set to %d (HP=%s, SPK=%s)",
             route_val,
             (route_val & DAC_ROUTE_HP) ? "on" : "off",
             (route_val & DAC_ROUTE_SPK) ? "on" : "off");
    s_applied_route = route_val;
    return ESP_OK;
}

static esp_err_t tlv320dac3100_apply_effective_route(void)
{
    uint8_t effective_route = s_requested_route & 0x03;
    if (s_headphone_detect_enabled && s_headphone_inserted) {
        effective_route = DAC_ROUTE_HP;
    }

    if (effective_route == s_applied_route) {
        return ESP_OK;
    }

    return tlv320dac3100_apply_output_route(effective_route);
}

esp_err_t tlv320dac3100_set_output_route(uint8_t route)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_requested_route = route & 0x03;
    return tlv320dac3100_apply_effective_route();
}

esp_err_t tlv320dac3100_is_headphone_inserted(bool *inserted)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!inserted) return ESP_ERR_INVALID_ARG;

    uint8_t sticky = 0;
    esp_err_t ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                           TLV320DAC3100_REG_DAC_STICKY_FLAGS,
                                           &sticky);
    if (ret != ESP_OK) return ret;

    uint8_t flags = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                 TLV320DAC3100_REG_DAC_INT_FLAGS,
                                 &flags);
    if (ret != ESP_OK) return ret;

    uint8_t detect = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                 TLV320DAC3100_REG_HEADSET_DETECT,
                                 &detect);
    if (ret != ESP_OK) return ret;

    s_headphone_sticky_flags = sticky;
    s_headphone_flags = flags;
    s_headphone_detect_reg = detect;

    *inserted = ((flags & (TLV320DAC3100_HEADSET_STATUS_INSERTED |
                           TLV320DAC3100_HEADSET_STATUS_BUTTON)) != 0) ||
                ((sticky & TLV320DAC3100_HEADSET_STATUS_BUTTON) != 0) ||
                ((detect & TLV320DAC3100_HEADSET_TYPE_MASK) != 0);
    return ESP_OK;
}

esp_err_t tlv320dac3100_set_headphone_detection_enabled(bool enabled)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (!enabled) {
        tlv320dac3100_stop_headphone_detect_task();

        uint8_t detect_cfg = 0;
        esp_err_t ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                               TLV320DAC3100_REG_HEADSET_DETECT,
                                               &detect_cfg);
        if (ret != ESP_OK) return ret;
        detect_cfg &= ~TLV320DAC3100_HEADSET_DETECT_ENABLE;
        ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                      TLV320DAC3100_REG_HEADSET_DETECT,
                                      detect_cfg);
        if (ret != ESP_OK) return ret;

        s_headphone_detect_enabled = false;
        ESP_LOGI(TAG, "Headphone detection suspended");
        return ESP_OK;
    }

    esp_err_t ret = tlv320dac3100_enable_headphone_detect();
    if (ret != ESP_OK) return ret;
    tlv320dac3100_start_headphone_detect_task();
    ESP_LOGI(TAG, "Headphone detection resumed");
    return ESP_OK;
}

esp_err_t tlv320dac3100_detect_headphone_once(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    tlv320dac3100_stop_headphone_detect_task();

    esp_err_t ret = tlv320dac3100_enable_headphone_detect();
    if (ret != ESP_OK) return ret;

    bool inserted = false;
    ret = tlv320dac3100_is_headphone_inserted(&inserted);
    if (ret != ESP_OK) return ret;

    s_headphone_inserted = inserted;
    uint8_t route = inserted ? DAC_ROUTE_HP : s_requested_route;
    ret = tlv320dac3100_apply_output_route(route);
    if (ret != ESP_OK) return ret;

    uint8_t detect_cfg = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                 TLV320DAC3100_REG_HEADSET_DETECT,
                                 &detect_cfg);
    if (ret != ESP_OK) return ret;
    detect_cfg &= ~TLV320DAC3100_HEADSET_DETECT_ENABLE;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                  TLV320DAC3100_REG_HEADSET_DETECT,
                                  detect_cfg);
    if (ret != ESP_OK) return ret;

    s_headphone_detect_enabled = false;
    ESP_LOGI(TAG, "One-shot headphone detect: inserted=%s, route=%d; detection suspended",
             inserted ? "yes" : "no", s_applied_route);
    return ESP_OK;
}

static esp_err_t tlv320dac3100_enable_headphone_detect(void)
{
    uint8_t timer_clk = 0;
    esp_err_t ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_3,
                                           TLV320DAC3100_REG_TIMER_CLOCK_MCLK,
                                           &timer_clk);
    if (ret != ESP_OK) return ret;
    timer_clk &= ~(1 << 7); /* Match known-good init: enable internal timer oscillator. */
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_3,
                                  TLV320DAC3100_REG_TIMER_CLOCK_MCLK,
                                  timer_clk);
    if (ret != ESP_OK) return ret;

    uint8_t micbias = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_1,
                                 TLV320DAC3100_REG_MICBIAS,
                                 &micbias);
    if (ret != ESP_OK) return ret;
    micbias &= ~0x0F;
    micbias |= TLV320DAC3100_MICBIAS_ALWAYS_ON_2V5;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                                  TLV320DAC3100_REG_MICBIAS,
                                  micbias);
    if (ret != ESP_OK) return ret;

    uint8_t detect_cfg = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                 TLV320DAC3100_REG_HEADSET_DETECT,
                                 &detect_cfg);
    if (ret != ESP_OK) return ret;
    detect_cfg |= TLV320DAC3100_HEADSET_DETECT_ENABLE;
    detect_cfg &= ~0x1C;
    detect_cfg |= TLV320DAC3100_HEADSET_DETECT_64MS;
    ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                  TLV320DAC3100_REG_HEADSET_DETECT,
                                  detect_cfg);
    if (ret != ESP_OK) return ret;

    s_headphone_detect_enabled = true;
    vTaskDelay(pdMS_TO_TICKS(150));

    ret = tlv320dac3100_is_headphone_inserted(&s_headphone_inserted);
    if (ret != ESP_OK) return ret;
    (void)tlv320dac3100_sample_vol_micdet_adc();

    ESP_LOGI(TAG, "Headphone detect enabled, inserted=%s, sticky=0x%02X, flags=0x%02X, detect=0x%02X, adc=%u",
             s_headphone_inserted ? "yes" : "no",
             s_headphone_sticky_flags,
             s_headphone_flags,
             s_headphone_detect_reg,
             s_vol_micdet_adc);
    return tlv320dac3100_apply_effective_route();
}

static void tlv320dac3100_start_headphone_detect_task(void)
{
    if (!s_headphone_detect_enabled || s_headphone_detect_task) return;

    s_headphone_detect_stop = false;
    BaseType_t ok = xTaskCreate(tlv320dac3100_headphone_detect_task,
                                "tlv_hpdet",
                                3072,
                                NULL,
                                3,
                                &s_headphone_detect_task);
    if (ok != pdPASS) {
        s_headphone_detect_task = NULL;
        ESP_LOGW(TAG, "Failed to start headphone detect task");
    }
}

static esp_err_t tlv320dac3100_sample_vol_micdet_adc(void)
{
    esp_err_t ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                            TLV320DAC3100_REG_VOL_MICDET_ADC_CTRL,
                                            TLV320DAC3100_VOL_MICDET_ADC_ENABLE);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t adc = 0;
    ret = tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0,
                                 TLV320DAC3100_REG_VOL_MICDET_ADC_VALUE,
                                 &adc);

    esp_err_t restore_ret = tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                                                    TLV320DAC3100_REG_VOL_MICDET_ADC_CTRL,
                                                    0x00);
    if (restore_ret == ESP_OK) {
        restore_ret = tlv320dac3100_apply_volume(s_current_volume);
    }
    if (ret != ESP_OK) return ret;
    if (restore_ret != ESP_OK) return restore_ret;

    s_vol_micdet_adc = adc & 0x7F;
    return ESP_OK;
}

static void tlv320dac3100_stop_headphone_detect_task(void)
{
    if (!s_headphone_detect_task) return;

    s_headphone_detect_stop = true;
    for (int i = 0; i < 20 && s_headphone_detect_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_headphone_detect_task) {
        ESP_LOGW(TAG, "Headphone detect task did not stop before timeout");
    }
}

static void tlv320dac3100_headphone_detect_task(void *arg)
{
    (void)arg;
    uint8_t last_sticky = s_headphone_sticky_flags;
    uint8_t last_flags = s_headphone_flags;
    uint8_t last_detect = s_headphone_detect_reg;
    uint8_t last_adc = s_vol_micdet_adc;
    uint32_t poll_count = 0;

    while (!s_headphone_detect_stop && s_initialized) {
        bool inserted = false;
        esp_err_t ret = tlv320dac3100_is_headphone_inserted(&inserted);
        bool log_poll = ret == ESP_OK && (++poll_count % 20) == 0;

        if (ret == ESP_OK &&
            (s_headphone_sticky_flags != last_sticky ||
             s_headphone_flags != last_flags ||
             s_headphone_detect_reg != last_detect ||
             s_vol_micdet_adc != last_adc)) {
            ESP_LOGI(TAG, "Headphone raw sticky=0x%02X, flags=0x%02X, detect=0x%02X, adc=%u, inserted=%s",
                     s_headphone_sticky_flags,
                     s_headphone_flags,
                     s_headphone_detect_reg,
                     s_vol_micdet_adc,
                     inserted ? "yes" : "no");
            last_sticky = s_headphone_sticky_flags;
            last_flags = s_headphone_flags;
            last_detect = s_headphone_detect_reg;
            last_adc = s_vol_micdet_adc;
        }

        if (log_poll) {
            ESP_LOGI(TAG, "Headphone poll sticky=0x%02X, flags=0x%02X, detect=0x%02X, adc=%u, inserted=%s, route=%d",
                     s_headphone_sticky_flags,
                     s_headphone_flags,
                     s_headphone_detect_reg,
                     s_vol_micdet_adc,
                     inserted ? "yes" : "no",
                     s_applied_route);
        }

        if (ret == ESP_OK && inserted != s_headphone_inserted) {
            s_headphone_inserted = inserted;

            ret = tlv320dac3100_apply_effective_route();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Headphone %s, sticky=0x%02X, flags=0x%02X, detect=0x%02X, adc=%u, route=%d",
                         inserted ? "inserted" : "removed",
                         s_headphone_sticky_flags,
                         s_headphone_flags,
                         s_headphone_detect_reg,
                         s_vol_micdet_adc,
                         s_applied_route);
            } else {
                ESP_LOGW(TAG, "Headphone route update failed: %s", esp_err_to_name(ret));
            }
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Headphone detect read failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_headphone_detect_task = NULL;
    s_headphone_detect_stop = false;
    vTaskDelete(NULL);
}

esp_err_t tlv320dac3100_power_down(void)
{
    if (!s_initialized) return ESP_OK;

    /* Mute, power down DAC, disable drivers */
    tlv320dac3100_set_mute(true);
    tlv320dac3100_write_reg(TLV320DAC3100_PAGE_0,
                            TLV320DAC3100_REG_DAC_DATAPATH, 0x00);
    tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                            TLV320DAC3100_REG_HP_DRV, 0x00);
    tlv320dac3100_write_reg(TLV320DAC3100_PAGE_1,
                            TLV320DAC3100_REG_SPK_DRV, 0x00);
    s_applied_route = DAC_ROUTE_NONE;

    ESP_LOGI(TAG, "Powered down");
    return ESP_OK;
}

esp_err_t tlv320dac3100_power_up(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Re-apply configuration */
    esp_err_t ret = tlv320dac3100_set_sample_rate(s_current_sample_rate);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_set_volume(s_current_volume);
    if (ret != ESP_OK) return ret;
    ret = tlv320dac3100_set_output_route(DAC_ROUTE_BOTH);
    if (ret != ESP_OK) return ret;
    tlv320dac3100_set_mute(s_muted);

    ESP_LOGI(TAG, "Powered up");
    return ESP_OK;
}

bool tlv320dac3100_is_initialized(void)
{
    return s_initialized;
}

void tlv320dac3100_debug_dump(void)
{
    if (!s_initialized) {
        ESP_LOGI(TAG, "Not initialized");
        return;
    }

    ESP_LOGI(TAG, "=== TLV320DAC3100 Register Dump ===");
    uint8_t val;
    for (int reg = 0x00; reg <= 0x50; reg++) {
        if (tlv320dac3100_read_reg(TLV320DAC3100_PAGE_0, reg, &val) == ESP_OK) {
            ESP_LOGI(TAG, "  Page0 Reg 0x%02X = 0x%02X", reg, val);
        }
    }
}

#endif /* CONFIG_HAS_TLV320DAC_I2C */
