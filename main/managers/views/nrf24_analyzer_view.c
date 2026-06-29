#include "managers/views/nrf24_analyzer_view.h"
#include "sdkconfig.h"

/* #define NRF24_JAM_DETECT_DEBUG */

#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)

#include "managers/views/options_screen.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "core/esp_comm_manager.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NRF24_CHANNEL_COUNT 126
#define ANALYZER_TIMER_MS 45
#define NRF24_STREAM_VERSION 1

#define NRF_CMD_R_REGISTER 0x00
#define NRF_CMD_W_REGISTER 0x20
#define NRF_CMD_FLUSH_TX   0xE1
#define NRF_CMD_FLUSH_RX   0xE2
#define NRF_CMD_NOP        0xFF

#define NRF_REG_CONFIG     0x00
#define NRF_REG_EN_AA      0x01
#define NRF_REG_EN_RXADDR  0x02
#define NRF_REG_SETUP_AW   0x03
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_STATUS     0x07
#define NRF_REG_RX_ADDR_P0 0x0A
#define NRF_REG_RX_PW_P0   0x11
#define NRF_REG_DYNPD      0x1C
#define NRF_REG_FEATURE    0x1D
#define NRF_REG_RPD        0x09

typedef enum {
    CONTROL_TOGGLE = 0,
    CONTROL_BACK = 1,
    CONTROL_COUNT
} nrf24_control_t;

static const char *TAG = "NRF24View";

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_graph = NULL;
static lv_obj_t *s_toggle_btn = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_toggle_label = NULL;
static lv_timer_t *s_timer = NULL;

#ifdef CONFIG_HAS_NRF24
static spi_device_handle_t s_spi_dev = NULL;
static spi_host_device_t s_spi_host = SPI3_HOST;
static bool s_spi_bus_initialized_by_us = false;
#endif
static bool s_hw_ready = false;
static bool s_paused = false;
static bool s_remote_mode = false;
static volatile bool s_remote_stream_online = false;
static volatile bool s_remote_error = false;

static uint8_t s_levels[NRF24_CHANNEL_COUNT];
static uint8_t s_peaks[NRF24_CHANNEL_COUNT];
static uint8_t s_next_channel = 0;
static uint32_t s_tick_count = 0;
static nrf24_control_t s_selected_control = CONTROL_TOGGLE;

#define JAM_DETECT_THRESHOLD     60
#define JAM_DETECT_MIN_CHANNELS  15
#define JAM_DETECT_GAP_CHANNELS  8
#define JAM_DETECT_HISTORY_SIZE  4
#define JAM_DETECT_CONFIDENCE_TICKS 6
#define JAM_DETECT_SIMULTANEOUS_THRESHOLD 3

typedef enum {
    JAM_NONE = 0,
    JAM_BROADBAND,
    JAM_SIG_FW_A_BLE,
    JAM_SIG_FW_A_WIFI,
    JAM_SIG_FW_A_NRF,
    JAM_SIG_FW_B_BLE,
    JAM_SIG_FW_B_WIFI,
    JAM_SIG_FW_B_NRF,
    JAM_SIG_FW_B_MULTI,
    JAM_UNKNOWN_SWEEP
} jam_type_t;

static jam_type_t s_jam_type = JAM_NONE;
static uint8_t s_jam_confidence = 0;
static uint8_t s_jam_strength = 0;
static int8_t s_jam_peak_ch = -1;
static uint8_t s_jam_active_count_history[JAM_DETECT_HISTORY_SIZE];
static uint8_t s_jam_history_idx = 0;

#ifdef NRF24_JAM_DETECT_DEBUG
typedef enum {
    DBG_SCENE_IDLE = 0,
    DBG_SCENE_FW_A_SWEEP,
    DBG_SCENE_FW_A_BLE,
    DBG_SCENE_FW_A_WIFI,
    DBG_SCENE_FW_A_NRF,
    DBG_SCENE_FW_B_WIFI,
    DBG_SCENE_FW_B_BLE,
    DBG_SCENE_FW_B_NRF,
    DBG_SCENE_FW_B_MULTI,
    DBG_SCENE_COUNT
} debug_scene_t;

static const char *s_debug_scene_names[DBG_SCENE_COUNT] = {
    "IDLE",
    "FW-A SWEEP ALL",
    "FW-A BLE ADV",
    "FW-A WIFI STEP",
    "FW-A HI-BAND",
    "FW-B WIFI CONT",
    "FW-B BLE ADV",
    "FW-B HI-BAND",
    "FW-B TRI-RADIO"
};

static debug_scene_t s_debug_scene = DBG_SCENE_IDLE;
static uint8_t s_debug_sweep_pos = 0;
static bool s_debug_active = false;

static void nrf24_debug_inject_levels(void);
#endif

#ifdef CONFIG_HAS_NRF24
static void nrf24_hw_stop(void);
#endif

static bool nrf24_is_remote_mode(void) {
#if defined(CONFIG_HAS_NRF24_REMOTE) && !defined(CONFIG_HAS_NRF24)
    return true;
#else
    return false;
#endif
}

#ifdef CONFIG_HAS_NRF24
static inline spi_host_device_t nrf24_spi_host_from_config(void) {
#if defined(SPI3_HOST)
    if (CONFIG_NRF24_SPI_HOST == 2) {
        return SPI2_HOST;
    }
    return SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}
#endif

static bool point_inside_obj(lv_obj_t *obj, lv_coord_t x, lv_coord_t y) {
    if (!obj || !lv_obj_is_valid(obj)) {
        return false;
    }

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2);
}

#ifdef CONFIG_HAS_NRF24
static esp_err_t nrf24_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_spi_dev || !tx || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (uint32_t)(len * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(s_spi_dev, &t);
}

static esp_err_t nrf24_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = { (uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F)), value };
    uint8_t rx[2] = {0};
    return nrf24_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t nrf24_read_reg(uint8_t reg, uint8_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[2] = { (uint8_t)(NRF_CMD_R_REGISTER | (reg & 0x1F)), NRF_CMD_NOP };
    uint8_t rx[2] = {0};
    esp_err_t err = nrf24_spi_transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

static esp_err_t nrf24_write_reg_buf(uint8_t reg, const uint8_t *buf, size_t len) {
    if (!buf || len == 0 || len > 5) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[6] = {0};
    uint8_t rx[6] = {0};
    tx[0] = (uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F));
    memcpy(&tx[1], buf, len);
    return nrf24_spi_transfer(tx, rx, len + 1);
}

