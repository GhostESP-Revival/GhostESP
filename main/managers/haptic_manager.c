#include "managers/haptic_manager.h"

#ifdef CONFIG_HAS_DRV2605_HAPTICS

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_shared.h"
#include "io_manager/i2c_bus_lock.h"
#include "vendor/drivers/axp2101.h"
#include <stdint.h>

#define DRV2605_REG_STATUS     0x00
#define DRV2605_REG_MODE       0x01
#define DRV2605_REG_RTPIN      0x02
#define DRV2605_REG_LIBRARY    0x03
#define DRV2605_REG_WAVESEQ1   0x04
#define DRV2605_REG_WAVESEQ2   0x05
#define DRV2605_REG_GO         0x0C
#define DRV2605_REG_OVERDRIVE  0x0D
#define DRV2605_REG_SUSTAINPOS 0x0E
#define DRV2605_REG_SUSTAINNEG 0x0F
#define DRV2605_REG_BREAK      0x10
#define DRV2605_REG_AUDIOCTRL  0x11
#define DRV2605_REG_AUDIOMAX  0x13
#define DRV2605_REG_RATEDV    0x16
#define DRV2605_REG_CLAMPV    0x17
#define DRV2605_REG_AUTOCALCOMP 0x18
#define DRV2605_REG_AUTOCALEMP  0x19
#define DRV2605_REG_FEEDBACK  0x1A
#define DRV2605_REG_CONTROL1  0x1B
#define DRV2605_REG_CONTROL3  0x1D
#define DRV2605_REG_OL_LRA_PERIOD 0x20

/* STATUS register (0x00): [7:5]=DEVICE_ID, [3]=DIAG_RESULT, [1]=OVER_TEMP, [0]=OC_DETECT */
#define DRV2605_STATUS_DIAG_RESULT  (1u << 3)
/* GO bit lives in the GO register (0x0C), bit 0 - NOT in STATUS */
#define DRV2605_GO_BIT              (1u << 0)

/* CONTROL3 (0x1D): bit0=LRA_OPEN_LOOP, bit5=ERM_OPEN_LOOP */
#define DRV2605_CTRL3_LRA_OPEN_LOOP (1u << 0)
#define DRV2605_CTRL3_ERM_OPEN_LOOP (1u << 5)

/* MODE register (0x01): mode select is bits [2:0], STANDBY is bit 6 */
#define DRV2605_MODE_INTERNAL_TRIGGER 0x00
#define DRV2605_MODE_RTP              0x05
#define DRV2605_MODE_DIAGNOSTIC       0x06
#define DRV2605_MODE_AUTOCAL          0x07
#define DRV2605_LIBRARY_ERM          0x01
#define DRV2605_LIBRARY_LRA          0x06
/* LRA drive levels. RATED_VOLTAGE sets the closed-loop drive reference (the
 * main "strength" knob); it is set before auto-calibration so the LRA is
 * calibrated for this level. OD_CLAMP is the overdrive ceiling - pushed near
 * the 3.3 V BLDO2 supply so effect onsets hit hard. Raised from the ~2 V
 * defaults because the actuator felt weak through the watch case. */
#define DRV2605_LRA_RATED_VOLTAGE    0x5A
#define DRV2605_LRA_OD_CLAMP         0xA4
/* Open-loop LRA drive period (~205 Hz), used only as a fallback if
 * auto-calibration fails. */
#define DRV2605_LRA_OL_PERIOD        0x33
#define DRV2605_AUTOCAL_TIMEOUT_MS   1500
#define DRV2605_AUTOCAL_POLL_MS      10
#define DRV2605_SCL_SPEED_HZ         400000
#define HAPTIC_MIN_INTERVAL_US       45000

static const char *TAG = "haptic";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_ready = false;
static int64_t s_last_play_us = 0;

static esp_err_t drv2605_write8(uint8_t reg, uint8_t value) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t data[2] = { reg, value };
    bool locked = i2c_bus_lock(CONFIG_DRV2605_I2C_PORT, 100);
    esp_err_t err = i2c_master_transmit(s_dev, data, sizeof(data), 100);
    if (locked) i2c_bus_unlock(CONFIG_DRV2605_I2C_PORT);
    return err;
}

