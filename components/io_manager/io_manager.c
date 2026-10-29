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

// Expander register addresses.
// TCA9535 / TCA9555 and the AW9523B share the input (0x00/0x01) and output
// (0x02/0x03) register layout; only the direction registers and the
// chip-specific extras differ, so the whole register set is selected by the
// chip variant instead of being hardcoded.
#define TCA9535_INPUT_PORT0     0x00
#define TCA9535_INPUT_PORT1     0x01
#define TCA9535_OUTPUT_PORT0    0x02
#define TCA9535_OUTPUT_PORT1    0x03
#ifdef CONFIG_IO_EXPANDER_CHIP_AW9523
#define TCA9535_CONFIG_PORT0    0x04
#define TCA9535_CONFIG_PORT1    0x05
#define AW9523_REG_ID           0x10
#define AW9523_REG_LEDMODE_P0   0x12
#define AW9523_REG_LEDMODE_P1   0x13
#define AW9523_REG_SOFT_RESET   0x7F
#define AW9523_ID_VALUE         0x23
/* Status RGB LED on P1: P10 red, P11 green, P12 blue, common anode, so a
 * cleared bit lights the channel. P13 is the touch reset and must stay high. */
#define AW9523_P1_LED_RED       0x01
#define AW9523_P1_LED_GREEN     0x02
#define AW9523_P1_LED_BLUE      0x04
#define AW9523_P1_LED_MASK      0x07
/* Constant-current dimming, 256 steps per pin (AW9523B datasheet "Table 13,
 * 256 step dimming control"). The DIM registers are NOT in pin order: pins
 * 0-7 use 0x24+pin, pins 8-11 use 0x20+(pin-8) and pins 12-15 use
 * 0x2C+(pin-12). P10/P11/P12 are pins 8/9/10, hence 0x20/0x21/0x22. */
#define AW9523_REG_DIM_RED      0x20   /* pin 8  = P10 */
#define AW9523_REG_DIM_GREEN    0x21   /* pin 9  = P11 */
#define AW9523_REG_DIM_BLUE     0x22   /* pin 10 = P12 */
static uint8_t g_out_p1_shadow = 0xFF;   /* last value written to P1 outputs */
static uint8_t g_led_dim[3] = {0, 0, 0}; /* red/green/blue as last written */
static bool g_led_dim_mode = false;      /* P10-P12 in constant-current mode */
#else
#define TCA9535_CONFIG_PORT0    0x06
#define TCA9535_CONFIG_PORT1    0x07
#endif

// Button-to-bit map.
//
// Both expander families expose their inputs as two 8-bit port registers, but
// they wire their buttons to different bits, so the map is data rather than a
// set of #defines: each logical button is a (port, bit) pair and every read
// path below goes through the helpers. Every button is active-low.
typedef struct {
    uint8_t port;   // 0 = P0 register (0x00), 1 = P1 register (0x01)
    uint8_t bit;    // bit position inside that register
} io_bit_t;

typedef struct {
    io_bit_t up, down, select, left, right;
    io_bit_t b1, b2, b3;   // extra face buttons (L1 / R1 / POWER on Attaky)
    uint8_t dir_p0, dir_p1;   // direction masks written at init (1 = input)
    uint8_t out_p0, out_p1;   // output values written at init
} io_manager_map_t;

#ifdef CONFIG_IO_EXPANDER_CHIP_AW9523
// AW9523B: the Attaky Core's button expander @ 0x59. All eight buttons sit on
// P0 - P00 Down, P01 Left, P02 Enter/Select, P03 Right, P04 Up, P05 L1,
// P06 R1, P07 POWER (short press; a long press is a hardware power toggle the
// PMIC owns). P1 drives the RGB status LED (P10-P12) and the touch reset
// (P13), so P1's low nibble must stay an output driving high: writing a
// whole-bank "all inputs" direction there would float the touch reset line.
// P1's high nibble (P14-P17) is unused and left as input.
static const io_manager_map_t g_io_map = {
    .up = {0, 4}, .down = {0, 0}, .select = {0, 2}, .left = {0, 1}, .right = {0, 3},
    .b1 = {0, 5}, .b2 = {0, 6}, .b3 = {0, 7},
    .dir_p0 = 0xFF, .dir_p1 = 0xF0,
    .out_p0 = 0xFF, .out_p1 = 0x0F,   // LED off + touch reset released (active low)
};
#else
// TCA9535 / TCA9555 default map (upstream's wiring: D-pad + select on P0,
// b1-b3 on P1, encoder A/B/button on P05-P07).
static const io_manager_map_t g_io_map = {
    .up = {0, 0}, .down = {0, 1}, .select = {0, 2}, .left = {0, 3}, .right = {0, 4},
    .b1 = {1, 0}, .b2 = {1, 1}, .b3 = {1, 2},
    .dir_p0 = 0xFF, .dir_p1 = 0xFF,
    .out_p0 = 0xFF, .out_p1 = 0xFF,
};
#endif

