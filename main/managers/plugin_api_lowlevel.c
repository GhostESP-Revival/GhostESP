#include "managers/plugin_api_internal.h"
#include "managers/fuel_gauge_manager.h"
#include "managers/joystick_manager.h"
#include "managers/settings_manager.h"
#include "managers/microphone/mic_driver.h"
#include "managers/zigbee_manager.h"
#include "managers/ble_manager.h"
#include "managers/ethernet_manager.h"
#include "managers/infrared_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/views/nfc_view.h"
#include "managers/nrf24_remote_manager.h"
#include "core/uart_share.h"
#include "i2c_bus_lock.h"
#include "vendor/pcap.h"

#if CONFIG_HAS_CAMERA
#include "managers/camera_stream_manager.h"
#include "esp_camera.h"
#endif

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#endif

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#include "managers/hid_script_parser.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#endif

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PLUGIN_SPI_MAX_DEVICES 4
#define PLUGIN_PWM_MAX_CHANNELS 4
#define PLUGIN_TASK_MAX 8
#define PLUGIN_SOCKET_MAX 16
#define PLUGIN_EVENT_MAX 16
#define PLUGIN_I2C_PORT I2C_NUM_0
#define PLUGIN_ADC_CHANNEL_MAX 10

typedef struct {
    bool active;
    spi_device_handle_t dev;
    spi_host_device_t host;
    bool bus_owned;
} plugin_spi_slot_t;

typedef struct {
    bool active;
    int pin;
    ledc_channel_t channel;
    ledc_timer_t timer;
    uint8_t resolution;
} plugin_pwm_slot_t;

typedef struct {
    bool active;
    ghostesp_gpio_intr_cb_t cb;
    void *user;
} plugin_gpio_intr_slot_t;

typedef struct {
    bool active;
    char topic[32];
    ghostesp_event_cb_t cb;
    void *user;
} plugin_event_slot_t;

static plugin_spi_slot_t s_spi_slots[PLUGIN_SPI_MAX_DEVICES];
static plugin_pwm_slot_t s_pwm_slots[PLUGIN_PWM_MAX_CHANNELS];
static plugin_gpio_intr_slot_t *s_gpio_intr = NULL;
static plugin_event_slot_t *s_events = NULL;

static bool ensure_gpio_intr_slots(void) {
    if (!s_gpio_intr) s_gpio_intr = calloc(GPIO_NUM_MAX, sizeof(*s_gpio_intr));
    return s_gpio_intr != NULL;
}

static bool ensure_event_slots(void) {
    if (!s_events) s_events = calloc(PLUGIN_EVENT_MAX, sizeof(*s_events));
    return s_events != NULL;
}
static TaskHandle_t s_tasks[PLUGIN_TASK_MAX];
static int s_sockets[PLUGIN_SOCKET_MAX];
static bool s_uart_open[UART_NUM_MAX];
static bool s_gpio_isr_installed = false;
static ghostesp_wifi_packet_cb_t s_wifi_packet_cb = NULL;
static void *s_wifi_packet_user = NULL;
static bool s_wifi_pcap_active = false;
static uint8_t s_display_brightness = 100;
static adc_oneshot_unit_handle_t s_adc_unit = NULL;
static bool s_adc_channels[PLUGIN_ADC_CHANNEL_MAX];

#ifndef CONFIG_IDF_TARGET_ESP32S2
static uint16_t s_ble_gatt_conn_handle = 0xffff;
static SemaphoreHandle_t s_ble_gatt_sem = NULL;
static uint8_t *s_ble_gatt_read_buf = NULL;
static size_t s_ble_gatt_read_len = 0;
static int s_ble_gatt_error = 0;
static bool s_ble_gatt_found_char = false;
static uint16_t s_ble_gatt_char_handle = 0;

static int plugin_ble_gatt_gap_cb(struct ble_gap_event *event, void *arg);
#endif

extern joystick_t joysticks[5];
extern void set_backlight_brightness(uint8_t value);
extern FSettings G_Settings;

static void socket_slots_init(void) {
    static bool initialized = false;
    if (initialized) return;
    for (int i = 0; i < PLUGIN_SOCKET_MAX; i++) s_sockets[i] = -1;
    initialized = true;
}

static void track_socket(int sock) {
    socket_slots_init();
    if (sock < 0) return;
    for (int i = 0; i < PLUGIN_SOCKET_MAX; i++) {
        if (s_sockets[i] == sock) return;
        if (s_sockets[i] < 0) {
            s_sockets[i] = sock;
            return;
        }
    }
}

static void untrack_socket(int sock) {
    socket_slots_init();
    for (int i = 0; i < PLUGIN_SOCKET_MAX; i++) if (s_sockets[i] == sock) s_sockets[i] = -1;
}

static bool has_permission(plugin_permission_t permission) {
    return plugin_api_internal_has_permission(permission);
}

static bool valid_pin(int pin) {
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static bool reserved_pin(int pin) {
    if (!valid_pin(pin)) return true;
#ifdef CONFIG_L_BTN
    if (pin == CONFIG_L_BTN) return true;
#endif
#ifdef CONFIG_C_BTN
    if (pin == CONFIG_C_BTN) return true;
#endif
#ifdef CONFIG_U_BTN
    if (pin == CONFIG_U_BTN) return true;
#endif
#ifdef CONFIG_R_BTN
    if (pin == CONFIG_R_BTN) return true;
#endif
#ifdef CONFIG_D_BTN
    if (pin == CONFIG_D_BTN) return true;
#endif
#ifdef CONFIG_SD_CARD_PIN_CS
    if (pin == CONFIG_SD_CARD_PIN_CS) return true;
#endif
#ifdef CONFIG_SD_CARD_PIN_CLK
    if (pin == CONFIG_SD_CARD_PIN_CLK) return true;
#endif
#ifdef CONFIG_SD_CARD_PIN_MISO
    if (pin == CONFIG_SD_CARD_PIN_MISO) return true;
#endif
#ifdef CONFIG_SD_CARD_PIN_MOSI
    if (pin == CONFIG_SD_CARD_PIN_MOSI) return true;
#endif
#ifdef CONFIG_SUBGHZ_GDO0_PIN
    if (pin == CONFIG_SUBGHZ_GDO0_PIN) return true;
#endif
    return false;
}

static TickType_t ticks(uint32_t timeout_ms) {
    return timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static bool ghostesp_path_allowed(const char *path) {
    return plugin_api_internal_absolute_storage_allowed(path);
}

static bool mkdir_recursive_path(const char *path) {
    if (!path || !path[0]) return false;
    char tmp[PLUGIN_APP_PATH_MAX];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return false;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0] != '\0' && mkdir(tmp, 0775) != 0) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                *p = '/';
                return false;
            }
        }
        *p = '/';
    }
    if (mkdir(tmp, 0775) != 0) {
        struct stat st;
        return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return true;
}

static bool stat_path(const char *path, ghostesp_storage_stat_t *out) {
    if (!path || !out) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    out->size = (uint64_t)st.st_size;
    out->is_directory = S_ISDIR(st.st_mode);
    return true;
}

bool plugin_api_gpio_set_mode(int pin, uint32_t mode) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin)) return false;
    return gpio_set_direction((gpio_num_t)pin, (gpio_mode_t)mode) == ESP_OK;
}

bool plugin_api_gpio_write(int pin, int level) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin)) return false;
    return gpio_set_level((gpio_num_t)pin, level ? 1 : 0) == ESP_OK;
}

int plugin_api_gpio_read(int pin) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin)) return -1;
    return gpio_get_level((gpio_num_t)pin);
}

bool plugin_api_gpio_set_pull(int pin, bool pullup, bool pulldown) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin)) return false;
    if (gpio_set_pull_mode((gpio_num_t)pin, pullup && pulldown ? GPIO_PULLUP_PULLDOWN : pullup ? GPIO_PULLUP_ONLY : pulldown ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING) != ESP_OK) return false;
    return true;
}

