#include "managers/encoder_manager.h"
#include "driver/gpio.h"
#include "esp_timer.h"

/* --- lookup table from Matthias Hertel’s library --- */
static const int8_t KNOBDIR[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0};

#define LATCH0 0
#define ENCODER_DEBOUNCE_US 1000         /* ignore latches quicker than this */
#define ENCODER_ACCEL_THRESH_US_1 15000  /* <15 ms => 2x */
#define ENCODER_ACCEL_THRESH_US_2 5000   /* <5 ms  => 4x */
#define LATCH3 3

/* helper for ms timestamps */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
\

void encoder_init(encoder_t *enc,
                  int pin_a,
                  int pin_b,
                  bool pullup,
                  encoder_latch_mode_t mode)
{
    enc->pin_a = pin_a;
    enc->pin_b = pin_b;
    enc->pullup = pullup;
    enc->mode = mode;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_a) | (1ULL << pin_b),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pullup ? GPIO_PULLUP_ENABLE  : GPIO_PULLUP_DISABLE,
        .pull_down_en = pullup ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int sig1 = gpio_get_level(pin_a);
    int sig2 = gpio_get_level(pin_b);
    enc->old_state = (int8_t)(sig1 | (sig2 << 1));

    enc->position = 0;
    enc->position_ext = 0;
    enc->position_ext_prev = 0;
    enc->pos_time_ms = enc->pos_time_prev_ms = now_ms();
    uint64_t now_us = esp_timer_get_time();
    enc->pos_time_us = now_us;
    enc->pos_time_prev_us = now_us;
    enc->rpm_time_index = 0;
    enc->rpm_time_count = 0;
    for (int i = 0; i < ENCODER_RPM_SMOOTHING_SIZE; ++i) enc->rpm_time_diffs_us[i] = 0;
}


/* poll-style tick (cheap enough to call from a tight loop or 1 kHz FreeRTOS timer) */
void encoder_tick(encoder_t *enc)
{
    int sig1 = gpio_get_level(enc->pin_a);
    int sig2 = gpio_get_level(enc->pin_b);
    int8_t this_state = (int8_t)(sig1 | (sig2 << 1));

    if (enc->old_state != this_state) {
        enc->position += KNOBDIR[this_state | (enc->old_state << 2)];
        enc->old_state = this_state;

        bool latched = false;
        switch (enc->mode) {
            case ENCODER_LATCH_FOUR3: latched = (this_state == LATCH3); break;
            case ENCODER_LATCH_FOUR0: latched = (this_state == LATCH0); break;
            case ENCODER_LATCH_TWO03: latched = (this_state == LATCH0 || this_state == LATCH3); break;
        }

        if (latched) {
            uint64_t now_us = esp_timer_get_time();
            uint32_t diff_us = (uint32_t)(now_us - enc->pos_time_us);

            /* Debounce */
            if (diff_us < ENCODER_DEBOUNCE_US) {
                return; /* ignore bounce */
            }

            /* Adaptive smoothing: restart average window if speed changes >30% */
            if (enc->rpm_time_count) {
                uint32_t last = enc->rpm_time_diffs_us[(enc->rpm_time_index + ENCODER_RPM_SMOOTHING_SIZE - 1) % ENCODER_RPM_SMOOTHING_SIZE];
                if (last) {
                    uint32_t diff_abs = (diff_us > last) ? (diff_us - last) : (last - diff_us);
                    if (diff_abs * 100 > last * 30) {
                        enc->rpm_time_count = 0;
                    }
                }
            }
            /* Update RPM buffer */
            if (enc->rpm_time_count < ENCODER_RPM_SMOOTHING_SIZE) enc->rpm_time_count++;
            enc->rpm_time_diffs_us[enc->rpm_time_index] = diff_us;
            enc->rpm_time_index = (enc->rpm_time_index + 1) % ENCODER_RPM_SMOOTHING_SIZE;

            /* Acceleration */
            int accel_mult = 1;
            if (diff_us < ENCODER_ACCEL_THRESH_US_2) {
                accel_mult = 4;
            } else if (diff_us < ENCODER_ACCEL_THRESH_US_1) {
                accel_mult = 2;
            }

            int32_t base_ext = (enc->mode == ENCODER_LATCH_TWO03) ? (enc->position >> 1) : (enc->position >> 2);
            int32_t delta = base_ext - enc->position_ext;
            enc->position_ext += delta * accel_mult;

            enc->pos_time_prev_us = enc->pos_time_us;
            enc->pos_time_us = now_us;
            enc->pos_time_prev_ms = (uint32_t)(enc->pos_time_prev_us / 1000);
            enc->pos_time_ms = (uint32_t)(enc->pos_time_us / 1000);
        }
    }
}

int32_t encoder_get_position(const encoder_t *enc)
{
    return enc->position_ext;
}

encoder_direction_t encoder_get_direction(encoder_t *enc)
{
    if (enc->position_ext_prev < enc->position_ext) {
        enc->position_ext_prev = enc->position_ext;
        return ENCODER_DIR_CW;
    } else if (enc->position_ext_prev > enc->position_ext) {
        enc->position_ext_prev = enc->position_ext;
        return ENCODER_DIR_CCW;
    }
    return ENCODER_DIR_NONE;
}

uint32_t encoder_get_millis_between_rotations(const encoder_t *enc)
{
    return (uint32_t)((enc->pos_time_us - enc->pos_time_prev_us) / 1000);
}

/* crude RPM estimate identical to original formula (20 steps per rev) */
uint32_t encoder_get_rpm(const encoder_t *enc)
{
    if (enc->rpm_time_count == 0) return 0;
    uint64_t sum_us = 0;
    for (uint8_t i = 0; i < enc->rpm_time_count; ++i) sum_us += enc->rpm_time_diffs_us[i];
    uint32_t avg_diff_us = (uint32_t)(sum_us / enc->rpm_time_count);
    if (avg_diff_us == 0) return 0;
    float rpm = 60000000.0f / (avg_diff_us * 20.0f);
    return (uint32_t)rpm;
} 