static esp_err_t nrf24_cmd(uint8_t cmd) {
    uint8_t tx[1] = { cmd };
    uint8_t rx[1] = {0};
    return nrf24_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t nrf24_set_channel(uint8_t channel) {
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);
    esp_err_t err = nrf24_write_reg(NRF_REG_RF_CH, (uint8_t)(channel & 0x7F));
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 1);
    return err;
}
#endif

static void nrf24_apply_control_selection(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t selected_border = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t normal_border = lv_color_hex(theme_palette_get_surface_alt(theme));

    if (s_toggle_btn && lv_obj_is_valid(s_toggle_btn)) {
        bool sel = (s_selected_control == CONTROL_TOGGLE);
        lv_obj_set_style_border_width(s_toggle_btn, sel ? 3 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_toggle_btn, sel ? selected_border : normal_border, LV_PART_MAIN);
    }
    if (s_back_btn && lv_obj_is_valid(s_back_btn)) {
        bool sel = (s_selected_control == CONTROL_BACK);
        lv_obj_set_style_border_width(s_back_btn, sel ? 3 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_back_btn, sel ? selected_border : normal_border, LV_PART_MAIN);
    }
}

static void nrf24_update_pause_ui(void) {
    if (s_toggle_label && lv_obj_is_valid(s_toggle_label)) {
        lv_label_set_text(s_toggle_label, s_paused ? "Resume" : "Pause");
    }

    if (s_status_label && lv_obj_is_valid(s_status_label)) {
        if (s_remote_mode) {
            if (s_remote_error) {
                lv_label_set_text(s_status_label, "Peer error");
            } else if (!esp_comm_manager_is_connected()) {
                lv_label_set_text(s_status_label, "GhostLink disconnected");
            } else if (!s_remote_stream_online) {
                lv_label_set_text(s_status_label, "Waiting for peer stream...");
            } else {
                lv_label_set_text(s_status_label, s_paused ? "Remote scan paused" : "Remote scan active");
            }
        } else if (!s_hw_ready) {
            lv_label_set_text(s_status_label, "NRF24 init failed");
        } else {
            lv_label_set_text(s_status_label, s_paused ? "Analyzer paused" : "Analyzer scanning");
        }
    }
}

static void nrf24_return_to_menu(void) {
    SelectedMenuType = OT_NRF24;
    display_manager_switch_view(&options_menu_view);
}

static void nrf24_toggle_pause(void) {
#ifdef NRF24_JAM_DETECT_DEBUG
    s_debug_active = true;
    s_debug_scene = (debug_scene_t)((s_debug_scene + 1) % DBG_SCENE_COUNT);
    s_jam_confidence = 0;
    s_jam_type = JAM_NONE;
    s_jam_strength = 0;
    s_jam_peak_ch = -1;
    memset(s_peaks, 0, sizeof(s_peaks));
    memset(s_jam_active_count_history, 0, sizeof(s_jam_active_count_history));
    s_jam_history_idx = 0;
    if (s_status_label && lv_obj_is_valid(s_status_label)) {
        lv_label_set_text_fmt(s_status_label, "DBG: %s", s_debug_scene_names[s_debug_scene]);
    }
    if (s_toggle_label && lv_obj_is_valid(s_toggle_label)) {
        lv_label_set_text(s_toggle_label, "Next");
    }
    return;
#endif

    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            nrf24_update_pause_ui();
            return;
        }

        if (s_paused) {
            if (esp_comm_manager_send_command("nrf24", "resume")) {
                s_paused = false;
            }
        } else {
            if (esp_comm_manager_send_command("nrf24", "pause")) {
                s_paused = true;
            }
        }

        nrf24_update_pause_ui();
        return;
    }

    if (!s_hw_ready) {
        return;
    }

    s_paused = !s_paused;
    nrf24_update_pause_ui();
}

#ifdef CONFIG_HAS_NRF24
static esp_err_t nrf24_hw_start(void) {
    s_spi_host = nrf24_spi_host_from_config();

    ESP_LOGI(TAG,
             "NRF24 init SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d",
             (int)s_spi_host,
             CONFIG_NRF24_SPI_MOSI_PIN,
             CONFIG_NRF24_SPI_MISO_PIN,
             CONFIG_NRF24_SPI_SCK_PIN,
             CONFIG_NRF24_CSN_PIN,
             CONFIG_NRF24_CE_PIN);

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_NRF24_CE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CE gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_NRF24_SPI_MOSI_PIN,
        .miso_io_num = CONFIG_NRF24_SPI_MISO_PIN,
        .sclk_io_num = CONFIG_NRF24_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_spi_bus_initialized_by_us = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_spi_bus_initialized_by_us = false;
        err = ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_NRF24_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_NRF24_CSN_PIN,
        .queue_size = 1,
    };

    err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(s_spi_host);
            s_spi_bus_initialized_by_us = false;
        }
        s_spi_dev = NULL;
        return err;
    }

    uint8_t rx_addr[5] = { 0xE7, 0xE7, 0xE7, 0xE7, 0xE7 };

    err = nrf24_write_reg(NRF_REG_EN_AA, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_EN_RXADDR, 0x01);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_SETUP_AW, 0x03);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_RF_SETUP, 0x07);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_RX_PW_P0, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_DYNPD, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_FEATURE, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg_buf(NRF_REG_RX_ADDR_P0, rx_addr, sizeof(rx_addr));
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_STATUS, 0x70);
    if (err == ESP_OK) err = nrf24_cmd(NRF_CMD_FLUSH_RX);
    if (err == ESP_OK) err = nrf24_cmd(NRF_CMD_FLUSH_TX);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_CONFIG, 0x03);
    if (err == ESP_OK) err = nrf24_set_channel(0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF24 init sequence failed: %s", esp_err_to_name(err));
        nrf24_hw_stop();
        return err;
    }

    ets_delay_us(2000);
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 1);
    return ESP_OK;
}