bool plugin_api_gpio_set_drive_strength(int pin, int strength) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin)) return false;
    if (strength < GPIO_DRIVE_CAP_0 || strength > GPIO_DRIVE_CAP_3) return false;
    return gpio_set_drive_capability((gpio_num_t)pin, (gpio_drive_cap_t)strength) == ESP_OK;
}

static void IRAM_ATTR gpio_intr_bridge(void *arg) {
    int pin = (int)(intptr_t)arg;
    if (pin < 0 || pin >= GPIO_NUM_MAX || !s_gpio_intr) return;
    plugin_gpio_intr_slot_t *slot = &s_gpio_intr[pin];
    if (slot->active && slot->cb) slot->cb(pin, gpio_get_level((gpio_num_t)pin), slot->user);
}

bool plugin_api_gpio_set_intr(int pin, int edge, ghostesp_gpio_intr_cb_t cb, void *user) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || reserved_pin(pin) || !cb) return false;
    if (!s_gpio_isr_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
        s_gpio_isr_installed = true;
    }
    if (!ensure_gpio_intr_slots()) return false;
    if (gpio_set_intr_type((gpio_num_t)pin, (gpio_int_type_t)edge) != ESP_OK) return false;
    gpio_isr_handler_remove((gpio_num_t)pin);
    if (gpio_isr_handler_add((gpio_num_t)pin, gpio_intr_bridge, (void *)(intptr_t)pin) != ESP_OK) return false;
    s_gpio_intr[pin].active = true;
    s_gpio_intr[pin].cb = cb;
    s_gpio_intr[pin].user = user;
    return true;
}

bool plugin_api_gpio_clear_intr(int pin) {
    if (!has_permission(PLUGIN_PERMISSION_RAW_GPIO) || !valid_pin(pin)) return false;
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    if (s_gpio_intr) memset(&s_gpio_intr[pin], 0, sizeof(*s_gpio_intr));
    return true;
}

bool plugin_api_uart_open(int uart_num, int tx_pin, int rx_pin, uint32_t baud) {
    if (!has_permission(PLUGIN_PERMISSION_UART) || uart_num <= UART_NUM_0 || uart_num >= UART_NUM_MAX || baud == 0) return false;
    if (reserved_pin(tx_pin) || reserved_pin(rx_pin)) return false;
    uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_share_ensure_installed((uart_port_t)uart_num, 2048, 2048, 0) != ESP_OK) return false;
    if (uart_share_acquire((uart_port_t)uart_num, UART_SHARE_OWNER_PLUGIN, pdMS_TO_TICKS(250)) != ESP_OK) return false;
    if (uart_param_config((uart_port_t)uart_num, &cfg) != ESP_OK) {
        uart_share_release((uart_port_t)uart_num, UART_SHARE_OWNER_PLUGIN);
        return false;
    }
    if (uart_set_pin((uart_port_t)uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_share_release((uart_port_t)uart_num, UART_SHARE_OWNER_PLUGIN);
        return false;
    }
    s_uart_open[uart_num] = true;
    return true;
}

int plugin_api_uart_write(int uart_num, const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_UART) || uart_num <= UART_NUM_0 || uart_num >= UART_NUM_MAX || (!data && len > 0)) return -1;
    return uart_write_bytes((uart_port_t)uart_num, data, len);
}

int plugin_api_uart_read(int uart_num, void *buffer, size_t len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_UART) || uart_num <= UART_NUM_0 || uart_num >= UART_NUM_MAX || !buffer || len == 0) return -1;
    return uart_read_bytes((uart_port_t)uart_num, buffer, len, ticks(timeout_ms));
}

bool plugin_api_uart_close(int uart_num) {
    if (!has_permission(PLUGIN_PERMISSION_UART) || uart_num <= UART_NUM_0 || uart_num >= UART_NUM_MAX) return false;
    s_uart_open[uart_num] = false;
    return uart_share_release((uart_port_t)uart_num, UART_SHARE_OWNER_PLUGIN) == ESP_OK;
}

static bool i2c_with_device(uint8_t addr, uint32_t timeout_ms, i2c_master_dev_handle_t *out) {
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(PLUGIN_I2C_PORT, &bus) != ESP_OK || !bus) return false;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (!i2c_bus_lock(PLUGIN_I2C_PORT, timeout_ms)) return false;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, out);
    if (err != ESP_OK) i2c_bus_unlock(PLUGIN_I2C_PORT);
    return err == ESP_OK;
}

bool plugin_api_i2c_probe(uint8_t addr, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_I2C)) return false;
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(PLUGIN_I2C_PORT, &bus) != ESP_OK || !bus) return false;
    if (!i2c_bus_lock(PLUGIN_I2C_PORT, timeout_ms)) return false;
    esp_err_t err = i2c_master_probe(bus, addr, (int)timeout_ms);
    i2c_bus_unlock(PLUGIN_I2C_PORT);
    return err == ESP_OK;
}

bool plugin_api_i2c_write(uint8_t addr, const void *data, size_t len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_I2C) || (!data && len > 0)) return false;
    i2c_master_dev_handle_t dev = NULL;
    if (!i2c_with_device(addr, timeout_ms, &dev)) return false;
    esp_err_t err = i2c_master_transmit(dev, data, len, (int)timeout_ms);
    i2c_master_bus_rm_device(dev);
    i2c_bus_unlock(PLUGIN_I2C_PORT);
    return err == ESP_OK;
}

int plugin_api_i2c_read(uint8_t addr, void *buffer, size_t len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_I2C) || !buffer || len == 0) return -1;
    i2c_master_dev_handle_t dev = NULL;
    if (!i2c_with_device(addr, timeout_ms, &dev)) return -1;
    esp_err_t err = i2c_master_receive(dev, buffer, len, (int)timeout_ms);
    i2c_master_bus_rm_device(dev);
    i2c_bus_unlock(PLUGIN_I2C_PORT);
    return err == ESP_OK ? (int)len : -1;
}

bool plugin_api_i2c_write_read(uint8_t addr, const void *tx, size_t tx_len, void *rx, size_t rx_len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_I2C) || !tx || tx_len == 0 || !rx || rx_len == 0) return false;
    i2c_master_dev_handle_t dev = NULL;
    if (!i2c_with_device(addr, timeout_ms, &dev)) return false;
    esp_err_t err = i2c_master_transmit_receive(dev, tx, tx_len, rx, rx_len, (int)timeout_ms);
    i2c_master_bus_rm_device(dev);
    i2c_bus_unlock(PLUGIN_I2C_PORT);
    return err == ESP_OK;
}

int plugin_api_spi_open(int host, int sclk, int miso, int mosi, int cs, uint32_t hz, int mode) {
    if (!has_permission(PLUGIN_PERMISSION_SPI) || hz == 0 || reserved_pin(sclk) || reserved_pin(miso) || reserved_pin(mosi) || reserved_pin(cs)) return -1;
    int slot = -1;
    for (int i = 0; i < PLUGIN_SPI_MAX_DEVICES; i++) if (!s_spi_slots[i].active) { slot = i; break; }
    if (slot < 0) return -1;
    spi_host_device_t spi_host = (spi_host_device_t)host;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    bool owned = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)hz,
        .mode = mode,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(spi_host, &dev_cfg, &dev);
    if (err != ESP_OK) {
        if (owned) spi_bus_free(spi_host);
        return -1;
    }
    s_spi_slots[slot] = (plugin_spi_slot_t){ .active = true, .dev = dev, .host = spi_host, .bus_owned = owned };
    return slot + 1;
}

