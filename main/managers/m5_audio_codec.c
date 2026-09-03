#include "managers/m5_audio_codec.h"

#if defined(CONFIG_HAS_ES7210_MIC) || defined(CONFIG_HAS_AW88298_SPEAKER)

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "lvgl_i2c/i2c_manager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "M5AudioCodec";
static i2c_master_dev_handle_t s_es7210 = NULL;
static i2c_master_dev_handle_t s_aw88298 = NULL;
static i2c_master_dev_handle_t s_aw9523 = NULL;
static i2c_master_dev_handle_t s_axp2101 = NULL;

static esp_err_t add_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(CONFIG_CORES3_AUDIO_I2C_PORT, &bus);
    if (ret != ESP_OK) return ret;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, device);
}

static esp_err_t es7210_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_es7210, data, sizeof(data), 100);
}

static esp_err_t aw88298_write(uint8_t reg, uint16_t value)
{
    uint8_t data[] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(s_aw88298, data, sizeof(data), 100);
}

#ifdef CONFIG_HAS_AW88298_SPEAKER

/* AW88298 registers used by the power-up sequence (datasheet V1.6, Table 2).
 *   SYSST   (0x01) RO status: PLLS bit0 = PLL locked, SWS bit8 = amplifier
 *                  switching, NOCLKS bit5 = PLL reference clock missing.
 *   SYSCTRL (0x04) PWDN bit0, AMPPD bit1, IPLL bit3 (0 = PLL reference is
 *                  BCLK), I2SEN bit6.
 *   SYSCTRL2(0x05) HMUTE bit4, BST_IPEAK bits[3:0] (0x8 = 3.5 A).
 * The amp derives every internal clock from the I2S bit clock, so PWDN may
 * only be cleared while BCLK is actually toggling - see
 * m5_audio_codec_speaker_enable(). */
#define AW88298_REG_SYSST      0x01
#define AW88298_REG_SYSCTRL    0x04
#define AW88298_REG_SYSCTRL2   0x05
#define AW88298_REG_I2SCTRL    0x06
#define AW88298_REG_HAGCCFG4   0x0C
#define AW88298_REG_BSTCTRL2   0x61

#define AW88298_SYSST_PLLS     (1u << 0)
#define AW88298_SYSST_NOCLKS   (1u << 5)
#define AW88298_SYSST_SWS      (1u << 8)

#define AW88298_SYSCTRL_BASE   0x4040u  /* I2SEN=1, IPLL=0 (BCLK reference) */
#define AW88298_SYSCTRL_PWDN   (1u << 0)
#define AW88298_SYSCTRL_AMPPD  (1u << 1)

/* BST_IPEAK = 3.5 A, HDCCE set, HMUTE clear. Hardware DC cancelling keeps a
 * DC offset from eating cone excursion on a small driver. */
#define AW88298_SYSCTRL2_BASE  0x0028u
#define AW88298_SYSCTRL2_HMUTE (1u << 4)

#define AW88298_BSTCTRL2_VALUE 0x0673u
#define AW88298_HOLDTH_VALUE   0x0064u  /* HAGCCFG4[7:0], VOL lives in [15:8] */

/* Current hardware volume, as the full HAGCCFG4 word. The amp is powered down
 * between sessions, so every bring-up rewrites this rather than assuming the
 * register survived. */
static uint16_t s_volume_reg = AW88298_HOLDTH_VALUE;  /* 0 dB */

#define AW88298_REG_ID         0x00
#define AW88298_REG_HAGCST     0x10
#define AW88298_REG_VDD        0x12
#define AW88298_REG_PVDD       0x14
#define AW88298_ID_EXPECTED    0x1852u

#define AW88298_SYSST_BSTS     (1u << 9)
#define AW88298_SYSST_UVLS     (1u << 14)

static esp_err_t aw88298_read(uint8_t reg, uint16_t *value)
{
    uint8_t raw[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_aw88298, &reg, 1, raw, sizeof(raw), 100);
    if (ret == ESP_OK && value) {
        *value = (uint16_t)((raw[0] << 8) | raw[1]);
    }
    return ret;
}