static esp_err_t drv2605_read8(uint8_t reg, uint8_t *value) {
    if (!s_dev || !value) return ESP_ERR_INVALID_ARG;

    bool locked = i2c_bus_lock(CONFIG_DRV2605_I2C_PORT, 100);
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 100);
    if (locked) i2c_bus_unlock(CONFIG_DRV2605_I2C_PORT);
    return err;
}

static esp_err_t drv2605_modify_reg(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t cur = 0;
    esp_err_t err = drv2605_read8(reg, &cur);
    if (err != ESP_OK) return err;
    return drv2605_write8(reg, (uint8_t)((cur & (uint8_t)~mask) | (value & mask)));
}

/* Write a register and read it straight back, warning if it didn't stick.
 * Used to diagnose registers (e.g. LIBRARY) that have been observed reading
 * back a different value than was written. */
static esp_err_t drv2605_write8_verify(uint8_t reg, uint8_t value) {
    esp_err_t err = drv2605_write8(reg, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 reg 0x%02X write failed: %s", reg, esp_err_to_name(err));
        return err;
    }
    uint8_t rb = 0;
    if (drv2605_read8(reg, &rb) == ESP_OK && rb != value) {
        ESP_LOGW(TAG, "DRV2605 reg 0x%02X wrote 0x%02X but read back 0x%02X", reg, value, rb);
    }
    return err;
}

/* Run auto-calibration for a closed-loop LRA. This measures the actuator's
 * resonant frequency / back-EMF so playback is driven at resonance, which is
 * far stronger and crisper than fixed-frequency open-loop drive. The GO bit
 * self-clears when calibration finishes; DIAG_RESULT (STATUS bit 3) is 0 on
 * success. Leaves the device back in internal-trigger mode. Requires a healthy
 * supply - it silently times out if the DRV2605 is undervoltage. */