int plugin_api_spi_transfer(int handle, const void *tx, void *rx, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_SPI) || handle <= 0 || handle > PLUGIN_SPI_MAX_DEVICES || len == 0) return -1;
    plugin_spi_slot_t *slot = &s_spi_slots[handle - 1];
    if (!slot->active || !slot->dev) return -1;
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(slot->dev, &t) == ESP_OK ? (int)len : -1;
}

bool plugin_api_spi_close(int handle) {
    if (!has_permission(PLUGIN_PERMISSION_SPI) || handle <= 0 || handle > PLUGIN_SPI_MAX_DEVICES) return false;
    plugin_spi_slot_t *slot = &s_spi_slots[handle - 1];
    if (!slot->active) return false;
    if (slot->dev) spi_bus_remove_device(slot->dev);
    if (slot->bus_owned) spi_bus_free(slot->host);
    memset(slot, 0, sizeof(*slot));
    return true;
}

static bool ensure_adc_channel(int channel) {
    if (channel < 0 || channel >= PLUGIN_ADC_CHANNEL_MAX) return false;
    if (!s_adc_unit) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&init_cfg, &s_adc_unit) != ESP_OK) return false;
    }
    if (!s_adc_channels[channel]) {
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_oneshot_config_channel(s_adc_unit, (adc_channel_t)channel, &chan_cfg) != ESP_OK) return false;
        s_adc_channels[channel] = true;
    }
    return true;
}

int plugin_api_adc_read_raw(int channel) {
    if (!has_permission(PLUGIN_PERMISSION_ADC) || !ensure_adc_channel(channel)) return -1;
    int raw = 0;
    return adc_oneshot_read(s_adc_unit, (adc_channel_t)channel, &raw) == ESP_OK ? raw : -1;
}

int plugin_api_adc_read_mv(int channel) {
    return plugin_api_adc_read_raw(channel);
}

bool plugin_api_pwm_attach(int pin, uint32_t freq_hz, uint8_t resolution_bits) {
    if (!has_permission(PLUGIN_PERMISSION_PWM) || reserved_pin(pin) || freq_hz == 0 || resolution_bits == 0 || resolution_bits > 14) return false;
    int slot = -1;
    for (int i = 0; i < PLUGIN_PWM_MAX_CHANNELS; i++) if (!s_pwm_slots[i].active || s_pwm_slots[i].pin == pin) { slot = i; break; }
    if (slot < 0) return false;
    ledc_timer_t timer = (ledc_timer_t)slot;
    ledc_channel_t channel = (ledc_channel_t)slot;
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)resolution_bits,
        .timer_num = timer,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK) return false;
    ledc_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&chan_cfg) != ESP_OK) return false;
    s_pwm_slots[slot] = (plugin_pwm_slot_t){ .active = true, .pin = pin, .channel = channel, .timer = timer, .resolution = resolution_bits };
    return true;
}

bool plugin_api_pwm_write(int pin, uint32_t duty) {
    if (!has_permission(PLUGIN_PERMISSION_PWM)) return false;
    for (int i = 0; i < PLUGIN_PWM_MAX_CHANNELS; i++) {
        if (!s_pwm_slots[i].active || s_pwm_slots[i].pin != pin) continue;
        if (ledc_set_duty(LEDC_LOW_SPEED_MODE, s_pwm_slots[i].channel, duty) != ESP_OK) return false;
        return ledc_update_duty(LEDC_LOW_SPEED_MODE, s_pwm_slots[i].channel) == ESP_OK;
    }
    return false;
}

bool plugin_api_pwm_detach(int pin) {
    if (!has_permission(PLUGIN_PERMISSION_PWM)) return false;
    for (int i = 0; i < PLUGIN_PWM_MAX_CHANNELS; i++) {
        if (!s_pwm_slots[i].active || s_pwm_slots[i].pin != pin) continue;
        ledc_stop(LEDC_LOW_SPEED_MODE, s_pwm_slots[i].channel, 0);
        memset(&s_pwm_slots[i], 0, sizeof(s_pwm_slots[i]));
        return true;
    }
    return false;
}

uint64_t plugin_api_system_uptime_us(void) {
    return has_permission(PLUGIN_PERMISSION_TIME) ? (uint64_t)esp_timer_get_time() : 0;
}

void plugin_api_delay_us(uint32_t us) {
    if (!has_permission(PLUGIN_PERMISSION_TIME)) return;
    esp_rom_delay_us(us);
}

uint32_t plugin_api_random_u32(void) {
    return has_permission(PLUGIN_PERMISSION_RANDOM) ? esp_random() : 0;
}

bool plugin_api_random_bytes(void *buffer, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_RANDOM) || (!buffer && len > 0)) return false;
    esp_fill_random(buffer, len);
    return true;
}

bool plugin_api_storage_stat(const char *path, ghostesp_storage_stat_t *out) {
    return ghostesp_path_allowed(path) && stat_path(path, out);
}

int64_t plugin_api_storage_size(const char *path) {
    ghostesp_storage_stat_t st;
    return plugin_api_storage_stat(path, &st) && !st.is_directory ? (int64_t)st.size : -1;
}

bool plugin_api_storage_rename(const char *from, const char *to) {
    return ghostesp_path_allowed(from) && ghostesp_path_allowed(to) && rename(from, to) == 0;
}

bool plugin_api_storage_mkdir_recursive(const char *path) {
    return ghostesp_path_allowed(path) && mkdir_recursive_path(path);
}

bool plugin_api_app_storage_stat(const char *path, ghostesp_storage_stat_t *out) {
    char full_path[PLUGIN_APP_PATH_MAX];
    return plugin_api_internal_build_app_path(path, full_path, sizeof(full_path)) && stat_path(full_path, out);
}

int64_t plugin_api_app_storage_size(const char *path) {
    ghostesp_storage_stat_t st;
    return plugin_api_app_storage_stat(path, &st) && !st.is_directory ? (int64_t)st.size : -1;
}

bool plugin_api_app_storage_rename(const char *from, const char *to) {
    char full_from[PLUGIN_APP_PATH_MAX];
    char full_to[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_internal_build_app_path(from, full_from, sizeof(full_from))) return false;
    if (!plugin_api_internal_build_app_path(to, full_to, sizeof(full_to))) return false;
    return rename(full_from, full_to) == 0;
}

bool plugin_api_app_storage_mkdir_recursive(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    return plugin_api_internal_build_app_path(path, full_path, sizeof(full_path)) && mkdir_recursive_path(full_path);
}

int plugin_api_battery_percent(void) {
    return has_permission(PLUGIN_PERMISSION_POWER) ? fuel_gauge_manager_get_percentage() : -1;
}

int plugin_api_battery_voltage_mv(void) {
    return has_permission(PLUGIN_PERMISSION_POWER) ? (int)fuel_gauge_manager_get_voltage_mv() : -1;
}

bool plugin_api_battery_is_charging(void) {
    return has_permission(PLUGIN_PERMISSION_POWER) && fuel_gauge_manager_is_charging();
}

uint8_t plugin_api_display_get_brightness(void) {
    return has_permission(PLUGIN_PERMISSION_DISPLAY) ? s_display_brightness : 0;
}

bool plugin_api_display_set_brightness(uint8_t percent) {
    if (!has_permission(PLUGIN_PERMISSION_DISPLAY)) return false;
    if (percent > 100) percent = 100;
    set_backlight_brightness(percent);
    s_display_brightness = percent;
    return true;
}

uint32_t plugin_api_input_buttons_state(void) {
    if (!has_permission(PLUGIN_PERMISSION_INPUT)) return 0;
    uint32_t state = 0;
    for (int i = 0; i < 5; i++) {
        if (joysticks[i].pin >= 0 && joystick_get_button_state(&joysticks[i])) state |= (1u << i);
    }
    return state;
}

