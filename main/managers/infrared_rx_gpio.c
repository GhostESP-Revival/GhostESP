/**
 * @file infrared_rx_gpio.c
 * @brief GPIO interrupt-based IR receiver
 *
 * Uses GPIO interrupts and hardware timer instead of RMT RX for reliable IR reception
 * Based on IRremoteESP8266 library approach
 */

#include "managers/infrared_manager.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "managers/infrared_decoder.h"
#include <string.h>

#ifdef CONFIG_HAS_INFRARED_RX

static const char *TAG = "ir_rx_gpio";

#define IR_RX_BUFFER_SIZE 512  // Raw timing buffer size
#define IR_RX_TIMEOUT_US 15000 // 15ms timeout (same as IRremote library default)
#define IR_RX_TICK_US 2        // 2us per tick for timing precision

// RX state machine
typedef enum {
    IR_RX_STATE_IDLE = 0,
    IR_RX_STATE_MARK,
    IR_RX_STATE_SPACE,
    IR_RX_STATE_STOP
} ir_rx_state_t;

// Volatile params accessed from ISR
typedef struct {
    volatile ir_rx_state_t state;
    volatile uint16_t rawbuf[IR_RX_BUFFER_SIZE];
    volatile uint16_t rawlen;
    volatile bool overflow;
    volatile uint32_t last_time_us;
    gpio_num_t pin;
} ir_rx_params_t;

static ir_rx_params_t ir_params = {0};
static esp_timer_handle_t ir_timeout_timer = NULL;
static InfraredDecoderContext *ir_decoder = NULL;
static volatile bool ir_rx_initialized = false;
static volatile bool ir_rx_cancel_flag = false;

// ISR for timeout - signal end of IR capture
static void IRAM_ATTR ir_timeout_callback(void *arg) {
    if (ir_params.rawlen > 0) {
        ir_params.state = IR_RX_STATE_STOP;
    }
}

// GPIO interrupt handler - captures edge timings
static void IRAM_ATTR ir_gpio_isr_handler(void *arg) {
    uint32_t now_us = (uint32_t)esp_timer_get_time();

    // Stop the timeout timer - we'll restart it at the end
    if (ir_timeout_timer) {
        esp_timer_stop(ir_timeout_timer);
    }

    uint16_t rawlen = ir_params.rawlen;

    // Check for buffer overflow
    if (rawlen >= IR_RX_BUFFER_SIZE) {
        ir_params.overflow = true;
        ir_params.state = IR_RX_STATE_STOP;
        return;
    }

    // Don't capture if we're stopped
    if (ir_params.state == IR_RX_STATE_STOP) {
        return;
    }

    // First edge - start of signal
    if (ir_params.state == IR_RX_STATE_IDLE) {
        ir_params.state = IR_RX_STATE_MARK;
        ir_params.rawbuf[rawlen] = 1;  // Mark start
        ir_params.rawlen++;
    } else {
        // Calculate time since last edge in ticks (2us per tick)
        uint32_t delta_us;
        if (now_us >= ir_params.last_time_us) {
            delta_us = now_us - ir_params.last_time_us;
        } else {
            // Handle timer wrap-around
            delta_us = (UINT32_MAX - ir_params.last_time_us) + now_us + 1;
        }

        uint16_t ticks = (uint16_t)(delta_us / IR_RX_TICK_US);
        if (ticks > 0 && rawlen < IR_RX_BUFFER_SIZE) {
            ir_params.rawbuf[rawlen] = ticks;
            ir_params.rawlen++;
        }
    }

    ir_params.last_time_us = now_us;

    // Restart timeout timer
    if (ir_timeout_timer) {
        esp_timer_start_once(ir_timeout_timer, IR_RX_TIMEOUT_US);
    }
}

bool infrared_manager_rx_init(void) {
    if (ir_rx_initialized) {
        ESP_LOGI(TAG, "IR RX already initialized");
        return true;
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "poltergeist") == 0) {
        gpio_reset_pin(24);
        gpio_set_direction(24, GPIO_MODE_OUTPUT);
        gpio_set_level(24, 0);
        ESP_LOGI(TAG, "IO24 configured for poltergeist template");
    }