/* I2SCTRL value for a rate: INPLEV=0, I2SRXEN=1, CHSEL=mono, I2SMD=Philips,
 * I2SBCK=32*fs (matches the 16-bit stereo frame the ESP32 drives), and
 * I2SSR=index. The table is each supported rate quantised to units of
 * 2205 Hz, so the search maps a decoded sample rate onto the I2SSR encoding
 * (7 = 44.1 kHz, 8 = 48 kHz).
 *
 * CHSEL is 11 = (L+R)/2, not the 01 = left inherited from M5Unified: this is a
 * single mono speaker, and selecting "left" silently discarded everything
 * panned right. The internal halving of the sum also restores the clipping
 * headroom that the decoder's -6 dB pre-attenuation used to provide. */
static uint16_t aw88298_i2sctrl_value(uint32_t sample_rate)
{
    static const uint8_t rates[] = {4, 5, 6, 8, 10, 11, 15, 20, 22, 44};
    size_t index = 0;
    size_t rate = (sample_rate + 1102) / 2205;
    while (index + 1 < sizeof(rates) && rate > rates[index]) ++index;
    return (uint16_t)(0x1CC0u | index);
}

/* Poll a SYSST bit. The amp needs a few hundred microseconds to lock its PLL
 * once BCLK is valid; the timeout only bounds the failure case. */
