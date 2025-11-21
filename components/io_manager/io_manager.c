#include "io_manager.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "i2c_bus_lock.h"

static const char *TAG = "IO_MANAGER";
static bool g_i2c_driver_installed = false; // whether this module installed the I2C driver
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

// Forward declarations
static esp_err_t tca9535_read_port(uint8_t reg, uint8_t *data);
static esp_err_t tca9535_read_inputs(uint8_t *port0, uint8_t *port1);
static esp_err_t tca9535_write_port(uint8_t reg, uint8_t data);
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

    // Configure/Install I2C
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = g_config.sda_pin,
        .scl_io_num = g_config.scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        // Keep shared port at 100 kHz so PN532 modules can coexist with the expander/status display.
        .master.clk_speed = 100000,
    };

    // Try to install the driver first; if it's already installed, skip param_config
    esp_err_t ret = i2c_driver_install(g_config.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        g_i2c_driver_installed = true;
        // We own the driver: safe to configure pins/clock
        ret = i2c_param_config(g_config.i2c_port, &i2c_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
            return ret;
        }
    } else if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
        // Driver already installed elsewhere; share without reconfiguring
        ESP_LOGW(TAG, "I2C driver already installed on port %d; sharing", g_config.i2c_port);
        g_i2c_driver_installed = false;
    } else {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
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

    if (g_io_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(
            io_manager_task,
            "io_mgr",
            IO_MANAGER_TASK_STACK,
            NULL,
            IO_MANAGER_TASK_PRIO,
            &g_io_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IO manager task");
            g_initialized = false;
            if (g_i2c_driver_installed) {
                i2c_driver_delete(g_config.i2c_port);
                g_i2c_driver_installed = false;
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
    if (g_i2c_driver_installed) {
        ret = i2c_driver_delete(g_config.i2c_port);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete I2C driver: %s", esp_err_to_name(ret));
            return ret;
        }
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
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            ret = ESP_ERR_NO_MEM;
            i2c_bus_unlock(g_config.i2c_port);
            break;
        }
        
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(g_config.i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
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
            io_manager_process_sample(port0, port1, NULL);
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
        ESP_LOGW(TAG, "failed to lock shared i2c bus for input read");
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd) {
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, TCA9535_INPUT_PORT0, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, port0, I2C_MASTER_ACK);
        i2c_master_read_byte(cmd, port1, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(g_config.i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
    } else {
        ret = ESP_ERR_NO_MEM;
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
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            ret = ESP_ERR_NO_MEM;
            i2c_bus_unlock(g_config.i2c_port);
            break;
        }
        
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (g_config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_write_byte(cmd, data, true);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(g_config.i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
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