static void nrf24_hw_stop(void) {
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);

    if (s_spi_dev) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }

    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
    }

    s_hw_ready = false;
}
#endif

static void nrf24_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data || length < 4) {
        return;
    }
    if (data[0] != NRF24_STREAM_VERSION) {
        return;
    }

    uint8_t cursor = data[1];
    uint8_t start_ch = data[2];
    uint8_t count = data[3];

    if (count == 0 || count > 32 || (size_t)(4 + count) > length) {
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)((start_ch + i) % NRF24_CHANNEL_COUNT);
        uint8_t level = data[4 + i];
        s_levels[ch] = level;
        if (level > s_peaks[ch]) {
            s_peaks[ch] = level;
        }
    }

    s_next_channel = (uint8_t)(cursor % NRF24_CHANNEL_COUNT);
    s_remote_stream_online = true;
    s_remote_error = false;
    s_hw_ready = true;
}

void nrf24_analyzer_register_stream_handler(void) {
    (void)esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_NRF24, nrf24_stream_rx_cb, NULL);
}

void nrf24_analyzer_view_update_remote_state(const char *state) {
    if (!state) {
        return;
    }

    if (strcmp(state, "started") == 0 || strcmp(state, "running") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_hw_ready = true;
        s_paused = false;
    } else if (strcmp(state, "paused") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_hw_ready = true;
        s_paused = true;
    } else if (strcmp(state, "resumed") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_hw_ready = true;
        s_paused = false;
    } else if (strcmp(state, "stopped") == 0) {
        s_remote_stream_online = false;
        s_paused = true;
    } else if (strcmp(state, "error") == 0) {
        s_remote_error = true;
        s_remote_stream_online = false;
        s_paused = true;
        s_hw_ready = false;
    }
}

static void nrf24_detect_jamming(void) {
    int active_count = 0;
    int gap_active = 0;
    int total_strength = 0;
    int best_ch = 0;
    uint8_t best_lvl = 0;
    int max_consecutive = 0;

    static const uint8_t gap_channels[] = {24, 25, 49, 50, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83};

    // ========================================================================
    // THREAT SIGNATURE ARRAYS
    // Mapped from known wild firmware channel-hopping behaviors.
    // ========================================================================

    // Threat Actor: Firmware A (Known for 5-channel step patterns and
    // sequential sweeps across the full 2.4GHz band)
    static const uint8_t fw_a_ble[] = {2, 26, 80};
    static const uint8_t fw_a_wifi[] = {2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72};
    static const uint8_t fw_a_nrf[] = {76, 78, 79};

    // Threat Actor: Firmware B (Known for continuous lower-band block sweeps
    // and simultaneous multi-radio carrier injection)
    static const uint8_t fw_b_wifi[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    static const uint8_t fw_b_ble[] = {2, 26, 80};
    static const uint8_t fw_b_nrf[] = {76, 78, 79};

    bool in_run = false;
    int run_start = 0;

    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        if (s_levels[ch] >= JAM_DETECT_THRESHOLD) {
            active_count++;
            total_strength += s_levels[ch];
            if (s_levels[ch] > best_lvl) {
                best_lvl = s_levels[ch];
                best_ch = ch;
            }
            if (!in_run) {
                in_run = true;
                run_start = ch;
            }
        } else {
            if (in_run) {
                int run_len = ch - run_start;
                if (run_len > max_consecutive) {
                    max_consecutive = run_len;
                }
                in_run = false;
            }
        }
    }
    if (in_run) {
        int run_len = NRF24_CHANNEL_COUNT - run_start;
        if (run_len > max_consecutive) {
            max_consecutive = run_len;
        }
    }

    for (int i = 0; i < (int)(sizeof(gap_channels) / sizeof(gap_channels[0])); ++i) {
        if (s_levels[gap_channels[i]] >= JAM_DETECT_THRESHOLD) {
            gap_active++;
        }
    }

    s_jam_active_count_history[s_jam_history_idx % JAM_DETECT_HISTORY_SIZE] = (uint8_t)active_count;
    s_jam_history_idx++;

    int hist_active = 0;
    for (int i = 0; i < JAM_DETECT_HISTORY_SIZE; ++i) {
        hist_active += s_jam_active_count_history[i];
    }
    int avg_active = hist_active / JAM_DETECT_HISTORY_SIZE;

    int fw_a_ble_hits = 0;
    for (int i = 0; i < (int)(sizeof(fw_a_ble) / sizeof(fw_a_ble[0])); ++i) {
        if (s_levels[fw_a_ble[i]] >= JAM_DETECT_THRESHOLD) fw_a_ble_hits++;
    }

    int fw_a_wifi_hits = 0;
    for (int i = 0; i < (int)(sizeof(fw_a_wifi) / sizeof(fw_a_wifi[0])); ++i) {
        if (s_levels[fw_a_wifi[i]] >= JAM_DETECT_THRESHOLD) fw_a_wifi_hits++;
    }

    int fw_a_nrf_hits = 0;
    for (int i = 0; i < (int)(sizeof(fw_a_nrf) / sizeof(fw_a_nrf[0])); ++i) {
        if (s_levels[fw_a_nrf[i]] >= JAM_DETECT_THRESHOLD) fw_a_nrf_hits++;
    }

    int fw_b_wifi_hits = 0;
    int fw_b_wifi_exclusive = 0;
    for (int i = 0; i < (int)(sizeof(fw_b_wifi) / sizeof(fw_b_wifi[0])); ++i) {
        if (s_levels[fw_b_wifi[i]] >= JAM_DETECT_THRESHOLD) {
            fw_b_wifi_hits++;
            fw_b_wifi_exclusive++;
        }
    }
    for (int ch = 13; ch < NRF24_CHANNEL_COUNT; ++ch) {
        if (s_levels[ch] >= JAM_DETECT_THRESHOLD) {
            fw_b_wifi_exclusive = -1;
            break;
        }
    }

    int fw_b_ble_hits = 0;
    for (int i = 0; i < (int)(sizeof(fw_b_ble) / sizeof(fw_b_ble[0])); ++i) {
        if (s_levels[fw_b_ble[i]] >= JAM_DETECT_THRESHOLD) fw_b_ble_hits++;
    }

    int fw_b_nrf_hits = 0;
    for (int i = 0; i < (int)(sizeof(fw_b_nrf) / sizeof(fw_b_nrf[0])); ++i) {
        if (s_levels[fw_b_nrf[i]] >= JAM_DETECT_THRESHOLD) fw_b_nrf_hits++;
    }

    int sim_count = 0;
    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        if (s_levels[ch] >= 90) sim_count++;
    }
    bool triple_radio = (sim_count >= JAM_DETECT_SIMULTANEOUS_THRESHOLD && avg_active <= 10);

    jam_type_t detected = JAM_NONE;

    if (gap_active >= JAM_DETECT_GAP_CHANNELS && avg_active >= JAM_DETECT_MIN_CHANNELS) {
        detected = JAM_BROADBAND;
    } else if (max_consecutive >= 23 && avg_active >= JAM_DETECT_MIN_CHANNELS) {
        detected = JAM_BROADBAND;
    } else if (triple_radio) {
        if (fw_b_ble_hits == 3 && fw_b_nrf_hits == 3) {
            detected = JAM_SIG_FW_B_MULTI;
        } else if (fw_b_wifi_exclusive > 0 && fw_b_nrf_hits == 3) {
            detected = JAM_SIG_FW_B_MULTI;
        } else if (fw_b_ble_hits == 3) {
            detected = JAM_SIG_FW_B_BLE;
        } else if (fw_b_nrf_hits == 3) {
            detected = JAM_SIG_FW_B_NRF;
        } else if (fw_b_wifi_exclusive > 5) {
            detected = JAM_SIG_FW_B_WIFI;
        } else {
            detected = JAM_SIG_FW_B_MULTI;
        }
    } else if (fw_b_wifi_exclusive > 5 && avg_active <= 12) {
        detected = JAM_SIG_FW_B_WIFI;
    } else if (fw_a_nrf_hits == 3 && gap_active < JAM_DETECT_GAP_CHANNELS) {
        detected = JAM_SIG_FW_A_NRF;
    } else if (fw_a_ble_hits == 3 && avg_active <= 5) {
        detected = JAM_SIG_FW_A_BLE;
    } else if (fw_a_wifi_hits >= 10 && avg_active >= 10 && gap_active < JAM_DETECT_GAP_CHANNELS) {
        detected = JAM_SIG_FW_A_WIFI;
    } else if (avg_active >= JAM_DETECT_MIN_CHANNELS) {
        detected = JAM_UNKNOWN_SWEEP;
    }

    if (detected != JAM_NONE) {
        if (s_jam_type == detected || s_jam_confidence == 0) {
            s_jam_confidence++;
            if (s_jam_confidence > JAM_DETECT_CONFIDENCE_TICKS) {
                s_jam_confidence = JAM_DETECT_CONFIDENCE_TICKS;
            }
        } else {
            s_jam_confidence = 1;
        }
        s_jam_type = detected;
        s_jam_strength = (uint8_t)(active_count > 0 ? total_strength / active_count : 0);
        s_jam_peak_ch = (int8_t)best_ch;
    } else {
        if (s_jam_confidence > 0) {
            s_jam_confidence--;
        }
        if (s_jam_confidence == 0) {
            s_jam_type = JAM_NONE;
            s_jam_strength = 0;
            s_jam_peak_ch = -1;
        }
    }
}