static esp_err_t drv2605_run_autocalibration(void) {
    esp_err_t err = drv2605_write8(DRV2605_REG_MODE, DRV2605_MODE_AUTOCAL);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_GO, DRV2605_GO_BIT);
    if (err != ESP_OK) {
        (void)drv2605_write8(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
        return err;
    }

    const int max_iters = DRV2605_AUTOCAL_TIMEOUT_MS / DRV2605_AUTOCAL_POLL_MS;
    bool done = false;
    for (int i = 0; i < max_iters; i++) {
        uint8_t go = 0;
        if (drv2605_read8(DRV2605_REG_GO, &go) == ESP_OK && (go & DRV2605_GO_BIT) == 0) {
            done = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(DRV2605_AUTOCAL_POLL_MS));
    }

    (void)drv2605_write8(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
    if (!done) return ESP_ERR_TIMEOUT;

    uint8_t status = 0;
    err = drv2605_read8(DRV2605_REG_STATUS, &status);
    if (err != ESP_OK) return err;
    if (status & DRV2605_STATUS_DIAG_RESULT) return ESP_FAIL;
    return ESP_OK;
}

static void drv2605_log_registers(void) {
    uint8_t v = 0;
    if (drv2605_read8(DRV2605_REG_MODE, &v) == ESP_OK) {
        ESP_LOGI(TAG, "  MODE     = 0x%02X (STANDBY=%d, MODE[2:0]=%d)",
                 v, (v >> 6) & 0x1, v & 0x7);
    }
    if (drv2605_read8(DRV2605_REG_LIBRARY, &v) == ESP_OK) {
        ESP_LOGI(TAG, "  LIBRARY  = 0x%02X", v);
    }
    if (drv2605_read8(DRV2605_REG_FEEDBACK, &v) == ESP_OK) {
        ESP_LOGI(TAG, "  FEEDBACK = 0x%02X (N_ERM_LRA=%d)", v, (v >> 7) & 0x1);
    }
    if (drv2605_read8(DRV2605_REG_CONTROL3, &v) == ESP_OK) {
        ESP_LOGI(TAG, "  CONTROL3 = 0x%02X (ERM_OL=%d, LRA_OL=%d)",
                 v, (v >> 5) & 0x1, v & 0x1);
    }
    if (drv2605_read8(DRV2605_REG_WAVESEQ1, &v) == ESP_OK) {
        ESP_LOGI(TAG, "  WAVESEQ1 = 0x%02X", v);
    }
}

/* DRV2605 ROM library (library 6 for LRA) effect indices. Single strong hits
 * are far easier to feel through a watch case on the wrist than the multi-tap
 * "double/triple click" effects, which spread their energy into weaker taps. */
static uint8_t drv2605_effect_id(haptic_effect_t effect) {
    switch (effect) {
        case HAPTIC_EFFECT_SELECTION:    return 4;   /* Sharp Click 100% */
        case HAPTIC_EFFECT_SUCCESS:      return 14;  /* Strong Buzz 100% */
        case HAPTIC_EFFECT_WARNING:      return 10;  /* Double Click 100% */
        case HAPTIC_EFFECT_ERROR:        return 47;  /* Buzz 1 100% */
        case HAPTIC_EFFECT_NOTIFICATION: return 15;  /* 750 ms Alert 100% */
        case HAPTIC_EFFECT_CLICK:
        default:                         return 1;   /* Strong Click 100% */
    }
}

esp_err_t haptic_manager_init(void) {
    if (s_ready) return ESP_OK;

    if (CONFIG_DRV2605_I2C_SDA_PIN < 0 || CONFIG_DRV2605_I2C_SCL_PIN < 0) {
        ESP_LOGW(TAG, "DRV2605 pins are not configured");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* The DRV2605 is powered from the AXP2101 BLDO2 rail on the T-Watch S3.
     * Ensure it is enabled at full voltage before any DRV2605 access, otherwise
     * the chip runs undervoltage: it answers I2C but won't latch config writes
     * or drive the actuator. */
    esp_err_t rail_err = axp2101_enable_haptic_rail();
    if (rail_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable DRV2605 power rail (BLDO2): %s",
                 esp_err_to_name(rail_err));
    }
    vTaskDelay(pdMS_TO_TICKS(30));  // let BLDO2 settle

    bool bus_created = false;
    esp_err_t err = i2c_shared_get_or_create_bus(CONFIG_DRV2605_I2C_PORT,
                                                 (gpio_num_t)CONFIG_DRV2605_I2C_SDA_PIN,
                                                 (gpio_num_t)CONFIG_DRV2605_I2C_SCL_PIN,
                                                 CONFIG_DRV2605_I2C_PULLUPS,
                                                 &s_bus,
                                                 &bus_created);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create/get DRV2605 I2C bus: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    err = i2c_shared_add_device(s_bus, CONFIG_DRV2605_I2C_ADDRESS, DRV2605_SCL_SPEED_HZ, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to attach DRV2605 at 0x%02X: %s", CONFIG_DRV2605_I2C_ADDRESS, esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    uint8_t status = 0;
    err = drv2605_read8(DRV2605_REG_STATUS, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 not responding at 0x%02X: %s", CONFIG_DRV2605_I2C_ADDRESS, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        xSemaphoreGive(s_mutex);
        return err;
    }

    /* Bring the device out of standby into internal-trigger mode. Verify it
     * sticks - on this hardware some low registers have been seen ignoring
     * writes, which is exactly what silent playback looks like. */
    err = drv2605_write8_verify(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_RTPIN, 0x00);
#ifdef CONFIG_DRV2605_LRA_MODE
    if (err == ESP_OK) err = drv2605_write8_verify(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_LRA);
#else
    if (err == ESP_OK) err = drv2605_write8_verify(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_ERM);
#endif
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_WAVESEQ1, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_WAVESEQ2, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_OVERDRIVE, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_SUSTAINPOS, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_SUSTAINNEG, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_BREAK, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_AUDIOMAX, 0x64);

    if (err == ESP_OK) {
#ifdef CONFIG_DRV2605_LRA_MODE
        /* LRA: configure for closed-loop drive (resonance tracking) and set the
         * drive levels that auto-calibration uses as its starting point. The
         * actual loop config is applied by auto-calibration below. */
        err = drv2605_modify_reg(DRV2605_REG_FEEDBACK, 0x80, 0x80);  /* N_ERM_LRA = LRA */
        if (err == ESP_OK)
            err = drv2605_modify_reg(DRV2605_REG_CONTROL3,
                                     (uint8_t)(DRV2605_CTRL3_ERM_OPEN_LOOP | DRV2605_CTRL3_LRA_OPEN_LOOP),
                                     0x00);  /* closed loop: clear both open-loop bits */
        if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_RATEDV, DRV2605_LRA_RATED_VOLTAGE);
        if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_CLAMPV, DRV2605_LRA_OD_CLAMP);
        if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_OL_LRA_PERIOD, DRV2605_LRA_OL_PERIOD);
