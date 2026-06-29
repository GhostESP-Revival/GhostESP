#include "io_manager.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "i2c_bus_lock.h"
#include "i2c_shared.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "IO_MANAGER";
static i2c_master_bus_handle_t g_i2c_bus = NULL;
static i2c_master_dev_handle_t g_tca9535_dev = NULL;
static bool g_i2c_bus_owned = false;
static SemaphoreHandle_t g_i2c_mutex = NULL;

// TCA9535 register addresses
#define TCA9535_INPUT_PORT0     0x00
#define TCA9535_INPUT_PORT1     0x01
#define TCA9535_OUTPUT_PORT0    0x02
#define TCA9535_OUTPUT_PORT1    0x03
#define TCA9535_CONFIG_PORT0    0x06
#define TCA9535_CONFIG_PORT1    0x07

// Button pin definitions (P00-P04)
#define BTN_UP      0   // P00
#define BTN_DOWN    1   // P01
#define BTN_SELECT  2   // P02
#define BTN_LEFT    3   // P03
#define BTN_RIGHT   4   // P04

// Additional port1 buttons (P10-P12 -> port1 bits 0-2)
#define BTN_B1      0
#define BTN_B2      1
#define BTN_B3      2

// Encoder pins on port0
#define ENC_PIN_A   5   // P05
#define ENC_PIN_B   6   // P06
#define ENC_PIN_BTN 7   // P07

// Debouncing constants
#define DEBOUNCE_MS            16
#define IO_MANAGER_POLL_MS     10
#define IO_MANAGER_TASK_STACK  3072
#define IO_MANAGER_TASK_PRIO   5
#define I2C_LOCK_WARN_INTERVAL_MS 5000

// Global variables
static io_manager_config_t g_config;
static bool g_initialized = false;
static uint8_t g_last_state_port0 = 0xFF;
static uint8_t g_stable_state_port0 = 0xFF;
static uint8_t g_cached_state_port0 = 0xFF;
static uint8_t g_last_state_port1 = 0xFF;
static uint8_t g_stable_state_port1 = 0xFF;
static uint8_t g_cached_state_port1 = 0xFF;
static int64_t g_last_change_time = 0;
static TaskHandle_t g_io_task_handle = NULL;
static StackType_t *g_io_task_stack = NULL;
static StaticTask_t *g_io_task_buffer = NULL;
static int64_t g_last_input_lock_warn_ms = 0;

// Forward declarations
static esp_err_t tca9535_read_port(uint8_t reg, uint8_t *data);
static esp_err_t tca9535_read_inputs(uint8_t *port0, uint8_t *port1);
static esp_err_t tca9535_write_port(uint8_t reg, uint8_t data);
static esp_err_t io_manager_get_or_create_bus(void);
static esp_err_t io_manager_add_device(uint8_t addr, i2c_master_dev_handle_t *dev_out);
static void io_manager_process_sample(uint8_t port0, uint8_t port1, btn_event_t *event_out);
static void io_manager_task(void *arg);

esp_err_t io_manager_init(const io_manager_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "IO Manager already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    // Copy configuration
    g_config = *config;

    // create mutex for i2c bus access
    if (!g_i2c_mutex) {
        g_i2c_mutex = xSemaphoreCreateMutex();
        if (!g_i2c_mutex) {
            ESP_LOGE(TAG, "failed to create i2c mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = io_manager_get_or_create_bus();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = io_manager_add_device(g_config.i2c_addr, &g_tca9535_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach TCA9535 device: %s", esp_err_to_name(ret));
        if (g_i2c_bus_owned && g_i2c_bus) {
            i2c_del_master_bus(g_i2c_bus);
            g_i2c_bus = NULL;
            g_i2c_bus_owned = false;
        }
        return ret;
    }

    // Configure TCA9535 ports as inputs with pull-ups enabled
    // Note: TCA9535 inputs are high-impedance. If using PCF8575 or similar, writing 1 to output enables weak pull-up.
    // Writing to output register for inputs doesn't hurt TCA9535.
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT0, 0xFF);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Failed to write output port 0");
    
    ret = tca9535_write_port(TCA9535_CONFIG_PORT0, 0xFF);  // All pins as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9535 port 0: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca9535_write_port(TCA9535_CONFIG_PORT1, 0xFF);  // All pins as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9535 port 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Enable pull-ups on input pins (TCA9535 has internal pull-ups)
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT0, 0xFF);  // Set output high to enable pull-ups
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable pull-ups on port 0: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT1, 0xFF);  // Set output high to enable pull-ups
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable pull-ups on port 1: %s", esp_err_to_name(ret));
        return ret;
    }

    io_manager_reset_events();