static bool wifi_has_ip(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

bool plugin_api_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) || !ssid || !ssid[0]) return false;
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    esp_wifi_disconnect();
    if (esp_wifi_connect() != ESP_OK) return false;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (wifi_has_ip()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return wifi_has_ip();
}

bool plugin_api_wifi_disconnect(void) {
    return has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) && esp_wifi_disconnect() == ESP_OK;
}

bool plugin_api_wifi_is_connected(void) {
    return has_permission(PLUGIN_PERMISSION_WIFI) && wifi_has_ip();
}

int plugin_api_wifi_rssi(void) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI)) return 0;
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

bool plugin_api_wifi_ip(char *out, size_t out_len) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI) || !out || out_len == 0) return false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    return ip4addr_ntoa_r((const ip4_addr_t *)&ip.ip, out, out_len) != NULL;
}

static int http_request(const char *url, const void *body, size_t body_len, void *buffer, size_t buffer_len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || !url || !buffer || buffer_len == 0) return -1;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = (int)timeout_ms,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;
    if (body) esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_err_t err = esp_http_client_open(client, body ? (int)body_len : 0);
    if (err == ESP_OK && body && body_len > 0) {
        int written = esp_http_client_write(client, body, body_len);
        if (written < 0 || (size_t)written != body_len) err = ESP_FAIL;
    }
    int total = -1;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        total = 0;
        while ((size_t)total < buffer_len) {
            int n = esp_http_client_read(client, (char *)buffer + total, buffer_len - (size_t)total);
            if (n <= 0) break;
            total += n;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total;
}

int plugin_api_http_get(const char *url, void *buffer, size_t buffer_len, uint32_t timeout_ms) {
    return http_request(url, NULL, 0, buffer, buffer_len, timeout_ms);
}

int plugin_api_http_post(const char *url, const void *body, size_t body_len, void *buffer, size_t buffer_len, uint32_t timeout_ms) {
    if (!body && body_len > 0) return -1;
    return http_request(url, body, body_len, buffer, buffer_len, timeout_ms);
}

typedef struct {
    ghostesp_task_fn_t fn;
    void *user;
} plugin_task_ctx_t;

static void plugin_task_bridge(void *arg) {
    plugin_task_ctx_t *ctx = (plugin_task_ctx_t *)arg;
    ghostesp_task_fn_t fn = ctx ? ctx->fn : NULL;
    void *user = ctx ? ctx->user : NULL;
    free(ctx);
    if (fn) fn(user);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < PLUGIN_TASK_MAX; i++) if (s_tasks[i] == self) s_tasks[i] = NULL;
    vTaskDelete(NULL);
}

ghostesp_task_t plugin_api_task_create(const char *name, ghostesp_task_fn_t fn, void *user, uint32_t stack_size, int priority) {
    if (!has_permission(PLUGIN_PERMISSION_TASKS) || !fn) return NULL;
    int slot = -1;
    for (int i = 0; i < PLUGIN_TASK_MAX; i++) if (!s_tasks[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    plugin_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->fn = fn;
    ctx->user = user;
    TaskHandle_t handle = NULL;
    if (stack_size < 2048) stack_size = 2048;
    if (xTaskCreate(plugin_task_bridge, name && name[0] ? name : "app_task", stack_size, ctx, priority, &handle) != pdPASS) {
        free(ctx);
        return NULL;
    }
    s_tasks[slot] = handle;
    return (ghostesp_task_t)handle;
}

bool plugin_api_task_delete(ghostesp_task_t task) {
    if (!has_permission(PLUGIN_PERMISSION_TASKS) || !task) return false;
    TaskHandle_t handle = (TaskHandle_t)task;
    for (int i = 0; i < PLUGIN_TASK_MAX; i++) {
        if (s_tasks[i] != handle) continue;
        s_tasks[i] = NULL;
        vTaskDelete(handle);
        return true;
    }
    return false;
}

void plugin_api_task_yield(void) {
    if (has_permission(PLUGIN_PERMISSION_TASKS)) taskYIELD();
}

int plugin_api_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || !host || !host[0] || port == 0) return -1;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;
    int sock = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000), .tv_usec = (long)((timeout_ms % 1000) * 1000) };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock >= 0) track_socket(sock);
    return sock;
}

int plugin_api_socket_send(int sock, const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || sock < 0 || (!data && len > 0)) return -1;
    return send(sock, data, len, 0);
}

int plugin_api_socket_recv(int sock, void *buffer, size_t len, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || sock < 0 || !buffer || len == 0) return -1;
    struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000), .tv_usec = (long)((timeout_ms % 1000) * 1000) };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(sock, buffer, len, 0);
}

bool plugin_api_socket_close(int sock) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || sock < 0) return false;
    untrack_socket(sock);
    return close(sock) == 0;
}

int plugin_api_udp_open(uint16_t local_port) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK)) return -1;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return -1;
    if (local_port != 0) {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(local_port), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(sock);
            return -1;
        }
    }
    track_socket(sock);
    return sock;
}

int plugin_api_udp_send_to(int sock, const char *host, uint16_t port, const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || sock < 0 || !host || port == 0 || (!data && len > 0)) return -1;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
    return sendto(sock, data, len, 0, (struct sockaddr *)&addr, sizeof(addr));
}

int plugin_api_udp_recv_from(int sock, void *buffer, size_t len, char *host_out, size_t host_out_len, uint16_t *port_out, uint32_t timeout_ms) {
    if (!has_permission(PLUGIN_PERMISSION_NETWORK) || sock < 0 || !buffer || len == 0) return -1;
    struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000), .tv_usec = (long)((timeout_ms % 1000) * 1000) };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, buffer, len, 0, (struct sockaddr *)&from, &from_len);
    if (n >= 0) {
        if (host_out && host_out_len > 0) inet_ntop(AF_INET, &from.sin_addr, host_out, host_out_len);
        if (port_out) *port_out = ntohs(from.sin_port);
    }
    return n;
}

int64_t plugin_api_time_unix(void) {
    if (!has_permission(PLUGIN_PERMISSION_TIME)) return 0;
    return (int64_t)time(NULL);
}

bool plugin_api_time_set_unix(int64_t unix_time) {
    if (!has_permission(PLUGIN_PERMISSION_TIME)) return false;
    struct timeval tv = { .tv_sec = (time_t)unix_time, .tv_usec = 0 };
    return settimeofday(&tv, NULL) == 0;
}

void plugin_api_system_reboot(void) {
    if (has_permission(PLUGIN_PERMISSION_SYSTEM)) esp_restart();
}

bool plugin_api_wifi_set_channel(uint8_t channel) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) || channel == 0 || channel > 14) return false;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

uint8_t plugin_api_wifi_get_channel(void) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI)) return 0;
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    return esp_wifi_get_channel(&primary, &second) == ESP_OK ? primary : 0;
}

static void plugin_wifi_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    if (!buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (s_wifi_pcap_active) pcap_write_packet_to_buffer(pkt->payload, pkt->rx_ctrl.sig_len, PCAP_CAPTURE_WIFI);
    if (s_wifi_packet_cb) s_wifi_packet_cb(pkt->payload, pkt->rx_ctrl.sig_len, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel, s_wifi_packet_user);
}

bool plugin_api_wifi_monitor_start(ghostesp_wifi_packet_cb_t cb, void *user) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI) || !cb) return false;
    s_wifi_packet_cb = cb;
    s_wifi_packet_user = user;
    esp_wifi_set_promiscuous_rx_cb(plugin_wifi_promisc_cb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        if (!s_wifi_pcap_active) esp_wifi_set_promiscuous_rx_cb(NULL);
        s_wifi_packet_cb = NULL;
        s_wifi_packet_user = NULL;
        return false;
    }
    return true;
}

