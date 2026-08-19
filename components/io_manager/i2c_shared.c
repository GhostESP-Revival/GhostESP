#include "i2c_shared.h"
#include "i2c_bus_lock.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "i2c_shared";

/* --------------------------------------------------------------------------
 * Bus registry. Bus handles created through this module are remembered per
 * port so get_or_create is cheap, creation is serialized against concurrent
 * callers, and we can map a bus handle back to its port for locking.
 * ------------------------------------------------------------------------ */
static i2c_master_bus_handle_t s_bus_handles[I2C_NUM_MAX];

static int bus_to_port(i2c_master_bus_handle_t bus)
{
    for (int p = 0; p < I2C_NUM_MAX; p++) {
        if (s_bus_handles[p] == bus) {
            return p;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Device cache. Persistent handles keyed by (addr, scl_speed_hz).
 *
 * The old code added a device, transmitted, and removed it again on every
 * transaction. i2c_master_bus_rm_device() rejects removal while the bus is
 * busy, so when two tasks used the bus at the same time (touch poll vs. IO
 * manager button poll) the removal failed with "Wrong I2C status" and the
 * handle leaked. Cached handles sidestep that entirely.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint16_t addr;
    uint32_t scl_speed_hz;
    i2c_master_dev_handle_t dev;
} i2c_shared_cached_dev_t;

static i2c_shared_cached_dev_t s_dev_cache[I2C_NUM_MAX][I2C_SHARED_MAX_CACHED_DEVICES];

static int dev_cache_find(i2c_port_num_t port, uint16_t addr, uint32_t scl_speed_hz)
{
    for (int i = 0; i < I2C_SHARED_MAX_CACHED_DEVICES; i++) {
        if (s_dev_cache[port][i].dev &&
            s_dev_cache[port][i].addr == addr &&
            s_dev_cache[port][i].scl_speed_hz == scl_speed_hz) {
            return i;
        }
    }
    return -1;
}

static esp_err_t with_temp_device(i2c_master_bus_handle_t bus,
                                  uint16_t addr,
                                  uint32_t scl_speed_hz,
                                  i2c_master_dev_handle_t *out_dev)
{
    if (!bus || !out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
    };

    return i2c_master_bus_add_device(bus, &dev_config, out_dev);
}

esp_err_t i2c_shared_get_or_create_bus(i2c_port_num_t port,
                                       gpio_num_t sda,
                                       gpio_num_t scl,
                                       bool enable_internal_pullup,
                                       i2c_master_bus_handle_t *out_bus,
                                       bool *out_created)
{
    if (!out_bus || port < 0 || port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_created) {
        *out_created = false;
    }

    /* Serialize lookup + creation so concurrent callers can't race to
     * create the same bus. */
    if (!i2c_bus_lock((int)port, 100)) {
        ESP_LOGE(TAG, "failed to lock I2C port %d for get/create", (int)port);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    if (s_bus_handles[port]) {
        *out_bus = s_bus_handles[port];
        goto out;
    }

    ret = i2c_master_get_bus_handle(port, out_bus);
    if (ret == ESP_OK) {
        s_bus_handles[port] = *out_bus;
        goto out;
    }
    if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_master_get_bus_handle port %d failed: %s", (int)port, esp_err_to_name(ret));
        goto out;
    }

    ESP_LOGI(TAG, "Creating I2C master bus on port %d (SDA=%d, SCL=%d, pullup=%d)",
             (int)port, (int)sda, (int)scl, enable_internal_pullup);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = enable_internal_pullup,
    };

    ret = i2c_new_master_bus(&bus_config, out_bus);
    if (ret == ESP_OK) {
        s_bus_handles[port] = *out_bus;
        if (out_created) {
            *out_created = true;
        }
        ESP_LOGI(TAG, "I2C master bus created on port %d", (int)port);
    } else {
        ESP_LOGE(TAG, "i2c_new_master_bus port %d (SDA=%d, SCL=%d) failed: %s",
                 (int)port, (int)sda, (int)scl, esp_err_to_name(ret));
    }

out:
    i2c_bus_unlock((int)port);
    return ret;
}

esp_err_t i2c_shared_release_bus(i2c_port_num_t port)
{
    if (port < 0 || port >= I2C_NUM_MAX) return ESP_ERR_INVALID_ARG;
    if (!i2c_bus_lock((int)port, 100)) return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < I2C_SHARED_MAX_CACHED_DEVICES; i++) {
        if (s_dev_cache[port][i].dev) {
            esp_err_t rm_ret = i2c_master_bus_rm_device(s_dev_cache[port][i].dev);
            if (ret == ESP_OK && rm_ret != ESP_OK) ret = rm_ret;
            memset(&s_dev_cache[port][i], 0, sizeof(s_dev_cache[port][i]));
        }
    }

    if (s_bus_handles[port]) {
        esp_err_t del_ret = i2c_del_master_bus(s_bus_handles[port]);
        if (ret == ESP_OK && del_ret != ESP_OK) ret = del_ret;
        if (del_ret == ESP_OK) s_bus_handles[port] = NULL;
    }

    i2c_bus_unlock((int)port);
    return ret;
}