static bool aw88298_wait_status(uint16_t mask, uint32_t timeout_ms)
{
    uint16_t status = 0;
    for (uint32_t waited = 0;; waited += 2) {
        if (aw88298_read(AW88298_REG_SYSST, &status) == ESP_OK && (status & mask)) {
            return true;
        }
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "AW88298 status 0x%04X not set within %lu ms (SYSST=0x%04X)",
                     (unsigned)mask, (unsigned long)timeout_ms, (unsigned)status);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* Full state dump, used when the amplifier fails to come up. Everything here
 * is read back from the part, so it separates the three things that look
 * identical from the ESP32 side:
 *   - ID != 0x1852 or writes that do not read back: I2C addressing is wrong,
 *     or the AW9523 speaker reset line is holding the part in reset.
 *   - VDD/PVDD near zero: the amp has no supply, so boost and amplifier can
 *     never start no matter what is written.
 *   - clocks/config correct but BSTS/SWS low: boost or amplifier fault. */
static void aw88298_log_state(const char *why)
{
    uint16_t id = 0, sysst = 0, sysctrl = 0, sysctrl2 = 0, i2sctrl = 0;
    uint16_t vol = 0, hagcst = 0, vdd = 0, pvdd = 0;

    (void)aw88298_read(AW88298_REG_ID, &id);
    (void)aw88298_read(AW88298_REG_SYSST, &sysst);
    (void)aw88298_read(AW88298_REG_SYSCTRL, &sysctrl);
    (void)aw88298_read(AW88298_REG_SYSCTRL2, &sysctrl2);
    (void)aw88298_read(AW88298_REG_I2SCTRL, &i2sctrl);
    (void)aw88298_read(AW88298_REG_HAGCCFG4, &vol);
    (void)aw88298_read(AW88298_REG_HAGCST, &hagcst);
    (void)aw88298_read(AW88298_REG_VDD, &vdd);
    (void)aw88298_read(AW88298_REG_PVDD, &pvdd);

    /* VDD full scale is 6.03 V and PVDD full scale is 12.05 V over 10 bits. */
    unsigned vdd_mv = (unsigned)(((uint32_t)(vdd & 0x3FF) * 6030u) / 1023u);
    unsigned pvdd_mv = (unsigned)(((uint32_t)(pvdd & 0x3FF) * 12050u) / 1023u);

    ESP_LOGW(TAG, "AW88298 %s: ID=0x%04X SYSST=0x%04X SYSCTRL=0x%04X SYSCTRL2=0x%04X",
             why, (unsigned)id, (unsigned)sysst, (unsigned)sysctrl, (unsigned)sysctrl2);
    ESP_LOGW(TAG, "AW88298 %s: I2SCTRL=0x%04X HAGCCFG4=0x%04X BSTVOUT=0x%02X VDD=%umV PVDD=%umV",
             why, (unsigned)i2sctrl, (unsigned)vol, (unsigned)(hagcst & 0x3F),
             vdd_mv, pvdd_mv);
    ESP_LOGW(TAG, "AW88298 %s: PLLS=%d CLKS=%d NOCLKS=%d BSTS=%d SWS=%d UVLS=%d", why,
             (sysst & AW88298_SYSST_PLLS) ? 1 : 0,
             (sysst & (1u << 4)) ? 1 : 0,
             (sysst & AW88298_SYSST_NOCLKS) ? 1 : 0,
             (sysst & AW88298_SYSST_BSTS) ? 1 : 0,
             (sysst & AW88298_SYSST_SWS) ? 1 : 0,
             (sysst & AW88298_SYSST_UVLS) ? 1 : 0);
    if (id != AW88298_ID_EXPECTED) {
        ESP_LOGE(TAG, "AW88298 ID mismatch (expected 0x1852) - wrong address or held in reset");
    }
}

#endif /* CONFIG_HAS_AW88298_SPEAKER */

esp_err_t m5_audio_codec_init(void)
{
    esp_err_t ret = lvgl_i2c_init(CONFIG_CORES3_AUDIO_I2C_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

#ifdef CONFIG_HAS_ES7210_MIC
    if (!s_es7210) {
        ret = add_device(CONFIG_ES7210_I2C_ADDR, &s_es7210);
        if (ret != ESP_OK) return ret;
    }

    /* Register table copied from M5Unified::_microphone_enabled_cb_cores3. */
    static const uint8_t es7210_regs[][2] = {
        {0x00, 0xFF}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00},
        {0x11, 0x60}, {0x40, 0x42}, {0x41, 0x70}, {0x42, 0x70},
        {0x43, 0x1B}, {0x44, 0x1B}, {0x45, 0x00}, {0x46, 0x00},
        {0x47, 0x00}, {0x48, 0x00}, {0x01, 0x14},
    };
    for (size_t i = 0; i < sizeof(es7210_regs) / sizeof(es7210_regs[0]); ++i) {
        ret = es7210_write(es7210_regs[i][0], es7210_regs[i][1]);
        if (ret != ESP_OK) return ret;
    }
#endif

#ifdef CONFIG_HAS_AW88298_SPEAKER
    if (!s_aw9523) {
        ret = add_device(0x58, &s_aw9523);
        if (ret != ESP_OK) return ret;
    }
    /* CoreS3-SE routes LCD reset, touch reset/interrupt, and speaker reset
     * through the AW9523B. Configure those pins before display/touch startup. */
    uint8_t aw9523_reg = 0x02;
    uint8_t p0_output = 0;
    ret = i2c_master_transmit_receive(s_aw9523, &aw9523_reg, 1,
                                      &p0_output, 1, 100);
    if (ret == ESP_OK) {
        uint8_t aw9523_data[2] = {0x02, (uint8_t)(p0_output | 0x05)};
        ret = i2c_master_transmit(s_aw9523, aw9523_data, sizeof(aw9523_data), 100);
    }
    if (ret == ESP_OK) {
        aw9523_reg = 0x03;
        uint8_t p1_output = 0;
        ret = i2c_master_transmit_receive(s_aw9523, &aw9523_reg, 1,
                                          &p1_output, 1, 100);
        if (ret == ESP_OK) {
            uint8_t aw9523_data[2] = {0x03, (uint8_t)(p1_output | 0x03)};
            ret = i2c_master_transmit(s_aw9523, aw9523_data, sizeof(aw9523_data), 100);
        }
    }
    if (ret == ESP_OK) {
        const uint8_t aw9523_config[][2] = {
            {0x04, 0x18}, /* P0_0 touch reset and P0_2 speaker reset: outputs */
            {0x05, 0x0C}, /* P1_2 touch interrupt and P1_3 speaker IRQ: inputs */
            {0x11, 0x10}, /* P0 push-pull mode */
            {0x12, 0xFF}, /* P0 GPIO mode */
            {0x13, 0xFF}, /* P1 GPIO mode */
        };
        for (size_t i = 0; i < sizeof(aw9523_config) / sizeof(aw9523_config[0]); ++i) {
            ret = i2c_master_transmit(s_aw9523, aw9523_config[i], 2, 100);
            if (ret != ESP_OK) break;
        }
    }
    if (ret != ESP_OK) return ret;

    /* CoreS3/CoreS3-SE LCD backlight is the AXP2101 DLDO1 rail, not a GPIO.
     * The AXP2101 sits on this same internal I2C bus (0x34). Enable the LCD
     * power rails so the panel is actually visible - without this the screen
     * stays dark after a cold boot (the rail defaults off). Register values
     * mirror M5GFX's CoreS3 power-on:
     *   0x90 = LDOS on/off  -> ALDO1-4 + BLDO1-2 + DLDO1(0x80, backlight)
     *   0x94 = ALDO3 = 3.3V, 0x95 = ALDO4 = 3.3V
     *   0x99 = DLDO1 (LCD backlight) voltage / brightness (0x1C ~= 3.3V, full) */
    if (!s_axp2101) {
        ret = add_device(0x34, &s_axp2101);
        if (ret != ESP_OK) return ret;
    }
    {
        static const uint8_t axp_regs[][2] = {
            {0x90, 0xBF}, {0x94, 0x1C}, {0x95, 0x1C}, {0x99, 0x1C},
        };
        for (size_t i = 0; i < sizeof(axp_regs) / sizeof(axp_regs[0]); ++i) {
            ret = i2c_master_transmit(s_axp2101, axp_regs[i], 2, 100);
            if (ret != ESP_OK) return ret;
        }
    }

    if (!s_aw88298) {
        ret = add_device(CONFIG_AW88298_I2C_ADDR, &s_aw88298);
        if (ret != ESP_OK) return ret;
    }

    /* Boot only stages the amp; it is deliberately left in power-down. The
     * AW88298 PLL takes its reference from the I2S bit clock (SYSCTRL.IPLL=0)
     * and no I2S channel exists this early in boot, so clearing PWDN here
     * would strand the amp permanently unlocked (SYSST.NOCLKS) - the datasheet
     * power-up sequence requires "I2S Clock Valid" before PWDN is released.
     * m5_audio_codec_speaker_enable() runs the real sequence once BCLK is up. */
    ret = aw88298_write(AW88298_REG_SYSCTRL2,
                        AW88298_SYSCTRL2_BASE | AW88298_SYSCTRL2_HMUTE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_SYSCTRL,
                                           AW88298_SYSCTRL_BASE | AW88298_SYSCTRL_PWDN |
                                           AW88298_SYSCTRL_AMPPD);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_BSTCTRL2, AW88298_BSTCTRL2_VALUE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_I2SCTRL, aw88298_i2sctrl_value(44100));
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_HAGCCFG4, s_volume_reg);
#endif

    if (ret == ESP_OK) ESP_LOGI(TAG, "CoreS3 audio codecs initialized");
    return ret;
}