static const char *nrf24_jam_type_str(jam_type_t t) {
    switch (t) {
        case JAM_BROADBAND:       return "BROADBAND NOISE";
        case JAM_SIG_FW_A_BLE:   return "SIG: FW-A BLE ADV";
        case JAM_SIG_FW_A_WIFI:  return "SIG: FW-A WIFI";
        case JAM_SIG_FW_A_NRF:   return "SIG: FW-A HI-BAND";
        case JAM_SIG_FW_B_BLE:   return "SIG: FW-B BLE ADV";
        case JAM_SIG_FW_B_WIFI:  return "SIG: FW-B WIFI";
        case JAM_SIG_FW_B_NRF:   return "SIG: FW-B HI-BAND";
        case JAM_SIG_FW_B_MULTI: return "SIG: FW-B TRI-RADIO";
        case JAM_UNKNOWN_SWEEP:  return "GENERIC SWEEP";
        default:                 return NULL;
    }
}

static void nrf24_graph_draw_event(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    if (!obj || obj != s_graph) {
        return;
    }

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t bg_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t text_muted = lv_color_hex(theme_palette_get_text_muted(theme));

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.bg_color = bg;
    rect_dsc.border_width = 1;
    rect_dsc.border_color = bg_alt;
    rect_dsc.radius = 6;
    lv_draw_rect(draw_ctx, &rect_dsc, &coords);

    lv_coord_t pad_x = 8;
    lv_coord_t pad_y = 10;
    lv_coord_t y_axis_w = 24;
    lv_coord_t x_axis_h = 16;
    lv_coord_t plot_x1 = coords.x1 + pad_x + y_axis_w;
    lv_coord_t plot_x2 = coords.x2 - pad_x;
    lv_coord_t plot_y1 = coords.y1 + pad_y;
    lv_coord_t plot_y2 = coords.y2 - pad_y - x_axis_h;
    lv_coord_t plot_w = plot_x2 - plot_x1 + 1;
    lv_coord_t plot_h = plot_y2 - plot_y1 + 1;

    if (plot_w <= 0 || plot_h <= 0) {
        return;
    }

    rect_dsc.radius = 0;
    rect_dsc.border_width = 0;
    rect_dsc.bg_color = bg_alt;
    for (int g = 1; g <= 4; ++g) {
        lv_coord_t gy = plot_y2 - (plot_h * g) / 4;
        lv_area_t line_area = {
            .x1 = plot_x1,
            .y1 = gy,
            .x2 = plot_x2,
            .y2 = gy
        };
        lv_draw_rect(draw_ctx, &rect_dsc, &line_area);
    }

    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        lv_coord_t bx1 = plot_x1 + (ch * plot_w) / NRF24_CHANNEL_COUNT;
        lv_coord_t bx2 = plot_x1 + ((ch + 1) * plot_w) / NRF24_CHANNEL_COUNT - 1;
        if (bx2 < bx1) {
            bx2 = bx1;
        }

        lv_coord_t bar_h = (lv_coord_t)((s_levels[ch] * plot_h) / 100);
        if (bar_h <= 0) {
            continue;
        }

        lv_area_t bar_area = {
            .x1 = bx1,
            .y1 = plot_y2 - bar_h + 1,
            .x2 = bx2,
            .y2 = plot_y2
        };

        rect_dsc.bg_color = accent;
        lv_draw_rect(draw_ctx, &rect_dsc, &bar_area);

        if (s_peaks[ch] > 0) {
            lv_coord_t peak_y = plot_y2 - (lv_coord_t)((s_peaks[ch] * plot_h) / 100);
            lv_area_t peak_area = {
                .x1 = bx1,
                .y1 = peak_y,
                .x2 = bx2,
                .y2 = peak_y
            };
            rect_dsc.bg_color = text;
            lv_draw_rect(draw_ctx, &rect_dsc, &peak_area);
        }
    }

    lv_coord_t cursor_x = plot_x1 + (s_next_channel * plot_w) / NRF24_CHANNEL_COUNT;
    lv_area_t cursor_area = {
        .x1 = cursor_x,
        .y1 = plot_y1,
        .x2 = cursor_x,
        .y2 = plot_y2
    };
    rect_dsc.bg_color = text_muted;
    lv_draw_rect(draw_ctx, &rect_dsc, &cursor_area);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = text;
    label_dsc.font = accessibility_get_font_small();

    lv_coord_t y_label_x1 = plot_x1 - 18;
    lv_coord_t y_label_x2 = plot_x1 - 2;

    lv_area_t y_75_txt = {
        .x1 = y_label_x1,
        .y1 = plot_y1 + (plot_h / 4) - 6,
        .x2 = y_label_x2,
        .y2 = plot_y1 + (plot_h / 4) + 6
    };
    lv_draw_label(draw_ctx, &label_dsc, &y_75_txt, "75", NULL);

    lv_area_t y_50_txt = {
        .x1 = y_label_x1,
        .y1 = plot_y1 + (plot_h / 2) - 6,
        .x2 = y_label_x2,
        .y2 = plot_y1 + (plot_h / 2) + 6
    };
    lv_draw_label(draw_ctx, &label_dsc, &y_50_txt, "50", NULL);

    lv_area_t y_25_txt = {
        .x1 = y_label_x1,
        .y1 = plot_y1 + ((plot_h * 3) / 4) - 6,
        .x2 = y_label_x2,
        .y2 = plot_y1 + ((plot_h * 3) / 4) + 6
    };
    lv_draw_label(draw_ctx, &label_dsc, &y_25_txt, "25", NULL);

    lv_area_t y_0_txt = {
        .x1 = y_label_x1,
        .y1 = plot_y2 - 6,
        .x2 = y_label_x2,
        .y2 = plot_y2 + 6
    };
    lv_draw_label(draw_ctx, &label_dsc, &y_0_txt, "0", NULL);

    lv_area_t left_txt = {
        .x1 = plot_x1,
        .y1 = plot_y2 + 2,
        .x2 = plot_x1 + 62,
        .y2 = coords.y2 - 1
    };
    lv_draw_label(draw_ctx, &label_dsc, &left_txt, "2.400 GHz", NULL);

    lv_area_t right_txt = {
        .x1 = plot_x2 - 62,
        .y1 = plot_y2 + 2,
        .x2 = plot_x2,
        .y2 = coords.y2 - 1
    };
    lv_draw_label(draw_ctx, &label_dsc, &right_txt, "2.525 GHz", NULL);

    if (s_jam_confidence >= JAM_DETECT_CONFIDENCE_TICKS && s_jam_type != JAM_NONE) {
        lv_color_t alert_bg = lv_color_hex(0xCC0000);
        lv_color_t alert_text = lv_color_hex(0xFFFFFF);

        lv_draw_rect_dsc_t alert_dsc;
        lv_draw_rect_dsc_init(&alert_dsc);
        alert_dsc.bg_opa = 180;
        alert_dsc.bg_color = alert_bg;
        alert_dsc.radius = 0;
        alert_dsc.border_width = 0;

        lv_coord_t alert_h = 14;
        lv_area_t alert_area = {
            .x1 = plot_x1,
            .y1 = plot_y1,
            .x2 = plot_x2,
            .y2 = plot_y1 + alert_h
        };
        lv_draw_rect(draw_ctx, &alert_dsc, &alert_area);

        lv_draw_label_dsc_t alert_lbl;
        lv_draw_label_dsc_init(&alert_lbl);
        alert_lbl.color = alert_text;
        alert_lbl.font = accessibility_get_font_small();

        const char *jam_str = nrf24_jam_type_str(s_jam_type);
        if (!jam_str) jam_str = "JAMMING";

        lv_area_t alert_txt_area = {
            .x1 = plot_x1 + 4,
            .y1 = plot_y1 + 2,
            .x2 = plot_x2 - 4,
            .y2 = plot_y1 + alert_h
        };
        lv_draw_label(draw_ctx, &alert_lbl, &alert_txt_area, jam_str, NULL);

        if (s_jam_peak_ch >= 0) {
            lv_coord_t peak_x = plot_x1 + (s_jam_peak_ch * plot_w) / NRF24_CHANNEL_COUNT;
            lv_draw_rect_dsc_t marker_dsc;
            lv_draw_rect_dsc_init(&marker_dsc);
            marker_dsc.bg_opa = 100;
            marker_dsc.bg_color = alert_bg;
            marker_dsc.radius = 0;
            marker_dsc.border_width = 0;
            lv_area_t marker_area = {
                .x1 = peak_x - 2,
                .y1 = plot_y1 + alert_h,
                .x2 = peak_x + 2,
                .y2 = plot_y2
            };
            lv_draw_rect(draw_ctx, &marker_dsc, &marker_area);
        }
    }

}