bool plugin_api_wifi_monitor_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    if (s_wifi_pcap_active) {
        pcap_flush_buffer_to_file();
        pcap_file_close();
        s_wifi_pcap_active = false;
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    s_wifi_packet_cb = NULL;
    s_wifi_packet_user = NULL;
    return true;
}

bool plugin_api_wifi_raw_tx(const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) || !data || len == 0) return false;
    return esp_wifi_80211_tx(WIFI_IF_STA, data, len, false) == ESP_OK;
}

bool plugin_api_nfc_get_last_uid(uint8_t *uid, size_t *uid_len) {
    if (!has_permission(PLUGIN_PERMISSION_NFC) || !uid || !uid_len) return false;
    uint8_t raw_uid[10];
    uint8_t raw_len = 0;
    if (!nfc_api_get_last_uid(raw_uid, &raw_len)) return false;
    *uid_len = raw_len > 10 ? 10 : raw_len;
    memcpy(uid, raw_uid, *uid_len);
    return true;
}

bool plugin_api_nfc_write_file(const char *app_relative_path) {
    (void)app_relative_path;
    return false;
}

bool plugin_api_ir_send_raw(uint32_t carrier_hz, const uint16_t *durations, size_t count) {
    if (!has_permission(PLUGIN_PERMISSION_IR) || !durations || count == 0) return false;
#ifdef CONFIG_HAS_INFRARED
    uint32_t *timings = calloc(count, sizeof(uint32_t));
    if (!timings) return false;
    for (size_t i = 0; i < count; i++) timings[i] = durations[i];
    infrared_signal_t signal = {0};
    snprintf(signal.name, sizeof(signal.name), "app_raw");
    signal.is_raw = true;
    signal.payload.raw.timings = timings;
    signal.payload.raw.timings_size = count;
    signal.payload.raw.frequency = carrier_hz ? carrier_hz : 38000;
    signal.payload.raw.duty_cycle = 0.33f;
    bool ok = infrared_manager_transmit(&signal);
    free(timings);
    return ok;
#else
    (void)carrier_hz;
    return false;
#endif
}

bool plugin_api_ir_receive_start(void) {
    if (!has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    return infrared_manager_rx_init();
#else
    return false;
#endif
}

bool plugin_api_ir_receive_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    infrared_manager_rx_deinit();
    return true;
#else
    return false;
#endif
}

int plugin_api_ir_receive_read(uint16_t *durations, size_t max_count) {
    if (!has_permission(PLUGIN_PERMISSION_IR) || !durations || max_count == 0) return -1;
#ifdef CONFIG_HAS_INFRARED
    infrared_signal_t signal = {0};
    if (!infrared_manager_rx_receive(&signal, 0) || !signal.is_raw) return 0;
    size_t n = signal.payload.raw.timings_size < max_count ? signal.payload.raw.timings_size : max_count;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = signal.payload.raw.timings[i];
        durations[i] = v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
    }
    infrared_manager_free_signal(&signal);
    return (int)n;
#else
    return -1;
#endif
}

bool plugin_api_subghz_transmit_raw(uint32_t frequency_hz, const uint16_t *durations, size_t count) {
    if (!has_permission(PLUGIN_PERMISSION_SUBGHZ) || !durations || count == 0) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    int32_t *signed_durations = calloc(count, sizeof(int32_t));
    if (!signed_durations) return false;
    for (size_t i = 0; i < count; i++) signed_durations[i] = (i % 2 == 0) ? (int32_t)durations[i] : -(int32_t)durations[i];
    bool ok = subghz_remote_manager_transmit_raw(signed_durations, count, frequency_hz, SUBGHZ_PRESET_OOK650_ASYNC);
    free(signed_durations);
    return ok;
#else
    return false;
#endif
}

bool plugin_api_ble_adv_start(const uint8_t *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_BLE) || !data || len == 0) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    return ble_start_custom_adv(data, len);
#else
    return false;
#endif
}

bool plugin_api_ble_adv_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_BLE)) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    return ble_stop_custom_adv();
#else
    return false;
#endif
}

bool plugin_api_ble_gatt_connect(const uint8_t mac[6]) {
    if (!has_permission(PLUGIN_PERMISSION_BLE) || !mac) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_ble_gatt_sem != NULL) {
        ble_gap_terminate(s_ble_gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_ble_gatt_conn_handle = 0xffff;
    }
    if (s_ble_gatt_sem == NULL) {
        s_ble_gatt_sem = xSemaphoreCreateBinary();
        if (!s_ble_gatt_sem) return false;
    }
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return false;
    ble_addr_t addr = {0};
    addr.type = BLE_ADDR_PUBLIC;
    memcpy(addr.val, mac, 6);
    s_ble_gatt_error = 0;
    int rc = ble_gap_connect(own_addr_type, &addr, 30000, NULL, plugin_ble_gatt_gap_cb, NULL);
    if (rc != 0) return false;
    xSemaphoreTake(s_ble_gatt_sem, pdMS_TO_TICKS(30000));
    if (s_ble_gatt_conn_handle == 0xffff || s_ble_gatt_error != 0) return false;
    ble_gap_conn_active();
    return true;
#else
    return false;
#endif
}

bool plugin_api_ble_gatt_disconnect(void) {
    if (!has_permission(PLUGIN_PERMISSION_BLE)) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_ble_gatt_conn_handle != 0xffff) {
        ble_gap_terminate(s_ble_gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_ble_gatt_conn_handle = 0xffff;
    }
    return true;
#else
    return false;
#endif
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
static void plugin_ble_gatt_ensure_sem(void) {
    if (!s_ble_gatt_sem) s_ble_gatt_sem = xSemaphoreCreateBinary();
}

static bool plugin_ble_gatt_wait(uint32_t timeout_ms) {
    return xSemaphoreTake(s_ble_gatt_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void plugin_ble_gatt_signal(void) {
    if (s_ble_gatt_sem) xSemaphoreGive(s_ble_gatt_sem);
}

static int plugin_ble_gatt_gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_ble_gatt_error = event->connect.status;
        s_ble_gatt_conn_handle = (event->connect.status == 0) ? event->connect.conn_handle : 0xffff;
        plugin_ble_gatt_signal();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_gatt_conn_handle = 0xffff;
        if (s_ble_gatt_read_buf) *((int*)s_ble_gatt_read_buf) = -1;
        plugin_ble_gatt_signal();
        break;
    default:
        break;
    }
    return 0;
}

static int plugin_ble_gattc_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                     struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error->status != 0) {
        s_ble_gatt_error = error->status;
        s_ble_gatt_read_len = 0;
        plugin_ble_gatt_signal();
        return 0;
    }
    if (!attr || !attr->om) {
        s_ble_gatt_read_len = 0;
        plugin_ble_gatt_signal();
        return 0;
    }
    uint16_t om_len = OS_MBUF_PKTLEN(attr->om);
    size_t copy_len = om_len < s_ble_gatt_read_len ? om_len : s_ble_gatt_read_len;
    if (s_ble_gatt_read_buf && copy_len > 0) ble_hs_mbuf_to_flat(attr->om, s_ble_gatt_read_buf, copy_len, &copy_len);
    s_ble_gatt_read_len = copy_len;
    s_ble_gatt_error = 0;
    plugin_ble_gatt_signal();
    return 0;
}

static int plugin_ble_gattc_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle;
    if (error->status == BLE_HS_EDONE) {
        plugin_ble_gatt_signal();
        return 0;
    }
    if (error->status != 0 || !chr) return 0;
    uint16_t target_uuid = (uint16_t)(uintptr_t)arg;
    if (chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == target_uuid) {
        s_ble_gatt_char_handle = chr->val_handle;
        s_ble_gatt_found_char = true;
    }
    return 0;
}