esp_err_t m5_audio_codec_speaker_enable(uint32_t sample_rate)
{
#ifdef CONFIG_HAS_AW88298_SPEAKER
    if (!s_aw88298) return ESP_ERR_INVALID_STATE;

    /* AW88298 datasheet V1.6, Table 2 "Detail Description of Power up
     * sequence". Every step is ordered against the I2S clock, which must
     * already be running when this is called:
     *   1. hard-mute and drop to power-down so the sequence starts clean
     *   2. configure boost + I2S data path while in stand-by
     *   3. PWDN=0 -> bias/OSC/PLL active, then wait for PLL lock
     *   4. AMPPD=0 -> boost and amplifier boot, then wait for SYSST.SWS
     *   5. release hard-mute
     * Clearing PWDN with no BCLK present, or changing the I2S clock under a
     * running amp without re-running this, leaves the part configured but
     * silent: it still acknowledges I2C writes and the ESP32 side still
     * reports healthy I2S writes, but nothing reaches the speaker. */
    esp_err_t ret = aw88298_write(AW88298_REG_SYSCTRL2,
                                  AW88298_SYSCTRL2_BASE | AW88298_SYSCTRL2_HMUTE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_SYSCTRL,
                                           AW88298_SYSCTRL_BASE | AW88298_SYSCTRL_PWDN |
                                           AW88298_SYSCTRL_AMPPD);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_BSTCTRL2, AW88298_BSTCTRL2_VALUE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_I2SCTRL,
                                           aw88298_i2sctrl_value(sample_rate));
    if (ret != ESP_OK) return ret;

    ret = aw88298_write(AW88298_REG_SYSCTRL, AW88298_SYSCTRL_BASE | AW88298_SYSCTRL_AMPPD);
    if (ret != ESP_OK) return ret;
    if (!aw88298_wait_status(AW88298_SYSST_PLLS, 50)) {
        aw88298_log_state("PLL not locked");
    }

    ret = aw88298_write(AW88298_REG_SYSCTRL, AW88298_SYSCTRL_BASE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_HAGCCFG4, s_volume_reg);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_SYSCTRL2, AW88298_SYSCTRL2_BASE);
    if (ret != ESP_OK) return ret;

    /* Check SWS only after the hard-mute is released: Table 2 lists the wait
     * before step 5, but a class-D output stage held in mute never actually
     * starts switching, so polling it while muted always times out. */
    if (aw88298_wait_status(AW88298_SYSST_SWS, 50)) {
        ESP_LOGI(TAG, "AW88298 speaker enabled @ %lu Hz (I2SCTRL=0x%04X)",
                 (unsigned long)sample_rate, (unsigned)aw88298_i2sctrl_value(sample_rate));
    } else {
        aw88298_log_state("amplifier not switching");
    }
    return ESP_OK;