static inline bool io_bit_pressed(io_bit_t b, uint8_t port0, uint8_t port1)
{
    uint8_t v = (b.port == 0) ? port0 : port1;
    return (v & (uint8_t)(1u << b.bit)) == 0;   // active-low
}

static inline bool io_bit_falling_edge(io_bit_t b, uint8_t prev0, uint8_t prev1,
                                       uint8_t now0, uint8_t now1)
{
    uint8_t prev = (b.port == 0) ? prev0 : prev1;
    uint8_t now = (b.port == 0) ? now0 : now1;
    uint8_t mask = (uint8_t)(1u << b.bit);
    return (prev & mask) && !(now & mask);
}

// Encoder pins on port0 (only meaningful when an encoder is fitted)
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
static StackType_t *g_io_task_stack = NULL;
static StaticTask_t *g_io_task_buffer = NULL;

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

#ifdef CONFIG_IO_EXPANDER_CHIP_AW9523
    // The AW9523B powers up with its P-pins configured as outputs (the exact
    // pattern depends on the AD0/AD1 address strap), so a software reset and
    // explicit direction writes are mandatory before the first read - otherwise
    // the expander drives the button lines itself and every button reads as
    // pressed. LEDMODE is written to 0xFF on both banks so P0/P1 are plain
    // GPIO outputs rather than constant-current LED sinks; the status LED
    // moves its own three pins into sink mode on first use (see
    // io_manager_set_status_led).
    ret = tca9535_write_port(AW9523_REG_SOFT_RESET, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AW9523 software reset failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(2));   // datasheet does not specify; 1-2 ms is safe
    if (tca9535_write_port(AW9523_REG_LEDMODE_P0, 0xFF) != ESP_OK ||
        tca9535_write_port(AW9523_REG_LEDMODE_P1, 0xFF) != ESP_OK) {
        ESP_LOGW(TAG, "AW9523 LEDMODE init failed; LED/reset pins may stay in sink mode");
    }
    uint8_t id = 0;
    if (tca9535_read_port(AW9523_REG_ID, &id) == ESP_OK) {
        if (id != AW9523_ID_VALUE) {
            ESP_LOGW(TAG, "Expander at 0x%02X reports ID 0x%02X (expected 0x%02X)",
                     g_config.i2c_addr, id, AW9523_ID_VALUE);
        }
    } else {
        ESP_LOGW(TAG, "Expander at 0x%02X did not answer its ID register", g_config.i2c_addr);
    }