static void nrf24_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);

#ifdef NRF24_JAM_DETECT_DEBUG
    if (s_debug_active) {
        nrf24_debug_inject_levels();

        int best_ch = 0;
        uint8_t best_level = 0;
        for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
            if (s_peaks[ch] > best_level) {
                best_level = s_peaks[ch];
                best_ch = ch;
            }
        }

        if (s_freq_label && lv_obj_is_valid(s_freq_label)) {
            if (s_jam_confidence >= JAM_DETECT_CONFIDENCE_TICKS) {
                const char *jam_str = nrf24_jam_type_str(s_jam_type);
                if (jam_str) {
                    lv_label_set_text_fmt(s_freq_label, "%s CH:%03d %u%%", jam_str, (int)s_jam_peak_ch, s_jam_strength);
                } else {
                    lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, (2400 + best_ch) - 2000, best_level);
                }
            } else {
                lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, (2400 + best_ch) - 2000, best_level);
            }
        }

        nrf24_detect_jamming();

        if (s_status_label && lv_obj_is_valid(s_status_label)) {
            lv_label_set_text_fmt(s_status_label, "DBG: %s [%d/%d]",
                s_debug_scene_names[s_debug_scene],
                s_jam_confidence,
                JAM_DETECT_CONFIDENCE_TICKS);
        }

        if ((s_tick_count % 4U) == 0U) {
            for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
                if (s_peaks[ch] > 0) s_peaks[ch]--;
            }
        }
        s_tick_count++;

        if (s_graph && lv_obj_is_valid(s_graph)) {
            lv_obj_invalidate(s_graph);
        }
        return;
    }