#else
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t m5_audio_codec_set_volume(uint8_t percent)
{
#ifdef CONFIG_HAS_AW88298_SPEAKER
    if (percent > 100) percent = 100;

    /* HAGCCFG4.VOL is attenuation: bits [7:4] in -6 dB steps and bits [3:0] in
     * -0.5 dB steps, so the field counts half-decibels in base 12. A straight
     * half-dB-per-percent taper gives 0 dB at 100% and -50 dB at 0%, which is
     * the usual linear-in-dB feel and lands on exact register steps.
     *
     * Doing this in the amp instead of scaling the PCM keeps the digital path
     * full-scale 16-bit - the old Q8 multiply-and-truncate threw away
     * resolution at every step below unity. */
    unsigned half_db = (unsigned)(100 - percent);
    unsigned coarse = half_db / 12;
    unsigned fine = half_db % 12;
    s_volume_reg = (uint16_t)(((coarse << 4 | fine) << 8) | AW88298_HOLDTH_VALUE);

    if (!s_aw88298) return ESP_ERR_INVALID_STATE;
    return aw88298_write(AW88298_REG_HAGCCFG4, s_volume_reg);
#else
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t m5_audio_codec_speaker_disable(void)
{
#ifdef CONFIG_HAS_AW88298_SPEAKER
    if (!s_aw88298) return ESP_ERR_INVALID_STATE;
    /* Mute before the amplifier and boost go down so the teardown is silent,
     * and leave the part in power-down: the next enable re-runs the full
     * sequence, which is only valid from this state. */
    esp_err_t ret = aw88298_write(AW88298_REG_SYSCTRL2,
                                  AW88298_SYSCTRL2_BASE | AW88298_SYSCTRL2_HMUTE);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_SYSCTRL,
                                           AW88298_SYSCTRL_BASE | AW88298_SYSCTRL_AMPPD);
    if (ret == ESP_OK) ret = aw88298_write(AW88298_REG_SYSCTRL,
                                           AW88298_SYSCTRL_BASE | AW88298_SYSCTRL_PWDN |
                                           AW88298_SYSCTRL_AMPPD);
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t m5_audio_codec_set_sample_rate(uint32_t sample_rate)
{
    /* A bare I2SCTRL rewrite does not re-lock the amp after the I2S clock has
     * been torn down and rebuilt at a new rate, so a rate change re-runs the
     * whole power-up sequence. */
    return m5_audio_codec_speaker_enable(sample_rate);
}

#else

esp_err_t m5_audio_codec_init(void) { return ESP_OK; }
esp_err_t m5_audio_codec_set_sample_rate(uint32_t sample_rate)
{
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t m5_audio_codec_speaker_enable(uint32_t sample_rate)
{
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t m5_audio_codec_speaker_disable(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t m5_audio_codec_set_volume(uint8_t percent)
{
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