esp_err_t i2c_shared_add_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev)
{
    return with_temp_device(bus, addr, scl_speed_hz, out_dev);
}

esp_err_t i2c_shared_get_device(i2c_master_bus_handle_t bus,
                                uint16_t addr,
                                uint32_t scl_speed_hz,
                                i2c_master_dev_handle_t *out_dev)
{
    if (!bus || !out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    int port = bus_to_port(bus);
    if (port < 0) {
        /* Bus not created through this module (e.g. BSP-owned): fall back to
         * an uncached handle rather than fail. */
        return with_temp_device(bus, addr, scl_speed_hz, out_dev);
    }

    if (!i2c_bus_lock(port, 100)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    int slot = dev_cache_find((i2c_port_num_t)port, addr, scl_speed_hz);
    if (slot >= 0) {
        *out_dev = s_dev_cache[port][slot].dev;
        goto out;
    }

    slot = -1;
    for (int i = 0; i < I2C_SHARED_MAX_CACHED_DEVICES; i++) {
        if (!s_dev_cache[port][i].dev) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "device cache full on port %d (addr 0x%02X), creating uncached handle",
                 port, addr);
        ret = with_temp_device(bus, addr, scl_speed_hz, out_dev);
        goto out;
    }

    i2c_master_dev_handle_t dev = NULL;
    ret = with_temp_device(bus, addr, scl_speed_hz, &dev);
    if (ret == ESP_OK) {
        s_dev_cache[port][slot].addr = addr;
        s_dev_cache[port][slot].scl_speed_hz = scl_speed_hz;
        s_dev_cache[port][slot].dev = dev;
        *out_dev = dev;
    }

out:
    i2c_bus_unlock(port);
    return ret;
}

bool i2c_shared_bus_lock(i2c_master_bus_handle_t bus, int timeout_ms)
{
    int port = bus_to_port(bus);
    if (port < 0) {
        return false;
    }
    return i2c_bus_lock(port, timeout_ms);
}

void i2c_shared_bus_unlock(i2c_master_bus_handle_t bus)
{
    int port = bus_to_port(bus);
    if (port >= 0) {
        i2c_bus_unlock(port);
    }
}

esp_err_t i2c_shared_transmit_to_addr(i2c_master_bus_handle_t bus,
                                      uint16_t addr,
                                      uint32_t scl_speed_hz,
                                      const uint8_t *data,
                                      size_t len,
                                      int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_shared_get_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Fail-open if the bus isn't registered (unknown bus): the driver still
     * serializes individual transactions on the wire. */
    bool locked = i2c_shared_bus_lock(bus, timeout_ms);
    ret = i2c_master_transmit(dev, data, len, timeout_ms);
    if (locked) {
        i2c_shared_bus_unlock(bus);
    }
    return ret;
}

esp_err_t i2c_shared_receive_from_addr(i2c_master_bus_handle_t bus,
                                       uint16_t addr,
                                       uint32_t scl_speed_hz,
                                       uint8_t *data,
                                       size_t len,
                                       int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_shared_get_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    bool locked = i2c_shared_bus_lock(bus, timeout_ms);
    ret = i2c_master_receive(dev, data, len, timeout_ms);
    if (locked) {
        i2c_shared_bus_unlock(bus);
    }
    return ret;
}

esp_err_t i2c_shared_transmit_receive_from_addr(i2c_master_bus_handle_t bus,
                                                uint16_t addr,
                                                uint32_t scl_speed_hz,
                                                const uint8_t *tx_data,
                                                size_t tx_len,
                                                uint8_t *rx_data,
                                                size_t rx_len,
                                                int timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_shared_get_device(bus, addr, scl_speed_hz, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    bool locked = i2c_shared_bus_lock(bus, timeout_ms);
    ret = i2c_master_transmit_receive(dev, tx_data, tx_len, rx_data, rx_len, timeout_ms);
    if (locked) {
        i2c_shared_bus_unlock(bus);
    }
    return ret;
}