#endif

    // Configure GPIO for IR RX pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_INFRARED_RX_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  // Trigger on both edges
    };

    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d", CONFIG_INFRARED_RX_PIN);
        return false;
    }

    // Create timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = ir_timeout_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ir_rx_timeout"
    };

    if (esp_timer_create(&timer_args, &ir_timeout_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timeout timer");
        return false;
    }

    // Initialize decoder
    ir_decoder = infrared_decoder_alloc();
    if (!ir_decoder) {
        ESP_LOGE(TAG, "Failed to allocate IR decoder");
        esp_timer_delete(ir_timeout_timer);
        ir_timeout_timer = NULL;
        return false;
    }

    // Initialize state
    memset((void *)&ir_params, 0, sizeof(ir_params));
    ir_params.pin = CONFIG_INFRARED_RX_PIN;
    ir_params.state = IR_RX_STATE_IDLE;

    // Install ISR service (only if not already installed)
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        // Only fail if it's an error other than "already installed"
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %d", isr_ret);
        infrared_decoder_free(ir_decoder);
        ir_decoder = NULL;
        esp_timer_delete(ir_timeout_timer);
        ir_timeout_timer = NULL;
        return false;
    }

    // Add handler for this specific pin
    if (gpio_isr_handler_add(CONFIG_INFRARED_RX_PIN, ir_gpio_isr_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GPIO ISR handler");
        infrared_decoder_free(ir_decoder);
        ir_decoder = NULL;
        esp_timer_delete(ir_timeout_timer);
        ir_timeout_timer = NULL;
        return false;
    }

    ir_rx_initialized = true;
    ir_rx_cancel_flag = false;

    ESP_LOGI(TAG, "IR RX initialized on GPIO %d (GPIO interrupt mode)", CONFIG_INFRARED_RX_PIN);
    return true;
}

void infrared_manager_rx_deinit(void) {
    if (!ir_rx_initialized) return;

    // Remove ISR handler
    gpio_isr_handler_remove(ir_params.pin);

    // Delete timer
    if (ir_timeout_timer) {
        esp_timer_stop(ir_timeout_timer);
        esp_timer_delete(ir_timeout_timer);
        ir_timeout_timer = NULL;
    }

    // Free decoder
    if (ir_decoder) {
        infrared_decoder_free(ir_decoder);
        ir_decoder = NULL;
    }

    ir_rx_initialized = false;
    ir_rx_cancel_flag = false;

    ESP_LOGI(TAG, "IR RX deinitialized");
}

void infrared_manager_rx_cancel(void) {
    ir_rx_cancel_flag = true;
}

bool infrared_manager_rx_is_initialized(void) {
    return ir_rx_initialized;
}

void infrared_manager_rx_suspend(void) {
    if (ir_rx_initialized) {
        gpio_intr_disable(ir_params.pin);
        if (ir_timeout_timer) {
            esp_timer_stop(ir_timeout_timer);
        }
    }
}

void infrared_manager_rx_resume(void) {
    if (ir_rx_initialized) {
        // Reset state
        ir_params.state = IR_RX_STATE_IDLE;
        ir_params.rawlen = 0;
        ir_params.overflow = false;

        gpio_intr_enable(ir_params.pin);
    }
}

// Convert raw buffer to timings in microseconds
static void convert_raw_to_timings(uint32_t *timings, size_t *count, const volatile uint16_t *rawbuf, uint16_t rawlen) {
    *count = 0;
    for (uint16_t i = 1; i < rawlen && i < IR_RX_BUFFER_SIZE; i++) {  // Skip first entry (mark start)
        if (*count >= IR_RX_MAX_SYMBOLS * 2) break;
        timings[*count] = rawbuf[i] * IR_RX_TICK_US;  // Convert ticks to microseconds
        (*count)++;
    }
}