#endif

    if (s_remote_mode) {
        for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
            if ((s_tick_count % 6U) == 0U && s_peaks[ch] > 0 && s_levels[ch] < s_peaks[ch]) {
                s_peaks[ch]--;
            }
        }
        s_tick_count++;

        int best_ch = 0;
        uint8_t best_level = 0;
        for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
            if (s_peaks[ch] > best_level) {
                best_level = s_peaks[ch];
                best_ch = ch;
            }
        }

        if (s_freq_label && lv_obj_is_valid(s_freq_label)) {
            int mhz = 2400 + best_ch;
            if (s_jam_confidence >= JAM_DETECT_CONFIDENCE_TICKS) {
                const char *jam_str = nrf24_jam_type_str(s_jam_type);
                if (jam_str) {
                    lv_label_set_text_fmt(s_freq_label, "%s CH:%03d %u%%", jam_str, (int)s_jam_peak_ch, s_jam_strength);
                } else {
                    lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, mhz - 2000, best_level);
                }
            } else {
                lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, mhz - 2000, best_level);
            }
        }

        nrf24_detect_jamming();

        nrf24_update_pause_ui();
        if (s_graph && lv_obj_is_valid(s_graph)) {
            lv_obj_invalidate(s_graph);
        }
        return;
    }

#ifdef CONFIG_HAS_NRF24
    if (!s_hw_ready || s_paused) {
        return;
    }

    int samples_per_channel = CONFIG_NRF24_ANALYZER_SAMPLES_PER_CHANNEL;
    int channels_per_tick = CONFIG_NRF24_ANALYZER_CHANNELS_PER_TICK;
    int settle_us = CONFIG_NRF24_ANALYZER_SETTLE_US;

    if (samples_per_channel < 1) samples_per_channel = 1;
    if (channels_per_tick < 1) channels_per_tick = 1;
    if (settle_us < 130) settle_us = 130;

    for (int i = 0; i < channels_per_tick; ++i) {
        uint8_t ch = s_next_channel;
        s_next_channel = (uint8_t)((s_next_channel + 1) % NRF24_CHANNEL_COUNT);

        int hits = 0;
        for (int s = 0; s < samples_per_channel; ++s) {
            if (nrf24_set_channel(ch) != ESP_OK) {
                continue;
            }

            ets_delay_us((uint32_t)settle_us);

            uint8_t rpd = 0;
            if (nrf24_read_reg(NRF_REG_RPD, &rpd) == ESP_OK && (rpd & 0x01)) {
                hits++;
            }
        }

        uint8_t raw_level = (uint8_t)((hits * 100) / samples_per_channel);
        s_levels[ch] = (uint8_t)((s_levels[ch] * 3 + raw_level) / 4);

        if (s_levels[ch] > s_peaks[ch]) {
            s_peaks[ch] = s_levels[ch];
        } else if ((s_tick_count % 4U) == 0U && s_peaks[ch] > 0) {
            s_peaks[ch]--;
        }
    }

    s_tick_count++;

    int best_ch = 0;
    uint8_t best_level = 0;
    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        if (s_peaks[ch] > best_level) {
            best_level = s_peaks[ch];
            best_ch = ch;
        }
    }

    if (s_freq_label && lv_obj_is_valid(s_freq_label)) {
        int mhz = 2400 + best_ch;
        if (s_jam_confidence >= JAM_DETECT_CONFIDENCE_TICKS) {
            const char *jam_str = nrf24_jam_type_str(s_jam_type);
            if (jam_str) {
                lv_label_set_text_fmt(s_freq_label, "%s CH:%03d %u%%", jam_str, (int)s_jam_peak_ch, s_jam_strength);
            } else {
                lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, mhz - 2000, best_level);
            }
        } else {
            lv_label_set_text_fmt(s_freq_label, "Peak CH:%03d  2.%03d GHz  %u%%", best_ch, mhz - 2000, best_level);
        }
    }

    nrf24_detect_jamming();

    if (s_graph && lv_obj_is_valid(s_graph)) {
        lv_obj_invalidate(s_graph);
    }
#endif
}