#endif

    // Configure the expander from the selected button map.
    // Note: inputs are high-impedance. Writing 1 to the output register for a
    // high-impedance input is harmless on both families and is kept because it
    // enables the weak pull-up on PCF8575-style parts.
    ret = tca9535_write_port(TCA9535_OUTPUT_PORT0, g_io_map.out_p0);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Failed to write output port 0");

    ret = tca9535_write_port(TCA9535_CONFIG_PORT0, g_io_map.dir_p0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure expander port 0: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca9535_write_port(TCA9535_CONFIG_PORT1, g_io_map.dir_p1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure expander port 1: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca9535_write_port(TCA9535_OUTPUT_PORT1, g_io_map.out_p1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set expander port 1 outputs: %s", esp_err_to_name(ret));
        return ret;
    }
#ifdef CONFIG_IO_EXPANDER_CHIP_AW9523
    g_out_p1_shadow = g_io_map.out_p1;   /* LED off, touch reset released */
#endif

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

    states->up = io_bit_pressed(g_io_map.up, current_port0, current_port1);
    states->down = io_bit_pressed(g_io_map.down, current_port0, current_port1);
    states->select = io_bit_pressed(g_io_map.select, current_port0, current_port1);
    states->left = io_bit_pressed(g_io_map.left, current_port0, current_port1);
    states->right = io_bit_pressed(g_io_map.right, current_port0, current_port1);
    states->b1 = io_bit_pressed(g_io_map.b1, current_port0, current_port1);
    states->b2 = io_bit_pressed(g_io_map.b2, current_port0, current_port1);
    states->b3 = io_bit_pressed(g_io_map.b3, current_port0, current_port1);

    return ESP_OK;
}

esp_err_t io_manager_get_cached_button_states(btn_event_t *states)
{
    if (!g_initialized || !states) {
        return ESP_ERR_INVALID_STATE;
    }

    states->up = io_bit_pressed(g_io_map.up, g_cached_state_port0, g_cached_state_port1);
    states->down = io_bit_pressed(g_io_map.down, g_cached_state_port0, g_cached_state_port1);
    states->select = io_bit_pressed(g_io_map.select, g_cached_state_port0, g_cached_state_port1);
    states->left = io_bit_pressed(g_io_map.left, g_cached_state_port0, g_cached_state_port1);
    states->right = io_bit_pressed(g_io_map.right, g_cached_state_port0, g_cached_state_port1);
    states->b1 = io_bit_pressed(g_io_map.b1, g_cached_state_port0, g_cached_state_port1);
    states->b2 = io_bit_pressed(g_io_map.b2, g_cached_state_port0, g_cached_state_port1);
    states->b3 = io_bit_pressed(g_io_map.b3, g_cached_state_port0, g_cached_state_port1);

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
             io_bit_pressed(g_io_map.up, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.down, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.select, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.left, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.right, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH");
    ESP_LOGI(TAG, "Debug - B1:%s B2:%s B3:%s",
             io_bit_pressed(g_io_map.b1, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.b2, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH",
             io_bit_pressed(g_io_map.b3, g_stable_state_port0, g_stable_state_port1) ? "LOW" : "HIGH");
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

        tmp.up = io_bit_falling_edge(g_io_map.up, prev0, prev1, port0, port1);
        tmp.down = io_bit_falling_edge(g_io_map.down, prev0, prev1, port0, port1);
        tmp.select = io_bit_falling_edge(g_io_map.select, prev0, prev1, port0, port1);
        tmp.left = io_bit_falling_edge(g_io_map.left, prev0, prev1, port0, port1);
        tmp.right = io_bit_falling_edge(g_io_map.right, prev0, prev1, port0, port1);
        tmp.b1 = io_bit_falling_edge(g_io_map.b1, prev0, prev1, port0, port1);
        tmp.b2 = io_bit_falling_edge(g_io_map.b2, prev0, prev1, port0, port1);
        tmp.b3 = io_bit_falling_edge(g_io_map.b3, prev0, prev1, port0, port1);
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

    // Take the shared per-port bus lock so this poll is serialized with the
    // other bus users (touch screen reads, etc.).
    bool locked = i2c_bus_lock(g_config.i2c_port, 50);

    uint8_t reg = TCA9535_INPUT_PORT0;
    uint8_t values[2] = {0};
    esp_err_t ret = locked ? i2c_master_transmit_receive(g_tca9535_dev, &reg, sizeof(reg), values, sizeof(values), 50)
                           : ESP_ERR_TIMEOUT;
    if (locked) {
        i2c_bus_unlock(g_config.i2c_port);
    }
    if (ret == ESP_OK) {
        *port0 = values[0];
        *port1 = values[1];
    }
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
esp_err_t io_manager_set_status_led(uint8_t red, uint8_t green, uint8_t blue)
{
#ifdef CONFIG_IO_EXPANDER_CHIP_AW9523
    if (!g_initialized || !g_tca9535_dev || !g_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (!g_led_dim_mode) {
        /* Dark first: once the pins stop being GPIO their brightness comes from
         * the DIM registers, and their power-on value is not zero. */
        ret = tca9535_write_port(AW9523_REG_DIM_RED, 0);
        if (ret == ESP_OK) ret = tca9535_write_port(AW9523_REG_DIM_GREEN, 0);
        if (ret == ESP_OK) ret = tca9535_write_port(AW9523_REG_DIM_BLUE, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "status LED dimming unavailable: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Only P10-P12 join the constant-current bank. P13 is the touch reset
         * and P14/P15 are unused, so this is a read-modify-write: a blind 0x00
         * would drop the touch controller into reset. */
        uint8_t mode = 0xFF;
        if (tca9535_read_port(AW9523_REG_LEDMODE_P1, &mode) == ESP_OK) {
            mode = (uint8_t)(mode & (uint8_t)~AW9523_P1_LED_MASK);
            if (tca9535_write_port(AW9523_REG_LEDMODE_P1, mode) != ESP_OK) {
                ESP_LOGW(TAG, "failed to switch the status LED to constant current");
            }
        }

        /* In that mode a pin sinks while its output bit is low, and the DIM
         * value sets the brightness. Hold the three bits low and keep the
         * shadow in step, because the touch-reset path rewrites this port from
         * the shadow. */
        g_out_p1_shadow = (uint8_t)(g_out_p1_shadow & (uint8_t)~AW9523_P1_LED_MASK);
        ret = tca9535_write_port(TCA9535_OUTPUT_PORT1, g_out_p1_shadow);
        if (ret != ESP_OK) {
            return ret;
        }

        g_led_dim_mode = true;
    }

    const uint8_t regs[3] = {AW9523_REG_DIM_RED, AW9523_REG_DIM_GREEN, AW9523_REG_DIM_BLUE};
    const uint8_t values[3] = {red, green, blue};
    for (int i = 0; i < 3; i++) {
        if (g_led_dim[i] == values[i]) {
            continue;   /* no change: avoid pointless I2C traffic in effects */
        }
        ret = tca9535_write_port(regs[i], values[i]);
        if (ret != ESP_OK) {
            return ret;
        }
        g_led_dim[i] = values[i];
    }

    return ESP_OK;
#else
    (void)red;
    (void)green;
    (void)blue;
    return ESP_ERR_NOT_SUPPORTED;
#endif
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