static int plugin_ble_gattc_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         const struct ble_gatt_svc *service, void *arg) {
    (void)conn_handle;
    if (error->status == BLE_HS_EDONE) {
        plugin_ble_gatt_signal();
        return 0;
    }
    if (error->status != 0 || !service) return 0;
    uint16_t target_svc_uuid = (uint16_t)(uintptr_t)arg;
    if (service->uuid.u.type != BLE_UUID_TYPE_16) return 0;
    if (service->uuid.u16.value != target_svc_uuid) return 0;
    if (ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                 plugin_ble_gattc_disc_chr_cb, arg) != 0) return 0;
    if (!plugin_ble_gatt_wait(10000)) return 0;
    return 0;
}
#endif

int plugin_api_ble_gatt_read(uint16_t service_uuid, uint16_t char_uuid, void *buffer, size_t buffer_len) {
    if (!has_permission(PLUGIN_PERMISSION_BLE) || !buffer || buffer_len == 0) return -1;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_ble_gatt_conn_handle == 0xffff) return -1;
    plugin_ble_gatt_ensure_sem();
    s_ble_gatt_read_buf = (uint8_t *)buffer;
    s_ble_gatt_read_len = buffer_len;
    s_ble_gatt_error = 0;
    s_ble_gatt_found_char = false;
    s_ble_gatt_char_handle = 0;
    ble_gattc_disc_all_svcs(s_ble_gatt_conn_handle, plugin_ble_gattc_disc_svc_cb, (void*)(uintptr_t)char_uuid);
    if (!plugin_ble_gatt_wait(15000)) return -1;
    if (!s_ble_gatt_found_char || s_ble_gatt_char_handle == 0) return -1;
    ble_gattc_read(s_ble_gatt_conn_handle, s_ble_gatt_char_handle, plugin_ble_gattc_read_cb, NULL);
    if (!plugin_ble_gatt_wait(10000)) return -1;
    if (s_ble_gatt_error != 0) return -1;
    return (int)s_ble_gatt_read_len;
#else
    (void)service_uuid;
    return -1;
#endif
}

bool plugin_api_ble_gatt_write(uint16_t service_uuid, uint16_t char_uuid, const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_BLE) || (!data && len > 0)) return false;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_ble_gatt_conn_handle == 0xffff) return false;
    plugin_ble_gatt_ensure_sem();
    s_ble_gatt_read_buf = NULL;
    s_ble_gatt_read_len = 0;
    s_ble_gatt_error = 0;
    s_ble_gatt_found_char = false;
    s_ble_gatt_char_handle = 0;
    ble_gattc_disc_all_svcs(s_ble_gatt_conn_handle, plugin_ble_gattc_disc_svc_cb, (void*)(uintptr_t)char_uuid);
    if (!plugin_ble_gatt_wait(15000)) return false;
    if (!s_ble_gatt_found_char || s_ble_gatt_char_handle == 0) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    int rc = ble_gattc_write_no_rsp(s_ble_gatt_conn_handle, s_ble_gatt_char_handle, om);
    if (rc != 0) { os_mbuf_free_chain(om); return false; }
    return true;
#else
    (void)service_uuid;
    return false;
#endif
}

bool plugin_api_ble_gatt_server_start(const char *name) {
    (void)name;
    return false;
}

bool plugin_api_ble_gatt_server_stop(void) {
    return false;
}

bool plugin_api_nrf24_start(bool stream_to_peer) {
    if (!has_permission(PLUGIN_PERMISSION_NRF24)) return false;
#ifdef CONFIG_HAS_NRF24
    return nrf24_remote_manager_start(stream_to_peer);
#else
    (void)stream_to_peer;
    return false;
#endif
}

void plugin_api_nrf24_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_NRF24)) return;
#ifdef CONFIG_HAS_NRF24
    nrf24_remote_manager_stop();
#endif
}

bool plugin_api_nrf24_is_running(void) {
    if (!has_permission(PLUGIN_PERMISSION_NRF24)) return false;
#ifdef CONFIG_HAS_NRF24
    return nrf24_remote_manager_is_running();
#else
    return false;
#endif
}

bool plugin_api_nrf24_is_paused(void) {
    if (!has_permission(PLUGIN_PERMISSION_NRF24)) return false;
#ifdef CONFIG_HAS_NRF24
    return nrf24_remote_manager_is_paused();
#else
    return false;
#endif
}

void plugin_api_nrf24_set_paused(bool paused) {
    if (!has_permission(PLUGIN_PERMISSION_NRF24)) return;
#ifdef CONFIG_HAS_NRF24
    nrf24_remote_manager_set_paused(paused);
#else
    (void)paused;
#endif
}

bool plugin_api_wifi_deauth(const uint8_t bssid[6], const uint8_t sta[6], uint8_t reason) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) || !bssid) return false;
    uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t *dst = sta ? sta : broadcast;
    uint8_t frame[26] = {
        0xc0, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0x00, 0x00,
        0x01, 0x00,
    };
    memcpy(&frame[4], dst, 6);
    memcpy(&frame[10], bssid, 6);
    memcpy(&frame[16], bssid, 6);
    frame[24] = reason ? reason : 1;
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK;
}

bool plugin_api_wifi_send_beacon(const char *ssid, const uint8_t bssid[6], uint8_t channel) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI_CONTROL) || !ssid || !bssid) return false;
    size_t ssid_len = strnlen(ssid, 32);
    uint8_t frame[128] = {0};
    size_t off = 0;
    const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    frame[off++] = 0x80; frame[off++] = 0x00; frame[off++] = 0x00; frame[off++] = 0x00;
    memcpy(&frame[off], broadcast, 6); off += 6;
    memcpy(&frame[off], bssid, 6); off += 6;
    memcpy(&frame[off], bssid, 6); off += 6;
    frame[off++] = 0x00; frame[off++] = 0x00;
    off += 8;
    frame[off++] = 0x64; frame[off++] = 0x00;
    frame[off++] = 0x31; frame[off++] = 0x04;
    frame[off++] = 0x00; frame[off++] = (uint8_t)ssid_len;
    memcpy(&frame[off], ssid, ssid_len); off += ssid_len;
    const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96};
    frame[off++] = 0x01; frame[off++] = sizeof(rates);
    memcpy(&frame[off], rates, sizeof(rates)); off += sizeof(rates);
    frame[off++] = 0x03; frame[off++] = 0x01; frame[off++] = channel ? channel : 1;
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, off, false) == ESP_OK;
}

bool plugin_api_wifi_pcap_start(const char *app_relative_path) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI) || !has_permission(PLUGIN_PERMISSION_STORAGE) || !app_relative_path) return false;
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_internal_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    char dir[PLUGIN_APP_PATH_MAX];
    char base[64];
    const char *slash = strrchr(full_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - full_path);
        if (dir_len == 0 || dir_len >= sizeof(dir)) return false;
        memcpy(dir, full_path, dir_len);
        dir[dir_len] = '\0';
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), "%s", full_path);
        snprintf(base, sizeof(base), "capture");
    }
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    if (!base[0]) snprintf(base, sizeof(base), "capture");
    if (!mkdir_recursive_path(dir)) return false;
    if (pcap_file_open_in_dir(base, dir, PCAP_CAPTURE_WIFI) != ESP_OK) return false;
    s_wifi_pcap_active = true;
    esp_wifi_set_promiscuous_rx_cb(plugin_wifi_promisc_cb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        pcap_file_close();
        s_wifi_pcap_active = false;
        if (!s_wifi_packet_cb) esp_wifi_set_promiscuous_rx_cb(NULL);
        return false;
    }
    return true;
}

bool plugin_api_wifi_pcap_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    if (!s_wifi_pcap_active) return true;
    pcap_flush_buffer_to_file();
    pcap_file_close();
    s_wifi_pcap_active = false;
    if (!s_wifi_packet_cb) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
    }
    return true;
}