#ifdef CONFIG_HAS_TLV320DAC_I2C
    /* Pulse DAC RESET line via IO expander P13. */
    ESP_LOGI(TAG, "Pulsing DAC RESET (P13)...");
    esp_err_t reset_ret = io_manager_dac_reset_pulse();
    if (reset_ret != ESP_OK) {
        ESP_LOGW(TAG, "DAC reset pulse failed: %s", esp_err_to_name(reset_ret));
    }
#endif

    if (g_io_task_handle == NULL) {
        BaseType_t task_ret = pdFAIL;
#if defined(CONFIG_SPIRAM)
        if (!g_io_task_stack) {
            g_io_task_stack = heap_caps_malloc(IO_MANAGER_TASK_STACK * sizeof(StackType_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!g_io_task_buffer) {
            g_io_task_buffer = heap_caps_malloc(sizeof(StaticTask_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (g_io_task_stack && g_io_task_buffer) {
            g_io_task_handle = xTaskCreateStatic(
                io_manager_task,
                "io_mgr",
                IO_MANAGER_TASK_STACK,
                NULL,
                IO_MANAGER_TASK_PRIO,
                g_io_task_stack,
                g_io_task_buffer);
            task_ret = g_io_task_handle ? pdPASS : pdFAIL;
            if (task_ret == pdPASS) {
                ESP_LOGI(TAG, "IO manager task stack allocated from PSRAM: %d bytes",
                         (int)(IO_MANAGER_TASK_STACK * sizeof(StackType_t)));
            }
        }
        if (task_ret != pdPASS) {
            free(g_io_task_stack);
            free(g_io_task_buffer);
            g_io_task_stack = NULL;
            g_io_task_buffer = NULL;
#endif
        task_ret = xTaskCreate(
            io_manager_task,
            "io_mgr",
            IO_MANAGER_TASK_STACK,
            NULL,
            IO_MANAGER_TASK_PRIO,
            &g_io_task_handle);
#if defined(CONFIG_SPIRAM)
        }
#endif
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IO manager task");
            g_initialized = false;
            if (g_tca9535_dev) {
                i2c_master_bus_rm_device(g_tca9535_dev);
                g_tca9535_dev = NULL;
            }
            if (g_i2c_bus_owned && g_i2c_bus) {
                i2c_del_master_bus(g_i2c_bus);
                g_i2c_bus = NULL;
                g_i2c_bus_owned = false;
            }
            if (g_i2c_mutex) {
                vSemaphoreDelete(g_i2c_mutex);
                g_i2c_mutex = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    g_initialized = true;

    ESP_LOGI(TAG, "IO Manager initialized successfully");
    ESP_LOGI(TAG, "SDA: %d, SCL: %d, Address: 0x%02X", g_config.sda_pin, g_config.scl_pin, g_config.i2c_addr);

    return ESP_OK;
}

esp_err_t io_manager_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    if (g_io_task_handle) {
        vTaskDelete(g_io_task_handle);
        g_io_task_handle = NULL;
    }

    esp_err_t ret = ESP_OK;
    if (g_tca9535_dev) {
        ret = i2c_master_bus_rm_device(g_tca9535_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to remove TCA9535 device: %s", esp_err_to_name(ret));
            return ret;
        }
        g_tca9535_dev = NULL;
    }

    if (g_i2c_bus_owned && g_i2c_bus) {
        ret = i2c_del_master_bus(g_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete I2C bus: %s", esp_err_to_name(ret));
            return ret;
        }
        g_i2c_bus = NULL;
        g_i2c_bus_owned = false;
    }

    if (g_i2c_mutex) {
        vSemaphoreDelete(g_i2c_mutex);
        g_i2c_mutex = NULL;
    }

    g_initialized = false;
    ESP_LOGI(TAG, "IO Manager deinitialized");

    return ESP_OK;
}

esp_err_t io_manager_scan_buttons(btn_event_t *event)
{
    if (!g_initialized || !event) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t current_port0 = 0, current_port1 = 0;
    esp_err_t ret = tca9535_read_inputs(&current_port0, &current_port1);
    if (ret != ESP_OK) {
        return ret;
    }

    io_manager_process_sample(current_port0, current_port1, event);
    return ESP_OK;
}

esp_err_t io_manager_get_button_states(btn_event_t *states)
{
    if (!g_initialized || !states) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t current_port0 = 0, current_port1 = 0;
    esp_err_t ret = tca9535_read_inputs(&current_port0, &current_port1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read port state: %s", esp_err_to_name(ret));
        return ret;
    }

    io_manager_process_sample(current_port0, current_port1, NULL);

    states->up = !(current_port0 & (1 << BTN_UP));
    states->down = !(current_port0 & (1 << BTN_DOWN));
    states->select = !(current_port0 & (1 << BTN_SELECT));
    states->left = !(current_port0 & (1 << BTN_LEFT));
    states->right = !(current_port0 & (1 << BTN_RIGHT));
    states->b1 = !(current_port1 & (1 << BTN_B1));
    states->b2 = !(current_port1 & (1 << BTN_B2));
    states->b3 = !(current_port1 & (1 << BTN_B3));

    return ESP_OK;
}

esp_err_t io_manager_get_cached_button_states(btn_event_t *states)
{
    if (!g_initialized || !states) {
        return ESP_ERR_INVALID_STATE;
    }

    states->up = !(g_cached_state_port0 & (1 << BTN_UP));
    states->down = !(g_cached_state_port0 & (1 << BTN_DOWN));
    states->select = !(g_cached_state_port0 & (1 << BTN_SELECT));
    states->left = !(g_cached_state_port0 & (1 << BTN_LEFT));
    states->right = !(g_cached_state_port0 & (1 << BTN_RIGHT));
    states->b1 = !(g_cached_state_port1 & (1 << BTN_B1));
    states->b2 = !(g_cached_state_port1 & (1 << BTN_B2));
    states->b3 = !(g_cached_state_port1 & (1 << BTN_B3));

    return ESP_OK;
}

uint8_t io_manager_get_raw_state(void)
{
    if (!g_initialized) {
        return 0xFF;
    }
    return g_last_state_port0;
}

// Debug function to get current button states
void io_manager_debug_states(void)
{
    if (!g_initialized) {
        ESP_LOGW(TAG, "IO Manager not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Debug - PORT0 last: 0x%02X stable: 0x%02X", g_last_state_port0, g_stable_state_port0);
    ESP_LOGI(TAG, "Debug - PORT1 last: 0x%02X stable: 0x%02X", g_last_state_port1, g_stable_state_port1);
    ESP_LOGI(TAG, "Debug - Up:%s Down:%s Select:%s Left:%s Right:%s",
             (g_stable_state_port0 & (1 << BTN_UP)) ? "HIGH" : "LOW",
             (g_stable_state_port0 & (1 << BTN_DOWN)) ? "HIGH" : "LOW",
             (g_stable_state_port0 & (1 << BTN_SELECT)) ? "HIGH" : "LOW",
             (g_stable_state_port0 & (1 << BTN_LEFT)) ? "HIGH" : "LOW",
             (g_stable_state_port0 & (1 << BTN_RIGHT)) ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "Debug - B1:%s B2:%s B3:%s",
             (g_stable_state_port1 & (1 << BTN_B1)) ? "HIGH" : "LOW",
             (g_stable_state_port1 & (1 << BTN_B2)) ? "HIGH" : "LOW",
             (g_stable_state_port1 & (1 << BTN_B3)) ? "HIGH" : "LOW");
}

void io_manager_reset_events(void)
{
    g_last_state_port0 = 0xFF;
    g_stable_state_port0 = 0xFF;
    g_cached_state_port0 = 0xFF;
    g_last_state_port1 = 0xFF;
    g_stable_state_port1 = 0xFF;
    g_cached_state_port1 = 0xFF;
    g_last_change_time = 0;
}

bool io_manager_is_initialized(void)
{
    return g_initialized;
}

bool io_manager_get_encoder_signals(bool *signal_a, bool *signal_b)
{
    if (!g_initialized || !signal_a || !signal_b) {
        return false;
    }
    uint8_t port0 = g_cached_state_port0;
    *signal_a = (port0 & (1 << ENC_PIN_A)) != 0;
    *signal_b = (port0 & (1 << ENC_PIN_B)) != 0;
    return true;
}

bool io_manager_get_encoder_button(void)
{
    if (!g_initialized) {
        return false;
    }
    return !(g_cached_state_port0 & (1 << ENC_PIN_BTN));
}

// Helper function to read from TCA9535 register
static esp_err_t tca9535_read_port(uint8_t reg, uint8_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    // acquire mutex with timeout to prevent deadlock
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "failed to acquire i2c mutex for read");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bool global_locked = i2c_bus_lock(g_config.i2c_port, 60);
        if (!global_locked) {
            ret = ESP_ERR_TIMEOUT;
            continue;
        }
        ret = i2c_master_transmit_receive(g_tca9535_dev, &reg, sizeof(reg), data, 1, 50);
        i2c_bus_unlock(g_config.i2c_port);
        
        if (ret == ESP_OK) {
            break;
        }
    }
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "failed to lock shared i2c bus for read reg 0x%02X", reg);
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

static void io_manager_process_sample(uint8_t port0, uint8_t port1, btn_event_t *event_out)
{
    bool changed = false;
    if (port0 != g_last_state_port0) {
        g_last_state_port0 = port0;
        changed = true;
    }
    if (port1 != g_last_state_port1) {
        g_last_state_port1 = port1;
        changed = true;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (changed) {
        g_cached_state_port0 = port0;
        g_cached_state_port1 = port1;
        g_last_change_time = now_ms;
    }

    bool emit = false;
    btn_event_t tmp = {0};
    if ((now_ms - g_last_change_time) >= DEBOUNCE_MS &&
        (port0 != g_stable_state_port0 || port1 != g_stable_state_port1)) {
        uint8_t prev0 = g_stable_state_port0;
        uint8_t prev1 = g_stable_state_port1;
        g_stable_state_port0 = port0;
        g_stable_state_port1 = port1;

        tmp.up = ((prev0 & (1 << BTN_UP)) && !(port0 & (1 << BTN_UP)));
        tmp.down = ((prev0 & (1 << BTN_DOWN)) && !(port0 & (1 << BTN_DOWN)));
        tmp.select = ((prev0 & (1 << BTN_SELECT)) && !(port0 & (1 << BTN_SELECT)));
        tmp.left = ((prev0 & (1 << BTN_LEFT)) && !(port0 & (1 << BTN_LEFT)));
        tmp.right = ((prev0 & (1 << BTN_RIGHT)) && !(port0 & (1 << BTN_RIGHT)));
        tmp.b1 = ((prev1 & (1 << BTN_B1)) && !(port1 & (1 << BTN_B1)));
        tmp.b2 = ((prev1 & (1 << BTN_B2)) && !(port1 & (1 << BTN_B2)));
        tmp.b3 = ((prev1 & (1 << BTN_B3)) && !(port1 & (1 << BTN_B3)));
        emit = true;
    }

    if (event_out) {
        if (emit) {
            *event_out = tmp;
        } else {
            event_out->up = false;
            event_out->down = false;
            event_out->select = false;
            event_out->left = false;
            event_out->right = false;
            event_out->b1 = false;
            event_out->b2 = false;
            event_out->b3 = false;
        }
    }
}

static void io_manager_task(void *arg)
{
    (void)arg;
    const TickType_t delay = pdMS_TO_TICKS(IO_MANAGER_POLL_MS);
    while (1) {
        uint8_t port0 = g_last_state_port0;
        uint8_t port1 = g_last_state_port1;
        if (tca9535_read_inputs(&port0, &port1) == ESP_OK) {
            // Only update cached states, don't consume button events
            if (port0 != g_last_state_port0) {
                g_last_state_port0 = port0;
                g_cached_state_port0 = port0;
                g_last_change_time = esp_timer_get_time() / 1000;
            }
            if (port1 != g_last_state_port1) {
                g_last_state_port1 = port1;
                g_cached_state_port1 = port1;
                g_last_change_time = esp_timer_get_time() / 1000;
            }
        }
        vTaskDelay(delay);
    }
}

static esp_err_t tca9535_read_inputs(uint8_t *port0, uint8_t *port1)
{
    if (!port0 || !port1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    bool global_locked = i2c_bus_lock(g_config.i2c_port, 60);
    if (!global_locked) {
        xSemaphoreGive(g_i2c_mutex);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - g_last_input_lock_warn_ms >= I2C_LOCK_WARN_INTERVAL_MS) {
            g_last_input_lock_warn_ms = now_ms;
            ESP_LOGW(TAG, "failed to lock shared i2c bus for input read");
        } else {
            ESP_LOGD(TAG, "failed to lock shared i2c bus for input read");
        }
        return ESP_ERR_TIMEOUT;
    }
    uint8_t reg = TCA9535_INPUT_PORT0;
    uint8_t values[2] = {0};
    ret = i2c_master_transmit_receive(g_tca9535_dev, &reg, sizeof(reg), values, sizeof(values), 50);
    if (ret == ESP_OK) {
        *port0 = values[0];
        *port1 = values[1];
    }
    i2c_bus_unlock(g_config.i2c_port);

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

// Helper function to write to TCA9535 register
static esp_err_t tca9535_write_port(uint8_t reg, uint8_t data)
{
    if (!g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    // acquire mutex with timeout to prevent deadlock
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "failed to acquire i2c mutex for write");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bool global_locked = i2c_bus_lock(g_config.i2c_port, 60);
        if (!global_locked) {
            ret = ESP_ERR_TIMEOUT;
            continue;
        }
        uint8_t payload[2] = {reg, data};
        ret = i2c_master_transmit(g_tca9535_dev, payload, sizeof(payload), 50);
        i2c_bus_unlock(g_config.i2c_port);
        
        if (ret == ESP_OK) {
            break;
        }
    }
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "failed to lock shared i2c bus for write reg 0x%02X", reg);
    }

    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t i2c_write_reg8_direct(uint8_t addr, uint8_t reg, uint8_t val)
{
    bool locked = i2c_bus_lock(g_config.i2c_port, 50);
    if (!locked) return ESP_ERR_TIMEOUT;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = io_manager_add_device(addr, &dev);
    if (ret == ESP_OK) {
        uint8_t payload[2] = {reg, val};
        ret = i2c_master_transmit(dev, payload, sizeof(payload), 50);
        i2c_master_bus_rm_device(dev);
    }
    i2c_bus_unlock(g_config.i2c_port);
    return ret;
}

esp_err_t i2c_read_reg8_direct(uint8_t addr, uint8_t reg, uint8_t *val)
{
    bool locked = i2c_bus_lock(g_config.i2c_port, 50);
    if (!locked) return ESP_ERR_TIMEOUT;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = io_manager_add_device(addr, &dev);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit_receive(dev, &reg, sizeof(reg), val, 1, 50);
        i2c_master_bus_rm_device(dev);
    }
    i2c_bus_unlock(g_config.i2c_port);
    return ret;
}

static void scan_single_i2c_port(i2c_port_num_t port, int *device_count, uint8_t *found_addresses, int max_devices)
{
    char row_buf[64];
    
    // scan addresses 1-0x7E to match arduino
    for (uint8_t row = 0; row < 0x8; row++) {
        int pos = snprintf(row_buf, sizeof(row_buf), "%x | ", row);
        for (uint8_t column = 0; column <= 0xf; column++) {
            uint8_t addr_7bit = (row << 4) | column;
            
            // skip reserved addresses (0x00 and 0x7F)
            if (addr_7bit == 0x00 || addr_7bit >= 0x7F) {
                if (pos < (int)sizeof(row_buf) - 2) {
                    row_buf[pos++] = ' ';
                    row_buf[pos++] = ' ';
                }
                continue;
            }

            bool global_locked = i2c_bus_lock(port, 60);
            if (!global_locked) {
                if (pos < (int)sizeof(row_buf) - 2) {
                    row_buf[pos++] = '?';
                    row_buf[pos++] = ' ';
                }
                continue;
            }

            i2c_master_bus_handle_t bus = NULL;
            esp_err_t ret = i2c_master_get_bus_handle(port, &bus);
            if (ret == ESP_OK) {
                ret = i2c_master_probe(bus, addr_7bit, 50);
            }
            i2c_bus_unlock(port);

            if (ret == ESP_OK) {
                if (pos < (int)sizeof(row_buf) - 2) {
                    row_buf[pos++] = '#';
                    row_buf[pos++] = ' ';
                }
                if (*device_count < max_devices) {
                    found_addresses[*device_count] = addr_7bit;
                    (*device_count)++;
                }
            } else {
                if (pos < (int)sizeof(row_buf) - 2) {
                    row_buf[pos++] = '-';
                    row_buf[pos++] = ' ';
                }
            }

            vTaskDelay(1);
        }
        row_buf[pos] = '\0';
        printf("%s\n", row_buf);
        vTaskDelay(1);
    }
}

void io_manager_scan_i2c(void)
{
    if (!g_i2c_mutex) {
        ESP_LOGE(TAG, "i2c not initialized");
        printf("i2c not initialized\n");
        return;
    }

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to acquire i2c mutex for scan");
        printf("failed to acquire i2c mutex\n");
        return;
    }

    printf("\n=== i2c bus scan ===\n");
    printf("SDA=%d SCL=%d | 100kHz | timeout=50ms\n", g_config.sda_pin, g_config.scl_pin);

    int device_count = 0;
    uint8_t found_addresses[120];
    const int max_devices = sizeof(found_addresses) / sizeof(found_addresses[0]);
    
    // scan all available i2c ports (typically 0 and 1)
    #ifdef I2C_NUM_1
    i2c_port_num_t ports_to_scan[] = { I2C_NUM_0, I2C_NUM_1 };
    #else
    i2c_port_num_t ports_to_scan[] = { I2C_NUM_0 };
    #endif
    int num_ports = sizeof(ports_to_scan) / sizeof(ports_to_scan[0]);
    
    for (int i = 0; i < num_ports && device_count < max_devices; i++) {
        i2c_port_num_t port = ports_to_scan[i];
        
        printf("\nscanning i2c port %d...\n", (int)port);
        printf("  | 0 1 2 3 4 5 6 7 8 9 a b c d e f\n");
        printf("--+--------------------------------\n");
        
        scan_single_i2c_port(port, &device_count, found_addresses, max_devices);
        
        printf("\n");
    }

    printf("found %d device(s) total\n", device_count);
    
    if (device_count > 0) {
        printf("\ndetected devices:\n");
        for (int i = 0; i < device_count; i++) {
            printf("  - device at address 0x%02x\n", found_addresses[i]);
            ESP_LOGI(TAG, "found i2c device at address 0x%02x", found_addresses[i]);
        }
    } else {
        printf("no devices found\n");
    }
    
    ESP_LOGI(TAG, "i2c scan complete, found %d devices", device_count);

    xSemaphoreGive(g_i2c_mutex);
}
esp_err_t io_manager_dac_reset_pulse(void)
{
#ifndef CONFIG_TLV320DAC_RESET_IO_EXPANDER_PIN
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!g_tca9535_dev || !g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t reset_pin = CONFIG_TLV320DAC_RESET_IO_EXPANDER_PIN; /* bit on port 1 */
    if (reset_pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t reset_mask = (uint8_t)(1U << reset_pin);

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool global_locked = i2c_bus_lock(g_config.i2c_port, 100);
    if (!global_locked) {
        xSemaphoreGive(g_i2c_mutex);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Step 1: read current port 1 config and output latch */
    uint8_t config1 = 0xFF;
    uint8_t reg = TCA9535_CONFIG_PORT1;
    ret = i2c_master_transmit_receive(g_tca9535_dev, &reg, sizeof(reg), &config1, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read config port1: %s", esp_err_to_name(ret));
        i2c_bus_unlock(g_config.i2c_port);
        xSemaphoreGive(g_i2c_mutex);
        return ret;
    }

    uint8_t output1 = 0xFF;
    reg = TCA9535_OUTPUT_PORT1;
    ret = i2c_master_transmit_receive(g_tca9535_dev, &reg, sizeof(reg), &output1, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read output port1: %s", esp_err_to_name(ret));
        i2c_bus_unlock(g_config.i2c_port);
        xSemaphoreGive(g_i2c_mutex);
        return ret;
    }

    /* Step 2: preload output low, then configure reset pin as output */
    uint8_t payload_low[2] = {TCA9535_OUTPUT_PORT1, (uint8_t)(output1 & ~reset_mask)};
    ret = i2c_master_transmit(g_tca9535_dev, payload_low, sizeof(payload_low), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive reset low: %s", esp_err_to_name(ret));
        i2c_bus_unlock(g_config.i2c_port);
        xSemaphoreGive(g_i2c_mutex);
        return ret;
    }

    uint8_t new_config = config1 & ~reset_mask;
    uint8_t payload_cfg[2] = {TCA9535_CONFIG_PORT1, new_config};
    ret = i2c_master_transmit(g_tca9535_dev, payload_cfg, sizeof(payload_cfg), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set reset pin as output: %s", esp_err_to_name(ret));
        i2c_bus_unlock(g_config.i2c_port);
        xSemaphoreGive(g_i2c_mutex);
        return ret;
    }

    ESP_LOGI(TAG, "DAC reset held low on P13 (port1 bit %d) for 200ms", reset_pin);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 3: drive reset pin HIGH and keep it as an output */
    uint8_t payload_high[2] = {TCA9535_OUTPUT_PORT1, (uint8_t)(output1 | reset_mask)};
    ret = i2c_master_transmit(g_tca9535_dev, payload_high, sizeof(payload_high), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive reset high: %s", esp_err_to_name(ret));
        i2c_bus_unlock(g_config.i2c_port);
        xSemaphoreGive(g_i2c_mutex);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_bus_unlock(g_config.i2c_port);
    xSemaphoreGive(g_i2c_mutex);

    ESP_LOGI(TAG, "DAC reset pulse complete (200ms low, held high)");
    return ESP_OK;
#endif
}

static esp_err_t io_manager_get_or_create_bus(void)
{
    return i2c_shared_get_or_create_bus(g_config.i2c_port,
                                        g_config.sda_pin,
                                        g_config.scl_pin,
                                        true,
                                        &g_i2c_bus,
                                        &g_i2c_bus_owned);
}

static esp_err_t io_manager_add_device(uint8_t addr, i2c_master_dev_handle_t *dev_out)
{
    if (!g_i2c_bus || !dev_out) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    return i2c_master_bus_add_device(g_i2c_bus, &dev_config, dev_out);
}