#ifdef NRF24_JAM_DETECT_DEBUG
static void nrf24_debug_inject_levels(void) {
    if (!s_debug_active) return;

    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        uint8_t noise = (uint8_t)(esp_timer_get_time() % 11);
        s_levels[ch] = (uint8_t)((s_levels[ch] * 2 + noise) / 3);
    }

    s_debug_sweep_pos = (s_debug_sweep_pos + 1) % 126;

    switch (s_debug_scene) {
        case DBG_SCENE_IDLE:
            break;

        case DBG_SCENE_FW_A_SWEEP: {
            for (int ch = 1; ch <= 83; ++ch) {
                uint8_t base = 85 + (uint8_t)((esp_timer_get_time() + ch * 37) % 16);
                s_levels[ch] = (uint8_t)((s_levels[ch] + base) / 2);
                if (s_levels[ch] > 100) s_levels[ch] = 100;
            }
            int sweep_center = 1 + (s_debug_sweep_pos % 83);
            for (int offset = -2; offset <= 2; ++offset) {
                int ch = sweep_center + offset;
                if (ch >= 1 && ch < NRF24_CHANNEL_COUNT) {
                    s_levels[ch] = 95 + (uint8_t)(esp_timer_get_time() % 6);
                    if (s_levels[ch] > 100) s_levels[ch] = 100;
                }
            }
            break;
        }

        case DBG_SCENE_FW_A_BLE: {
            static const uint8_t ble_ch[] = {2, 26, 80};
            for (int i = 0; i < 3; ++i) {
                s_levels[ble_ch[i]] = 90 + (uint8_t)((esp_timer_get_time() + i * 13) % 11);
                if (s_levels[ble_ch[i]] > 100) s_levels[ble_ch[i]] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_A_WIFI: {
            static const uint8_t wifi_ch[] = {2,7,12,17,22,27,32,37,42,47,52,57,62,67,72};
            int active = s_debug_sweep_pos % 15;
            for (int i = 0; i < 15; ++i) {
                uint8_t lvl;
                if (i == active || i == (active + 1) % 15) {
                    lvl = 92 + (uint8_t)((esp_timer_get_time() + i * 7) % 9);
                } else {
                    lvl = 70 + (uint8_t)((esp_timer_get_time() + i * 17) % 15);
                }
                s_levels[wifi_ch[i]] = lvl;
                if (s_levels[wifi_ch[i]] > 100) s_levels[wifi_ch[i]] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_A_NRF: {
            static const uint8_t nrf_ch[] = {76, 78, 79};
            int active = s_debug_sweep_pos % 3;
            for (int i = 0; i < 3; ++i) {
                uint8_t lvl;
                if (i == active) {
                    lvl = 93 + (uint8_t)(esp_timer_get_time() % 8);
                } else {
                    lvl = 75 + (uint8_t)((esp_timer_get_time() + i * 23) % 12);
                }
                s_levels[nrf_ch[i]] = lvl;
                if (s_levels[nrf_ch[i]] > 100) s_levels[nrf_ch[i]] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_B_WIFI: {
            for (int ch = 1; ch <= 12; ++ch) {
                uint8_t lvl = 88 + (uint8_t)((esp_timer_get_time() + ch * 11) % 13);
                s_levels[ch] = lvl;
                if (s_levels[ch] > 100) s_levels[ch] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_B_BLE: {
            static const uint8_t ble_ch[] = {2, 26, 80};
            for (int i = 0; i < 3; ++i) {
                s_levels[ble_ch[i]] = 93 + (uint8_t)((esp_timer_get_time() + i * 19) % 8);
                if (s_levels[ble_ch[i]] > 100) s_levels[ble_ch[i]] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_B_NRF: {
            static const uint8_t nrf_ch[] = {76, 78, 79};
            for (int i = 0; i < 3; ++i) {
                s_levels[nrf_ch[i]] = 91 + (uint8_t)((esp_timer_get_time() + i * 29) % 10);
                if (s_levels[nrf_ch[i]] > 100) s_levels[nrf_ch[i]] = 100;
            }
            break;
        }

        case DBG_SCENE_FW_B_MULTI: {
            for (int ch = 1; ch <= 12; ++ch) {
                s_levels[ch] = 82 + (uint8_t)((esp_timer_get_time() + ch * 7) % 12);
                if (s_levels[ch] > 100) s_levels[ch] = 100;
            }
            s_levels[2] = 93;
            s_levels[26] = 93;
            s_levels[80] = 93;
            s_levels[76] = 91;
            s_levels[78] = 91;
            s_levels[79] = 91;
            if (s_levels[2] > 100) s_levels[2] = 100;
            if (s_levels[26] > 100) s_levels[26] = 100;
            if (s_levels[80] > 100) s_levels[80] = 100;
            if (s_levels[76] > 100) s_levels[76] = 100;
            if (s_levels[78] > 100) s_levels[78] = 100;
            if (s_levels[79] > 100) s_levels[79] = 100;
            break;
        }

        default:
            break;
    }

    for (int ch = 0; ch < NRF24_CHANNEL_COUNT; ++ch) {
        if (s_levels[ch] > s_peaks[ch]) {
            s_peaks[ch] = s_levels[ch];
        }
    }
}
#endif

static void nrf24_toggle_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nrf24_toggle_pause();
}

static void nrf24_back_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nrf24_return_to_menu();
}

static void nrf24_handle_activate_selected(void) {
    if (s_selected_control == CONTROL_TOGGLE) {
        nrf24_toggle_pause();
    } else {
        nrf24_return_to_menu();
    }
}

static void nrf24_input_handler(InputEvent *event) {
    if (!event) {
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *touch = &event->data.touch_data;
        if (touch->state != LV_INDEV_STATE_REL) {
            return;
        }

        if (point_inside_obj(s_toggle_btn, touch->point.x, touch->point.y)) {
            s_selected_control = CONTROL_TOGGLE;
            nrf24_apply_control_selection();
            nrf24_toggle_pause();
            return;
        }

        if (point_inside_obj(s_back_btn, touch->point.x, touch->point.y)) {
            s_selected_control = CONTROL_BACK;
            nrf24_apply_control_selection();
            nrf24_return_to_menu();
            return;
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 0) {
            s_selected_control = CONTROL_TOGGLE;
            nrf24_apply_control_selection();
            return;
        }
        if (button == 1) {
            nrf24_handle_activate_selected();
            return;
        }
        if (button == 3) {
            s_selected_control = CONTROL_BACK;
            nrf24_apply_control_selection();
            return;
        }
        if (button == 2 || button == 4) {
            return;
        }
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == 29 || key == '`') {
            nrf24_return_to_menu();
            return;
        }
        if (key == LV_KEY_ENTER || key == 13) {
            nrf24_handle_activate_selected();
            return;
        }
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == 'h' || key == 'l' || key == ',' || key == '/') {
            s_selected_control = (s_selected_control == CONTROL_TOGGLE) ? CONTROL_BACK : CONTROL_TOGGLE;
            nrf24_apply_control_selection();
            return;
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            nrf24_handle_activate_selected();
            return;
        }
        if (event->data.encoder.direction != 0) {
            s_selected_control = (s_selected_control == CONTROL_TOGGLE) ? CONTROL_BACK : CONTROL_TOGGLE;
            nrf24_apply_control_selection();
        }
        return;
    }

#ifdef CONFIG_USE_ENCODER
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        nrf24_return_to_menu();
        return;
    }
#endif
}

void nrf24_analyzer_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));

    memset(s_levels, 0, sizeof(s_levels));
    memset(s_peaks, 0, sizeof(s_peaks));
    s_next_channel = 0;
    s_tick_count = 0;
    s_paused = false;
    s_remote_mode = nrf24_is_remote_mode();
    s_remote_stream_online = false;
    s_remote_error = false;
    s_hw_ready = false;
    s_selected_control = CONTROL_TOGGLE;
    s_jam_type = JAM_NONE;
    s_jam_confidence = 0;
    s_jam_strength = 0;
    s_jam_peak_ch = -1;
    s_jam_history_idx = 0;
    memset(s_jam_active_count_history, 0, sizeof(s_jam_active_count_history));
#ifdef NRF24_JAM_DETECT_DEBUG
    s_debug_scene = DBG_SCENE_IDLE;
    s_debug_active = false;
    s_debug_sweep_pos = 0;
#endif

    display_manager_fill_screen(bg);
    s_root = gui_screen_create_root(NULL, "NRF24", bg, LV_OPA_COVER);
    nrf24_analyzer_view.root = s_root;

    s_content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s_content, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, LV_PART_MAIN);

    s_status_label = lv_label_create(s_content);
    lv_label_set_text(s_status_label, "Initializing analyzer...");
    lv_obj_set_style_text_color(s_status_label, text, 0);
    lv_obj_set_style_text_font(s_status_label, accessibility_get_font_body(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 8, 6);

    s_freq_label = lv_label_create(s_content);
    lv_label_set_text(s_freq_label, "Peak CH:000  2.400 GHz  0%");
    lv_obj_set_style_text_color(s_freq_label, text, 0);
    lv_obj_set_style_text_font(s_freq_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_freq_label, LV_ALIGN_TOP_LEFT, 8, 24);

    int button_h = (LV_VER_RES <= 170) ? 28 : 34;
    int graph_top = 44;
    int graph_bottom_margin = button_h + 34;
    int graph_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT - graph_top - graph_bottom_margin;
    if (graph_h < 70) {
        graph_h = 70;
    }

    s_graph = lv_obj_create(s_content);
    lv_obj_set_pos(s_graph, 8, graph_top);
    lv_obj_set_size(s_graph, LV_HOR_RES - 16, graph_h);
    lv_obj_set_style_bg_color(s_graph, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_graph, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_graph, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_graph, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_graph, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_graph, nrf24_graph_draw_event, LV_EVENT_DRAW_MAIN, NULL);

    s_toggle_btn = lv_btn_create(s_content);
    lv_obj_set_size(s_toggle_btn, 92, button_h);
    lv_obj_align(s_toggle_btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_radius(s_toggle_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toggle_btn, lv_color_hex(theme_palette_get_surface_alt(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toggle_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toggle_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_toggle_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_toggle_btn, nrf24_toggle_btn_cb, LV_EVENT_CLICKED, NULL);

    s_toggle_label = lv_label_create(s_toggle_btn);
    lv_label_set_text(s_toggle_label, "Pause");
    lv_obj_set_style_text_color(s_toggle_label, text, 0);
    lv_obj_center(s_toggle_label);

    s_back_btn = lv_btn_create(s_content);
    lv_obj_set_size(s_back_btn, 92, button_h);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_radius(s_back_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(theme_palette_get_surface_alt(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_back_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_back_btn, nrf24_back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(s_back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_lbl, text, 0);
    lv_obj_center(back_lbl);

    nrf24_apply_control_selection();

    s_timer = lv_timer_create(nrf24_timer_cb, ANALYZER_TIMER_MS, NULL);

    if (s_remote_mode) {
        s_paused = false;
        if (esp_comm_manager_is_connected()) {
            if (!esp_comm_manager_send_command("nrf24", "start")) {
                s_remote_error = true;
            }
        }
        nrf24_update_pause_ui();
#ifdef NRF24_JAM_DETECT_DEBUG
    } else {
        if (s_status_label && lv_obj_is_valid(s_status_label)) {
            lv_label_set_text(s_status_label, "DBG: tap Next to cycle");
        }
        s_hw_ready = true;
        s_debug_active = true;
        s_debug_scene = DBG_SCENE_IDLE;
#elif defined(CONFIG_HAS_NRF24)
    } else if (nrf24_hw_start() == ESP_OK) {
        s_hw_ready = true;
        nrf24_update_pause_ui();
    } else {
        s_hw_ready = false;
        s_paused = true;
        nrf24_update_pause_ui();
#endif
    }
}

void nrf24_analyzer_destroy(void) {
    lvgl_timer_del_safe(&s_timer);
#ifdef CONFIG_HAS_NRF24
    nrf24_hw_stop();
#endif

    if (s_remote_mode && esp_comm_manager_is_connected()) {
        (void)esp_comm_manager_send_command("nrf24", "stop");
    }

    lvgl_obj_del_safe(&s_root);
    nrf24_analyzer_view.root = NULL;

    s_content = NULL;
    s_status_label = NULL;
    s_freq_label = NULL;
    s_graph = NULL;
    s_toggle_btn = NULL;
    s_back_btn = NULL;
    s_toggle_label = NULL;
    s_remote_stream_online = false;
    s_remote_error = false;
}

static void get_nrf24_analyzer_callback(void **callback) {
    if (callback) {
        *callback = nrf24_analyzer_view.input_callback;
    }
}

View nrf24_analyzer_view = {
    .root = NULL,
    .create = nrf24_analyzer_create,
    .destroy = nrf24_analyzer_destroy,
    .input_callback = nrf24_input_handler,
    .name = "NRF24 Analyzer",
    .get_hardwareinput_callback = get_nrf24_analyzer_callback
};

#endif