bool plugin_api_ethernet_is_connected(void) {
#ifdef CONFIG_WITH_ETHERNET
    return has_permission(PLUGIN_PERMISSION_ETHERNET) && ethernet_manager_is_connected();
#else
    return false;
#endif
}

bool plugin_api_ethernet_ip(char *out, size_t out_len) {
    if (!has_permission(PLUGIN_PERMISSION_ETHERNET) || !out || out_len == 0) return false;
#ifdef CONFIG_WITH_ETHERNET
    esp_netif_ip_info_t ip;
    if (ethernet_manager_get_ip_info(&ip) != ESP_OK || ip.ip.addr == 0) return false;
    return ip4addr_ntoa_r((const ip4_addr_t *)&ip.ip, out, out_len) != NULL;
#else
    if (out && out_len) out[0] = '\0';
    return false;
#endif
}

int plugin_api_camera_capture_jpeg(void *buffer, size_t buffer_len) {
    if (!has_permission(PLUGIN_PERMISSION_CAMERA) || !buffer || buffer_len == 0) return -1;
#if CONFIG_HAS_CAMERA
    CameraStreamState state = camera_stream_get_state();
    bool started = false;
    if (!state.is_running) {
        if (camera_stream_start() != ESP_OK) return -1;
        started = true;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        if (started) camera_stream_stop();
        return -1;
    }
    size_t copy_len = fb->len < buffer_len ? fb->len : buffer_len;
    memcpy(buffer, fb->buf, copy_len);
    esp_camera_fb_return(fb);
    if (started) camera_stream_stop();
    return (int)copy_len;
#else
    return -1;
#endif
}

bool plugin_api_camera_capture_jpeg_file(const char *app_relative_path) {
    if (!has_permission(PLUGIN_PERMISSION_CAMERA) || !has_permission(PLUGIN_PERMISSION_STORAGE)) return false;
#if CONFIG_HAS_CAMERA
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_internal_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    CameraStreamState state = camera_stream_get_state();
    bool started = false;
    if (!state.is_running) {
        if (camera_stream_start() != ESP_OK) return false;
        started = true;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        if (started) camera_stream_stop();
        return false;
    }
    FILE *f = fopen(full_path, "wb");
    bool ok = f && fwrite(fb->buf, 1, fb->len, f) == fb->len;
    if (f) fclose(f);
    esp_camera_fb_return(fb);
    if (started) camera_stream_stop();
    return ok;
#else
    (void)app_relative_path;
    return false;
#endif
}

bool plugin_api_usb_hid_keyboard_send(const char *text) {
    if (!has_permission(PLUGIN_PERMISSION_USB) || !text || !text[0]) return false;
#ifdef CONFIG_HAS_BADUSB
    for (const char *p = text; *p; p++) {
        uint8_t keycode = 0, modifier = 0;
        if (!hid_ascii_to_keycode(*p, &keycode, &modifier)) continue;
        int timeout = 100;
        while (!tud_hid_n_ready(0) && timeout-- > 0) vTaskDelay(pdMS_TO_TICKS(1));
        if (!tud_hid_n_ready(0)) return false;
        uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
        tud_hid_n_keyboard_report(0, 0, modifier, keycodes);
        vTaskDelay(pdMS_TO_TICKS(5));
        tud_hid_n_keyboard_report(0, 0, 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
#else
    return false;
#endif
}

bool plugin_api_usb_hid_mouse_move(int dx, int dy, uint8_t buttons) {
    if (!has_permission(PLUGIN_PERMISSION_USB)) return false;
#ifdef CONFIG_HAS_BADUSB
    return badusb_hid_mouse_send((int8_t)dx, (int8_t)dy, buttons);
#else
    (void)dx; (void)dy; (void)buttons;
    return false;
#endif
}

bool plugin_api_audio_mic_is_available(void) {
    return has_permission(PLUGIN_PERMISSION_AUDIO) && mic_is_initialized();
}

int plugin_api_audio_mic_read(int32_t *samples, size_t max_samples, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!has_permission(PLUGIN_PERMISSION_AUDIO) || !samples || max_samples == 0 || !mic_is_initialized()) return -1;
    int32_t *tmp = malloc(max_samples * 2 * sizeof(int32_t));
    if (!tmp) return -1;
    size_t bytes = 0;
    esp_err_t err = mic_read_samples(tmp, max_samples, &bytes);
    int count = err == ESP_OK ? (int)(bytes / sizeof(int32_t)) : -1;
    if (count > 0) memcpy(samples, tmp, (size_t)count * sizeof(int32_t));
    free(tmp);
    return count;
}

float plugin_api_audio_mic_rms(const int32_t *samples, size_t count) {
    if (!has_permission(PLUGIN_PERMISSION_AUDIO) || !samples || count == 0) return 0.0f;
    return mic_calculate_rms(samples, count);
}

bool plugin_api_zigbee_capture_start(uint8_t channel) {
    if (!has_permission(PLUGIN_PERMISSION_ZIGBEE)) return false;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    return zigbee_manager_start_capture(channel) == ESP_OK;
#else
    (void)channel;
    return false;
#endif
}

bool plugin_api_zigbee_capture_stop(void) {
    if (!has_permission(PLUGIN_PERMISSION_ZIGBEE)) return false;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    zigbee_manager_stop_capture();
    return true;
#else
    return false;
#endif
}

bool plugin_api_zigbee_is_capturing(void) {
    if (!has_permission(PLUGIN_PERMISSION_ZIGBEE)) return false;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    return zigbee_manager_is_capturing();
#else
    return false;
#endif
}

int plugin_api_zigbee_device_count(void) {
    if (!has_permission(PLUGIN_PERMISSION_ZIGBEE)) return 0;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    return zigbee_manager_get_device_count();
#else
    return 0;
#endif
}

bool plugin_api_settings_get_u8(const char *key, uint8_t *out) {
    if (!has_permission(PLUGIN_PERMISSION_SETTINGS) || !key || !out) return false;
    if (strcmp(key, "theme") == 0) *out = settings_get_menu_theme(&G_Settings);
    else if (strcmp(key, "max_brightness") == 0) *out = settings_get_max_screen_brightness(&G_Settings);
    else if (strcmp(key, "nav_buttons") == 0) *out = settings_get_nav_buttons_enabled(&G_Settings) ? 1 : 0;
    else if (strcmp(key, "neopixel_brightness") == 0) *out = settings_get_neopixel_max_brightness(&G_Settings);
    else if (strcmp(key, "power_save") == 0) *out = settings_get_power_save_enabled(&G_Settings) ? 1 : 0;
    else return false;
    return true;
}

bool plugin_api_settings_set_u8(const char *key, uint8_t value) {
    if (!has_permission(PLUGIN_PERMISSION_SETTINGS) || !key) return false;
    if (strcmp(key, "theme") == 0) settings_set_menu_theme(&G_Settings, value);
    else if (strcmp(key, "max_brightness") == 0) settings_set_max_screen_brightness(&G_Settings, value);
    else if (strcmp(key, "nav_buttons") == 0) settings_set_nav_buttons_enabled(&G_Settings, value != 0);
    else if (strcmp(key, "neopixel_brightness") == 0) settings_set_neopixel_max_brightness(&G_Settings, value);
    else if (strcmp(key, "power_save") == 0) settings_set_power_save_enabled(&G_Settings, value != 0);
    else return false;
    return true;
}

bool plugin_api_settings_get_string(const char *key, char *out, size_t out_len) {
    if (!has_permission(PLUGIN_PERMISSION_SETTINGS) || !key || !out || out_len == 0) return false;
    if (strcmp(key, "device_name") != 0) return false;
    const char *name = settings_get_ap_ssid(&G_Settings);
    snprintf(out, out_len, "%s", name ? name : "");
    return true;
}

bool plugin_api_settings_set_string(const char *key, const char *value) {
    if (!has_permission(PLUGIN_PERMISSION_SETTINGS) || !key || !value) return false;
    if (strcmp(key, "device_name") != 0) return false;
    settings_set_ap_ssid(&G_Settings, value);
    return true;
}

bool plugin_api_settings_save(void) {
    if (!has_permission(PLUGIN_PERMISSION_SETTINGS)) return false;
    settings_save(&G_Settings);
    return true;
}

static bool nvs_key_for_app(const char *key, char out[16]) {
    if (!has_permission(PLUGIN_PERMISSION_STORAGE) || !key || !key[0]) return false;
    const char *app = plugin_api_internal_app_id();
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)app; p && *p; ++p) { hash ^= *p; hash *= 16777619u; }
    hash ^= ':'; hash *= 16777619u;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) { hash ^= *p; hash *= 16777619u; }
    snprintf(out, 16, "%08lx", (unsigned long)hash);
    return true;
}