#else
        err = drv2605_modify_reg(DRV2605_REG_FEEDBACK, 0x80, 0x00);
        if (err == ESP_OK)
            err = drv2605_modify_reg(DRV2605_REG_CONTROL3,
                                     (uint8_t)(DRV2605_CTRL3_ERM_OPEN_LOOP | DRV2605_CTRL3_LRA_OPEN_LOOP),
                                     DRV2605_CTRL3_ERM_OPEN_LOOP);
#endif
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 init sequence failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        xSemaphoreGive(s_mutex);
        return err;
    }

    (void)drv2605_write8(DRV2605_REG_GO, 0x00);

#ifdef CONFIG_DRV2605_LRA_MODE
    /* Calibrate the LRA so playback is driven at resonance (strongest, crispest
     * feel). If calibration fails, fall back to fixed-frequency open loop so
     * haptics still work, just a little weaker. */
    esp_err_t cal_err = drv2605_run_autocalibration();
    if (cal_err == ESP_OK) {
        uint8_t comp = 0, bemf = 0;
        (void)drv2605_read8(DRV2605_REG_AUTOCALCOMP, &comp);
        (void)drv2605_read8(DRV2605_REG_AUTOCALEMP, &bemf);
        ESP_LOGI(TAG, "DRV2605 auto-calibration OK (comp=0x%02X bemf=0x%02X)", comp, bemf);
    } else {
        ESP_LOGW(TAG, "DRV2605 auto-calibration failed (%s); using open-loop fallback",
                 esp_err_to_name(cal_err));
        (void)drv2605_modify_reg(DRV2605_REG_CONTROL3, DRV2605_CTRL3_LRA_OPEN_LOOP,
                                 DRV2605_CTRL3_LRA_OPEN_LOOP);
        (void)drv2605_write8(DRV2605_REG_OL_LRA_PERIOD, DRV2605_LRA_OL_PERIOD);
    }
#endif

    /* Short confirmation tick so init is felt without the long boot buzz. */
    (void)drv2605_write8(DRV2605_REG_WAVESEQ1, drv2605_effect_id(HAPTIC_EFFECT_CLICK));
    (void)drv2605_write8(DRV2605_REG_WAVESEQ2, 0x00);
    (void)drv2605_write8(DRV2605_REG_GO, DRV2605_GO_BIT);

    ESP_LOGI(TAG, "DRV2605 init complete - registers after init:");
    drv2605_log_registers();

    s_ready = true;
    ESP_LOGI(TAG, "DRV2605 haptics ready at 0x%02X on I2C port %d (SDA=%d, SCL=%d)",
             CONFIG_DRV2605_I2C_ADDRESS,
             CONFIG_DRV2605_I2C_PORT,
             CONFIG_DRV2605_I2C_SDA_PIN,
             CONFIG_DRV2605_I2C_SCL_PIN);

    (void)bus_created;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool haptic_manager_is_ready(void) {
    return s_ready;
}

void haptic_manager_play(haptic_effect_t effect) {
    if (!s_ready || !s_mutex) return;

    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_play_us < HAPTIC_MIN_INTERVAL_US) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    s_last_play_us = now_us;

    uint8_t effect_id = drv2605_effect_id(effect);
    esp_err_t err = drv2605_write8(DRV2605_REG_WAVESEQ1, effect_id);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_GO, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_WAVESEQ2, 0x00);
    if (err == ESP_OK) err = drv2605_write8(DRV2605_REG_GO, 0x01);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "DRV2605 play failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return;
    }

    /* Note: the GO bit (GO register 0x0C, bit 0) self-clears as soon as a short
     * ROM effect finishes, so reading it back here is racy and not a reliable
     * health signal. Only flag a persistent actuator/calibration fault. */
    uint8_t status = 0;
    if (drv2605_read8(DRV2605_REG_STATUS, &status) == ESP_OK) {
        if (status & DRV2605_STATUS_DIAG_RESULT) {
            ESP_LOGW(TAG, "DRV2605 DIAG_RESULT set (status=0x%02X) - check actuator/calibration",
                     status);
        }
    }

    xSemaphoreGive(s_mutex);
}

#endif /* CONFIG_HAS_DRV2605_HAPTICS */