bool infrared_manager_rx_receive(infrared_signal_t *signal, int timeout_ms) {
    if (!ir_rx_initialized || !signal) {
        ESP_LOGE(TAG, "IR RX not initialized or invalid signal pointer");
        return false;
    }

    int64_t deadline_us = -1;
    if (timeout_ms >= 0) {
        deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    }

    // Reset state to start receiving
    ir_params.state = IR_RX_STATE_IDLE;
    ir_params.rawlen = 0;
    ir_params.overflow = false;

    if (ir_decoder) {
        infrared_decoder_reset(ir_decoder);
    }

    ESP_LOGI(TAG, "Waiting for IR signal (timeout: %d ms)...", timeout_ms);

    while (true) {
        // Check for cancel
        if (ir_rx_cancel_flag) {
            ir_rx_cancel_flag = false;
            ESP_LOGI(TAG, "IR RX cancelled");
            return false;
        }

        // Check timeout
        if (deadline_us >= 0) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= deadline_us) {
                ESP_LOGW(TAG, "IR RX timeout");
                return false;
            }
        }

        // Check if we have a complete signal
        if (ir_params.state == IR_RX_STATE_STOP && ir_params.rawlen > 0) {
            ESP_LOGI(TAG, "IR signal captured: %d raw entries", ir_params.rawlen);

            if (ir_params.overflow) {
                ESP_LOGW(TAG, "Buffer overflow - signal may be truncated");
            }

            // Copy raw buffer to local (ISR could overwrite)
            uint16_t local_rawbuf[IR_RX_BUFFER_SIZE];
            uint16_t local_rawlen = ir_params.rawlen;
            memcpy(local_rawbuf, (void *)ir_params.rawbuf, local_rawlen * sizeof(uint16_t));

            // Reset for next signal
            ir_params.state = IR_RX_STATE_IDLE;
            ir_params.rawlen = 0;
            ir_params.overflow = false;

            // Validate signal has minimum entries
            if (local_rawlen < 6) {
                ESP_LOGW(TAG, "Signal too short (%d entries), ignoring", local_rawlen);
                continue;  // Keep listening
            }

            // Convert to timings
            uint32_t timings[IR_RX_MAX_SYMBOLS * 2];
            size_t timing_count = 0;
            convert_raw_to_timings(timings, &timing_count, local_rawbuf, local_rawlen);

            ESP_LOGI(TAG, "Converted to %zu timing values", timing_count);

            // Try to decode using protocol decoder
            if (ir_decoder) {
                infrared_decoder_reset(ir_decoder);
                InfraredDecodedMessage *decoded = NULL;

                // Feed timings to decoder (alternating mark/space)
                bool current_level = true;  // Start with mark (IR receivers are inverted)
                for (size_t i = 0; i < timing_count; i++) {
                    InfraredDecodedMessage *res = infrared_decoder_decode(ir_decoder, current_level, timings[i]);
                    if (res) {
                        decoded = res;
                        break;
                    }
                    current_level = !current_level;
                }

                // Finalize decoding
                if (!decoded) {
                    InfraredDecodedMessage *res = infrared_decoder_decode(ir_decoder, false, 0);
                    if (res) {
                        decoded = res;
                    }
                }

                // If decoded successfully, return parsed signal
                if (decoded) {
                    memset(signal, 0, sizeof(*signal));
                    signal->is_raw = false;

                    const char *proto_name = infrared_protocol_to_string(decoded->protocol);
                    strncpy(signal->payload.message.protocol,
                            proto_name ? proto_name : "Unknown",
                            sizeof(signal->payload.message.protocol) - 1);
                    signal->payload.message.address = decoded->address;
                    signal->payload.message.command = decoded->command;
                    snprintf(signal->name, sizeof(signal->name), "Learned_%.20s",
                             signal->payload.message.protocol);

                    ESP_LOGI(TAG, "Decoded: %s addr=0x%lx cmd=0x%lx",
                             signal->payload.message.protocol,
                             (unsigned long)signal->payload.message.address,
                             (unsigned long)signal->payload.message.command);

                    return true;
                }
            }

            // Fallback: store as raw signal
            ESP_LOGI(TAG, "Storing as raw signal");
            memset(signal, 0, sizeof(*signal));
            signal->is_raw = true;
            signal->payload.raw.frequency = 38000;
            signal->payload.raw.duty_cycle = 0.33f;
            signal->payload.raw.timings_size = timing_count;
            signal->payload.raw.timings = malloc(timing_count * sizeof(uint32_t));

            if (!signal->payload.raw.timings) {
                ESP_LOGE(TAG, "Failed to allocate memory for raw timings");
                return false;
            }

            memcpy(signal->payload.raw.timings, timings, timing_count * sizeof(uint32_t));
            snprintf(signal->name, sizeof(signal->name), "Raw_IR");

            ESP_LOGI(TAG, "Raw signal stored: %zu timings", timing_count);
            return true;
        }

        // Wait a bit before checking again
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

// Stubs for compatibility (no longer needed with GPIO approach)
rmt_channel_handle_t infrared_manager_get_rx_channel(void) {
    return NULL;
}

QueueHandle_t infrared_manager_get_rx_queue(void) {
    return NULL;
}

#else  // !CONFIG_HAS_INFRARED_RX

bool infrared_manager_rx_init(void) { return false; }
void infrared_manager_rx_deinit(void) {}
bool infrared_manager_rx_receive(infrared_signal_t *signal, int timeout_ms) { return false; }
void infrared_manager_rx_cancel(void) {}
bool infrared_manager_rx_is_initialized(void) { return false; }
void infrared_manager_rx_suspend(void) {}
void infrared_manager_rx_resume(void) {}
rmt_channel_handle_t infrared_manager_get_rx_channel(void) { return NULL; }
QueueHandle_t infrared_manager_get_rx_queue(void) { return NULL; }

#endif  // CONFIG_HAS_INFRARED_RX