bool plugin_api_nvs_get_u32(const char *key, uint32_t *out) {
    if (!out) return false;
    char nkey[16];
    if (!nvs_key_for_app(key, nkey)) return false;
    nvs_handle_t h;
    if (nvs_open("plugin", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_u32(h, nkey, out);
    nvs_close(h);
    return err == ESP_OK;
}

bool plugin_api_nvs_set_u32(const char *key, uint32_t value) {
    char nkey[16];
    if (!nvs_key_for_app(key, nkey)) return false;
    nvs_handle_t h;
    if (nvs_open("plugin", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u32(h, nkey, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

int plugin_api_nvs_get_blob(const char *key, void *buffer, size_t buffer_len) {
    char nkey[16];
    if (!buffer || !nvs_key_for_app(key, nkey)) return -1;
    nvs_handle_t h;
    if (nvs_open("plugin", NVS_READONLY, &h) != ESP_OK) return -1;
    size_t len = buffer_len;
    esp_err_t err = nvs_get_blob(h, nkey, buffer, &len);
    nvs_close(h);
    return err == ESP_OK ? (int)len : -1;
}

bool plugin_api_nvs_set_blob(const char *key, const void *data, size_t len) {
    char nkey[16];
    if ((!data && len > 0) || !nvs_key_for_app(key, nkey)) return false;
    nvs_handle_t h;
    if (nvs_open("plugin", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, nkey, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool plugin_api_nvs_delete(const char *key) {
    char nkey[16];
    if (!nvs_key_for_app(key, nkey)) return false;
    nvs_handle_t h;
    if (nvs_open("plugin", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, nkey);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

ghostesp_event_sub_t plugin_api_event_subscribe(const char *topic, ghostesp_event_cb_t cb, void *user) {
    if (!has_permission(PLUGIN_PERMISSION_TASKS) || !topic || !cb) return NULL;
    if (!ensure_event_slots()) return NULL;
    for (int i = 0; i < PLUGIN_EVENT_MAX; i++) {
        if (s_events[i].active) continue;
        s_events[i].active = true;
        snprintf(s_events[i].topic, sizeof(s_events[i].topic), "%s", topic);
        s_events[i].cb = cb;
        s_events[i].user = user;
        return &s_events[i];
    }
    return NULL;
}

bool plugin_api_event_unsubscribe(ghostesp_event_sub_t sub) {
    if (!has_permission(PLUGIN_PERMISSION_TASKS) || !sub || !s_events) return false;
    plugin_event_slot_t *slot = (plugin_event_slot_t *)sub;
    for (int i = 0; i < PLUGIN_EVENT_MAX; i++) {
        if (&s_events[i] != slot) continue;
        memset(slot, 0, sizeof(*slot));
        return true;
    }
    return false;
}

bool plugin_api_event_publish(const char *topic, const void *data, size_t len) {
    if (!has_permission(PLUGIN_PERMISSION_TASKS) || !topic || (!data && len > 0)) return false;
    if (!s_events) return true;
    for (int i = 0; i < PLUGIN_EVENT_MAX; i++) {
        if (!s_events[i].active || strcmp(s_events[i].topic, topic) != 0) continue;
        if (s_events[i].cb) s_events[i].cb(topic, data, len, s_events[i].user);
    }
    return true;
}

static bool parser_file_summary(const char *app_relative_path, const char *kind, char *out, size_t out_len) {
    if (!has_permission(PLUGIN_PERMISSION_STORAGE) || !out || out_len == 0) return false;
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_internal_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    struct stat st;
    if (stat(full_path, &st) != 0) return false;
    snprintf(out, out_len, "%s file, %lld bytes", kind, (long long)st.st_size);
    return true;
}

bool plugin_api_parser_nfc_summary(const char *app_relative_path, char *out, size_t out_len) {
    return parser_file_summary(app_relative_path, "NFC", out, out_len);
}

bool plugin_api_parser_ir_summary(const char *app_relative_path, char *out, size_t out_len) {
    return parser_file_summary(app_relative_path, "IR", out, out_len);
}

bool plugin_api_parser_subghz_summary(const char *app_relative_path, char *out, size_t out_len) {
    return parser_file_summary(app_relative_path, "SubGHz", out, out_len);
}

void plugin_api_lowlevel_release(void) {
    if (s_wifi_packet_cb || s_wifi_pcap_active) {
        if (s_wifi_pcap_active) {
            pcap_flush_buffer_to_file();
            pcap_file_close();
            s_wifi_pcap_active = false;
        }
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        s_wifi_packet_cb = NULL;
        s_wifi_packet_user = NULL;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (s_ble_gatt_conn_handle != 0xffff) {
        ble_gap_terminate(s_ble_gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_ble_gatt_conn_handle = 0xffff;
    }
#endif
#ifdef CONFIG_HAS_NRF24
    nrf24_remote_manager_stop();
#endif
    for (int i = 0; i < PLUGIN_TASK_MAX; i++) {
        if (!s_tasks[i]) continue;
        vTaskDelete(s_tasks[i]);
        s_tasks[i] = NULL;
    }
    socket_slots_init();
    for (int i = 0; i < PLUGIN_SOCKET_MAX; i++) {
        if (s_sockets[i] < 0) continue;
        close(s_sockets[i]);
        s_sockets[i] = -1;
    }
    if (s_events) memset(s_events, 0, PLUGIN_EVENT_MAX * sizeof(*s_events));
    if (s_gpio_intr) {
        for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
            if (!s_gpio_intr[pin].active) continue;
            gpio_isr_handler_remove((gpio_num_t)pin);
            gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
            memset(&s_gpio_intr[pin], 0, sizeof(*s_gpio_intr));
        }
    }
    for (int i = 0; i < PLUGIN_PWM_MAX_CHANNELS; i++) {
        if (!s_pwm_slots[i].active) continue;
        ledc_stop(LEDC_LOW_SPEED_MODE, s_pwm_slots[i].channel, 0);
        memset(&s_pwm_slots[i], 0, sizeof(s_pwm_slots[i]));
    }
    for (int i = 0; i < PLUGIN_SPI_MAX_DEVICES; i++) {
        if (!s_spi_slots[i].active) continue;
        if (s_spi_slots[i].dev) spi_bus_remove_device(s_spi_slots[i].dev);
        if (s_spi_slots[i].bus_owned) spi_bus_free(s_spi_slots[i].host);
        memset(&s_spi_slots[i], 0, sizeof(s_spi_slots[i]));
    }
    for (int i = 0; i < UART_NUM_MAX; i++) {
        if (!s_uart_open[i]) continue;
        uart_share_release((uart_port_t)i, UART_SHARE_OWNER_PLUGIN);
        s_uart_open[i] = false;
    }
    if (s_adc_unit) {
        adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
        memset(s_adc_channels, 0, sizeof(s_adc_channels));
    }
}
