#include "managers/settings_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "managers/display_manager.h"
#include "mbedtls/base64.h"  // For base64 decoding
#include "managers/rgb_manager.h"
#include <esp_log.h>
#include <string.h>
#include <time.h>
#include <nvs.h>

#define S_TAG "SETTINGS"

// NVS Keys
static const char *NVS_RGB_MODE_KEY = "rgb_mode";
static const char *NVS_CHANNEL_DELAY_KEY = "channel_delay";
static const char *NVS_BROADCAST_SPEED_KEY = "broadcast_speed";
static const char *NVS_AP_SSID_KEY = "ap_ssid";
static const char *NVS_AP_PASSWORD_KEY = "ap_password";
static const char *NVS_RGB_SPEED_KEY = "rgb_speed";
static const char *NVS_PORTAL_URL_KEY = "portal_url";
static const char *NVS_PORTAL_SSID_KEY = "portal_ssid";
static const char *NVS_PORTAL_PASSWORD_KEY = "portal_password";
static const char *NVS_PORTAL_AP_SSID_KEY = "portal_ap_ssid";
static const char *NVS_PORTAL_DOMAIN_KEY = "portal_domain";
static const char *NVS_PORTAL_OFFLINE_KEY = "portal_offline";
static const char *NVS_PRINTER_IP_KEY = "printer_ip";
static const char *NVS_PRINTER_TEXT_KEY = "printer_text";
static const char *NVS_PRINTER_FONT_SIZE_KEY = "pntr_ft_size";
static const char *NVS_PRINTER_ALIGNMENT_KEY = "pntr_alignment";
static const char *NVS_PRINTER_CONNECTED_KEY = "pntr_connected";
static const char *NVS_BOARD_TYPE_KEY = "board_type";
static const char *NVS_CUSTOM_PIN_CONFIG_KEY = "custom_pin_config";
static const char *NVS_FLAPPY_GHOST_NAME = "flap_name";
static const char *NVS_TIMEZONE_NAME = "sel_tz";
static const char *NVS_ACCENT_COLOR = "sel_ac";
static const char *NVS_GPS_RX_PIN = "gps_rx_pin";
static const char *NVS_DISPLAY_TIMEOUT_KEY = "disp_timeout";
static const char *NVS_ENABLE_RTS_KEY = "rts_enable";
static const char *NVS_STA_SSID_KEY = "sta_ssid";
static const char *NVS_STA_PASSWORD_KEY = "sta_password";
static const char *NVS_RGB_DATA_PIN_KEY = "rgb_data_pin";
static const char *NVS_RGB_RED_PIN_KEY = "rgb_red_pin";
static const char *NVS_RGB_GREEN_PIN_KEY = "rgb_green_pin";
static const char *NVS_RGB_BLUE_PIN_KEY = "rgb_blue_pin";
static const char *NVS_THIRD_CTRL_KEY = "third_ctrl";
static const char *NVS_MENU_THEME_KEY = "menu_theme";
static const char *NVS_TERMINAL_TEXT_COLOR_KEY = "term_color";
static const char *NVS_INVERT_COLORS_KEY = "invert_colors";
static const char *NVS_INFRARED_EASY_MODE_KEY = "ir_easy_mode";
static const char *NVS_WEB_AUTH_KEY = "web_auth";
static const char *NVS_WEBUI_AP_ONLY_KEY = "webui_ap";
static const char *NVS_ESP_COMM_TX_PIN_KEY = "esp_comm_tx";
static const char *NVS_ESP_COMM_RX_PIN_KEY = "esp_comm_rx";
static const char *NVS_AP_ENABLED_KEY = "ap_enabled";
static const char *NVS_POWER_SAVE_KEY = "power_save";
static const char *NVS_ZEBRA_MENUS_KEY = "zebra_menus";
static const char *NVS_MAX_SCREEN_BRIGHTNESS_KEY = "max_bright";
static const char *NVS_NAV_BUTTONS_KEY = "nav_buttons";
static const char *NVS_MENU_LAYOUT_KEY = "menu_layout";
static const char *NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY = "neopixel_bright";
static const char *NVS_RGB_LED_COUNT_KEY = "rgb_led_cnt";
static const char *NVS_ENCODER_INVERT_KEY = "enc_inv";
static const char *NVS_AUTO_SAVE_SCANS_KEY = "auto_save_sc";
static const char *NVS_SETUP_COMPLETE_KEY = "setup_done";
static const char *NVS_WIFI_COUNTRY_KEY = "wifi_country";
static const char *NVS_WIGLE_API_KEY = "wigle_api_key";
static const char *NVS_WIGLE_DONATE_KEY = "wigle_donate";
static const char *NVS_WIGLE_AUTO_UPLOAD_KEY = "wigle_auto_ul";
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *NVS_STATUS_IDLE_ANIM_KEY = "idle_anim"; // nvs keys must be <=15 chars
static const char *NVS_STATUS_IDLE_TIMEOUT_KEY = "idle_to_ms";
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
static const char *NVS_BADUSB_VID_KEY = "bu_vid";
static const char *NVS_BADUSB_PID_KEY = "bu_pid";
static const char *NVS_BADUSB_MFR_KEY = "bu_mfr";
static const char *NVS_BADUSB_PROD_KEY = "bu_prod";
static const char *NVS_BADUSB_RAND_KEY = "bu_rand";
static const char *NVS_BADUSB_KB_KEY = "bu_kb_layout";
#endif
static const char *NVS_IO_BTN_P10_CMD_KEY = "io_btn_p10";
static const char *NVS_IO_BTN_P11_CMD_KEY = "io_btn_p11";
static const char *NVS_IO_BTN_P12_CMD_KEY = "io_btn_p12";

// MIC RGB Visualizer NVS keys
static const char *NVS_MIC_VISUALIZER_MODE_KEY = "mic_vis_mode";
static const char *NVS_MIC_COLOR_MODE_KEY = "mic_color";
static const char *NVS_MIC_SENSITIVITY_KEY = "mic_sens";
static const char *NVS_MIC_SMOOTHING_KEY = "mic_smooth";
static const char *NVS_MIC_CONTRAST_KEY = "mic_contrast";
static const char *NVS_MIC_MIRROR_MODE_KEY = "mic_mirror";

static const char *NVS_GHOSTLINK_SPLIT_VIEW_KEY = "glink_split";
static const char *NVS_MENU_BG_SHADE_KEY = "menu_bg_shd";
static const char *NVS_MENU_ROUNDED_KEY = "menu_rounded";
static const char *NVS_MENU_ITEM_BORDERS_KEY = "menu_itm_brd";

static const char *TAG = "SettingsManager";

static nvs_handle_t nvsHandle;
FSettings G_Settings;

void settings_init(FSettings *settings) {
  settings_set_defaults(settings);
  esp_err_t err = nvs_flash_init();

  if (err == ESP_ERR_NVS_NO_FREE_PAGES || 
      err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
      err == ESP_ERR_NVS_NOT_FOUND) {
      printf("NVS corrupt - erasing partition...");
      esp_err_t erase_err = nvs_flash_erase();
      if (erase_err != ESP_OK) {
          printf("Erase failed: %s", esp_err_to_name(erase_err));
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart(); // Hard reset if erase fails
      }
      err = nvs_flash_init();
  }

  if (err != ESP_OK) {
      printf("NVS FATAL: %s - Rebooting", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
  }

  err = nvs_open("storage", NVS_READWRITE, &nvsHandle);
  if (err == ESP_OK) {
    settings_load(settings);
    printf("Settings loaded successfully.\n");
    settings_print_nvs_stats();
  } else {
    printf("Failed to open NVS handle: %s\n", esp_err_to_name(err));
  }
}

void settings_deinit(void) { nvs_close(nvsHandle); }

void settings_set_defaults(FSettings *settings) {
  settings->rgb_mode = RGB_MODE_NORMAL;
  settings->channel_delay = 1.0f;
  settings->broadcast_speed = 5;
  // default to the 'Bright' palette (index 3)
  settings->menu_theme = 3;
  strcpy(settings->ap_ssid, "GhostNet");
  strcpy(settings->ap_password, "GhostNet");
  settings->rgb_speed = 15;

  // Evil Portal defaults
  strcpy(settings->portal_url, "/default/path");
  strcpy(settings->portal_ssid, "EvilPortal");
  strcpy(settings->portal_password, "");
  strcpy(settings->portal_ap_ssid, "EvilAP");
  strcpy(settings->portal_domain, "portal.local");
  settings->portal_offline_mode = false;

  // Power Printer defaults
  strcpy(settings->printer_ip, "192.168.1.100");
  strcpy(settings->printer_text, "Default Text");
  settings->printer_font_size = 12;
  settings->printer_alignment = ALIGNMENT_CM;
  strcpy(settings->flappy_ghost_name, "Bob");
  strcpy(settings->selected_hex_accent_color, "#ffffff");
  strcpy(settings->selected_timezone, "MST7MDT,M3.2.0,M11.1.0");
  settings->gps_rx_pin = 0;
  settings->display_timeout_ms = 30000; // Default to 30 seconds
  settings->rts_enabled = false;
  strcpy(settings->sta_ssid, ""); // Default empty station SSID
  strcpy(settings->sta_password, ""); // Default empty station password
  settings->rgb_data_pin = -1;
  settings->rgb_red_pin = -1;
  settings->rgb_green_pin = -1;
  settings->rgb_blue_pin = -1;
  settings->third_control_enabled = false;
  settings->terminal_text_color = 0x00FF00;
  settings->invert_colors = false;
  settings->web_auth_enabled = false;
  settings->webui_restrict_to_ap = true;
#ifdef CONFIG_IDF_TARGET_ESP32
  settings->esp_comm_tx_pin = 17;
  settings->esp_comm_rx_pin = 16;
#else
  settings->esp_comm_tx_pin = 6;
  settings->esp_comm_rx_pin = 7;
#endif
  settings->ap_enabled = true; // Default to enabled
  settings->power_save_enabled = false;
  settings->zebra_menus_enabled = false; // or true if you want it enabled by default
  settings->max_screen_brightness = 100; // Default to 100% brightness
  settings->infrared_easy_mode = false; // Default to disabled
  settings->nav_buttons_enabled = true; // Default to enabled
  settings->menu_layout = 0; // Default to carousel layout
  settings->neopixel_max_brightness = 100; // Default to 100% brightness
  settings->encoder_invert_direction = false;
  settings->rgb_led_count = CONFIG_NUM_LEDS;
  settings->auto_save_scans = true;
  settings->setup_complete = false;
  settings->wifi_country = 0;
  strcpy(settings->wigle_api_key, "");
  settings->wigle_auto_upload = false; // Default to off
  settings->wigle_donate = true; // Default to donating
  settings->io_btn_p10_cmd[0] = '\0';
  settings->io_btn_p11_cmd[0] = '\0';
  settings->io_btn_p12_cmd[0] = '\0';
  // MIC RGB Visualizer defaults
  settings->mic_visualizer_mode = MIC_MODE_4BAND_SPECTRUM;
  settings->mic_color_mode = MIC_COLOR_RAINBOW;
  settings->mic_sensitivity = 50; // 50% default
  settings->mic_smoothing = 30; // 30% default
  settings->mic_contrast = 2; // Medium contrast
  settings->mic_mirror_mode = false;
  settings->ghostlink_split_view = true; // Default to split view
  settings->menu_bg_shade = 2;
  settings->menu_rounded = true;
  settings->menu_item_borders = false;
#ifdef CONFIG_WITH_STATUS_DISPLAY
  settings->status_idle_animation = IDLE_ANIM_GAME_OF_LIFE;
  settings->status_idle_timeout_ms = 5000; // default 5s
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  settings->badusb_vid = 0x1209;
  settings->badusb_pid = 0x0001;
  strcpy(settings->badusb_manufacturer, "USB Device");
  strcpy(settings->badusb_product, "HID Keyboard");
  settings->badusb_randomize = false;
  settings->badusb_kb_layout = KB_LAYOUT_US;
#endif
}

void settings_load(FSettings *settings) {
  esp_err_t err;
  uint8_t value_u8;
  uint16_t value_u16;
  uint32_t value_u32;
  float value_float;
  size_t str_size;

  // Load RGB Mode
  err = nvs_get_u8(nvsHandle, NVS_RGB_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->rgb_mode = (RGBMode)value_u8;
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(S_TAG, "Using default RGB mode");
  }

  size_t required_size = sizeof(value_float); // Set the size of the buffer
  err = nvs_get_blob(nvsHandle, NVS_CHANNEL_DELAY_KEY, &value_float,
                     &required_size);
  if (err == ESP_OK) {
    settings->channel_delay = value_float;
  } else {
    printf("Failed to load Channel Delay: %s\n", esp_err_to_name(err));
  }

  // Load Broadcast Speed
  err = nvs_get_u16(nvsHandle, NVS_BROADCAST_SPEED_KEY, &value_u16);
  if (err == ESP_OK) {
    settings->broadcast_speed = value_u16;
  }

  // Load AP SSID
  str_size = sizeof(settings->ap_ssid);
  err = nvs_get_str(nvsHandle, NVS_AP_SSID_KEY, settings->ap_ssid, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load AP SSID\n");
  }

  // Load AP Password
  str_size = sizeof(settings->ap_password);
  err = nvs_get_str(nvsHandle, NVS_AP_PASSWORD_KEY, settings->ap_password,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load AP Password\n");
  }

  // Load RGB Speed
  err = nvs_get_u8(nvsHandle, NVS_RGB_SPEED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->rgb_speed = value_u8;
  }

  // Load RGB LED Count
  err = nvs_get_u16(nvsHandle, NVS_RGB_LED_COUNT_KEY, &value_u16);
  if (err == ESP_OK && value_u16 != 0) {
    settings->rgb_led_count = value_u16;
  }

  // Load Evil Portal settings
  str_size = sizeof(settings->portal_url);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_URL_KEY, settings->portal_url,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal URL\n");
  }

  str_size = sizeof(settings->portal_ssid);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_SSID_KEY, settings->portal_ssid,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal SSID\n");
  }

  str_size = sizeof(settings->portal_password);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_PASSWORD_KEY,
                    settings->portal_password, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal Password\n");
  }

  str_size = sizeof(settings->portal_ap_ssid);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_AP_SSID_KEY, settings->portal_ap_ssid,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal AP SSID\n");
  }

  str_size = sizeof(settings->portal_domain);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_DOMAIN_KEY, settings->portal_domain,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal Domain\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_PORTAL_OFFLINE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->portal_offline_mode = value_u8;
  }

  // Load Power Printer settings
  str_size = sizeof(settings->printer_ip);
  err = nvs_get_str(nvsHandle, NVS_PRINTER_IP_KEY, settings->printer_ip,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Printer IP\n");
  }

  str_size = sizeof(settings->printer_text);
  err = nvs_get_str(nvsHandle, NVS_PRINTER_TEXT_KEY, settings->printer_text,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Printer Text\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_PRINTER_FONT_SIZE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->printer_font_size = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_PRINTER_ALIGNMENT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->printer_alignment = (PrinterAlignment)value_u8;
  }

  str_size = sizeof(settings->flappy_ghost_name);
  err = nvs_get_str(nvsHandle, NVS_FLAPPY_GHOST_NAME,
                    settings->flappy_ghost_name, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Flappy Ghost Name\n");
  }

  str_size = sizeof(settings->selected_timezone);
  err = nvs_get_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Timezone String\n");
  }

  str_size = sizeof(settings->selected_hex_accent_color);
  err = nvs_get_str(nvsHandle, NVS_ACCENT_COLOR,
                    settings->selected_hex_accent_color, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Hex Accent Color String\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_GPS_RX_PIN, &value_u8);
  if (err == ESP_OK) {
    settings->gps_rx_pin = value_u8;
  }

  uint32_t timeout_value;
  err = nvs_get_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, &timeout_value);
  if (err == ESP_OK) {
    settings->display_timeout_ms = timeout_value;
  } else {
    settings->display_timeout_ms = 30000; // Default to 30 seconds if not found
  }

  uint8_t rtsenabledvalue;
  err = nvs_get_u8(nvsHandle, NVS_ENABLE_RTS_KEY, &rtsenabledvalue);
  if (err == ESP_OK) {
    settings->rts_enabled = rtsenabledvalue;
  } else {
    settings->rts_enabled = false;
  }

  uint8_t thirdenabledvalue;
  err = nvs_get_u8(nvsHandle, NVS_THIRD_CTRL_KEY, &thirdenabledvalue);
  if (err == ESP_OK) {
    settings->third_control_enabled = thirdenabledvalue;
  } else {
    settings->third_control_enabled = false;
  }

  // Load Station SSID
  str_size = sizeof(settings->sta_ssid);
  err = nvs_get_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load STA SSID: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->sta_ssid, ""); // Ensure it's empty if not found
  }

  // Load Station Password
  str_size = sizeof(settings->sta_password);
  err = nvs_get_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load STA Password: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->sta_password, ""); // Ensure it's empty if not found
  }

  // Load Wigle API key (format: APIName:APIToken)
  str_size = sizeof(settings->wigle_api_key);
  err = nvs_get_str(nvsHandle, NVS_WIGLE_API_KEY, settings->wigle_api_key, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle API key: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->wigle_api_key, "");
  }

  // Load Wigle donate setting
  uint8_t donate_val = 1;
  err = nvs_get_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, &donate_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle donate setting: %s\n", esp_err_to_name(err));
  }
  settings->wigle_donate = (donate_val != 0);

  // Load Wigle auto-upload setting
  uint8_t auto_upload_val = 0;
  err = nvs_get_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, &auto_upload_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle auto-upload setting: %s\n", esp_err_to_name(err));
  }
  settings->wigle_auto_upload = (auto_upload_val != 0);

  str_size = sizeof(settings->io_btn_p10_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P10_CMD_KEY, settings->io_btn_p10_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p10_cmd[0] = '\0';
  str_size = sizeof(settings->io_btn_p11_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P11_CMD_KEY, settings->io_btn_p11_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p11_cmd[0] = '\0';
  str_size = sizeof(settings->io_btn_p12_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P12_CMD_KEY, settings->io_btn_p12_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p12_cmd[0] = '\0';

  printf("Settings loaded from NVS.\n");
  int32_t tmp;
  err = nvs_get_i32(nvsHandle, NVS_RGB_DATA_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_data_pin = tmp;
  } else {
    settings->rgb_data_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_RED_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_red_pin = tmp;
  } else {
    settings->rgb_red_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_GREEN_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_green_pin = tmp;
  } else {
    settings->rgb_green_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_BLUE_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_blue_pin = tmp;
  } else {
    settings->rgb_blue_pin = -1;
  }

  err = nvs_get_u8(nvsHandle, NVS_MENU_THEME_KEY, &value_u8);
  if (err == ESP_OK) settings->menu_theme = value_u8;
  err = nvs_get_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, &value_u32);
  if (err == ESP_OK) {
    settings->terminal_text_color = value_u32;
  }
  err = nvs_get_u8(nvsHandle, NVS_INVERT_COLORS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->invert_colors = (value_u8 != 0);
  }

  err = nvs_get_u8(nvsHandle, NVS_WEB_AUTH_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->web_auth_enabled = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->webui_restrict_to_ap = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_AP_ENABLED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->ap_enabled = (value_u8 != 0);
  } else {
    settings->ap_enabled = true; // Default to enabled if not found
  }

  err = nvs_get_u8(nvsHandle, NVS_POWER_SAVE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->power_save_enabled = (value_u8 != 0);
  } else {
    settings->power_save_enabled = false; // Default to disabled if not found
  }

  err = nvs_get_i32(nvsHandle, NVS_ESP_COMM_TX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->esp_comm_tx_pin = tmp;
  } else {
#ifdef CONFIG_IDF_TARGET_ESP32
    settings->esp_comm_tx_pin = 17;
#else
    settings->esp_comm_tx_pin = 6;
#endif
  }

  err = nvs_get_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->esp_comm_rx_pin = tmp;
  } else {
#ifdef CONFIG_IDF_TARGET_ESP32
    settings->esp_comm_rx_pin = 16;
#else
    settings->esp_comm_rx_pin = 7;
#endif
  }

  err = nvs_get_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->zebra_menus_enabled = (value_u8 != 0);
  } else {
    settings->zebra_menus_enabled = false;
  } // Default to disabled if not found
  // Load Max Screen Brightness
  err = nvs_get_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->max_screen_brightness = value_u8;
  } else {
    settings->max_screen_brightness = 100; // Default to 100% if not found
  }

  // Load Infrared Easy Mode
  err = nvs_get_u8(nvsHandle, NVS_INFRARED_EASY_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->infrared_easy_mode = (bool)value_u8;
  } else {
    settings->infrared_easy_mode = false; // Default to disabled if not found
  }

  // Load Navigation Buttons Enabled
  err = nvs_get_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->nav_buttons_enabled = (bool)value_u8;
  } else {
    settings->nav_buttons_enabled = true; // Default to enabled if not found
  }

  // Load Auto Save Scans
  err = nvs_get_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->auto_save_scans = (bool)value_u8;
  } else {
    settings->auto_save_scans = true; // Default to enabled if not found
  }

  // Load Menu Layout
  err = nvs_get_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_layout = value_u8;
  } else {
    settings->menu_layout = 0; // Default to carousel layout if not found
  }

  // Load Neopixel Max Brightness
  err = nvs_get_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->neopixel_max_brightness = value_u8;
  } else {
    settings->neopixel_max_brightness = 100; // Default to 100% if not found
  }

  // Load encoder direction inversion
  err = nvs_get_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->encoder_invert_direction = (bool)value_u8;
  } else {
    settings->encoder_invert_direction = false;
  }

  err = nvs_get_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->setup_complete = (bool)value_u8;
  } else {
    settings->setup_complete = false;
  }

  err = nvs_get_u8(nvsHandle, NVS_WIFI_COUNTRY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->wifi_country = value_u8;
  } else {
    settings->wifi_country = 0;
  }

#ifdef CONFIG_WITH_STATUS_DISPLAY
  err = nvs_get_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->status_idle_animation = (IdleAnimation)value_u8;
  } else {
    // try legacy key that exceeded length (for migration, if it ever existed)
    const char *legacy_key = "status_idle_anim";
    esp_err_t err2 = nvs_get_u8(nvsHandle, legacy_key, &value_u8);
    if (err2 == ESP_OK) {
      settings->status_idle_animation = (IdleAnimation)value_u8;
      // re-save under the new shorter key
      nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, value_u8);
    } else {
      settings->status_idle_animation = IDLE_ANIM_GAME_OF_LIFE;
    }
  }
  // load idle timeout
  err = nvs_get_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, &value_u32);
  if (err == ESP_OK) {
    settings->status_idle_timeout_ms = value_u32;
  } else {
    settings->status_idle_timeout_ms = 5000; // default 5s
  }
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  err = nvs_get_u16(nvsHandle, NVS_BADUSB_VID_KEY, &value_u16);
  if (err == ESP_OK) settings->badusb_vid = value_u16;

  err = nvs_get_u16(nvsHandle, NVS_BADUSB_PID_KEY, &value_u16);
  if (err == ESP_OK) settings->badusb_pid = value_u16;

  str_size = sizeof(settings->badusb_manufacturer);
  err = nvs_get_str(nvsHandle, NVS_BADUSB_MFR_KEY, settings->badusb_manufacturer, &str_size);

  str_size = sizeof(settings->badusb_product);
  err = nvs_get_str(nvsHandle, NVS_BADUSB_PROD_KEY, settings->badusb_product, &str_size);

  err = nvs_get_u8(nvsHandle, NVS_BADUSB_RAND_KEY, &value_u8);
  if (err == ESP_OK) settings->badusb_randomize = (bool)value_u8;

  err = nvs_get_u8(nvsHandle, NVS_BADUSB_KB_KEY, &value_u8);
  if (err == ESP_OK) settings->badusb_kb_layout = value_u8;
#endif

  // Load MIC RGB Visualizer settings
  err = nvs_get_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_visualizer_mode = (MicVisualizerMode)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_color_mode = (MicColorMode)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_sensitivity = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_smoothing = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_contrast = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_mirror_mode = (bool)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->ghostlink_split_view = (bool)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_bg_shade = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_rounded = (bool)value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_item_borders = (bool)value_u8;
  }
}

static void update_rainbow_effect(const FSettings *settings) {
#ifndef CONFIG_WITH_SCREEN
  return;
#endif

  if (settings_get_rgb_mode(settings) == RGB_MODE_RAINBOW) {
    display_manager_set_rainbow_mode(true);
  } else {
    display_manager_set_rainbow_mode(false);
  }
}


void settings_restart_rgb_effect(void) {
    ESP_LOGI(TAG, "Restarting RGB effect...");
    
    // 1. Signal any existing task to stop
    // 1. Signal any existing task to stop
    if (rgb_effect_task_handle != NULL) {
        rgb_manager_signal_rainbow_exit();
        vTaskDelay(pdMS_TO_TICKS(50));

        rgb_effect_task_handle = NULL;
    }
    
    // Force cleanup of status bar rainbow effect if we are switching away from RAINBOW
    // Use the *new* value directly from G_Settings to be sure.
    if (settings_get_rgb_mode(&G_Settings) != RGB_MODE_RAINBOW) {
        // We call update_rainbow_effect to ensure the timer is deleted
        update_rainbow_effect(&G_Settings);
        // And explicitly force the status bar color update just in case
        display_manager_update_status_bar_color();
    }

    // 4. Start new task based on mode
    RGBMode mode = settings_get_rgb_mode(&G_Settings);
    if (mode == RGB_MODE_RAINBOW) {
#if RGB_EFFECT_USE_PINNED_API
        xTaskCreatePinnedToCore(rainbow_task, "Rainbow Task", 3072, &rgb_manager,
                                RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle,
                                RGB_EFFECT_TASK_CORE);
#else
        xTaskCreate(rainbow_task, "Rainbow Task", 3072, &rgb_manager,
                    RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
#endif
    } else if (mode == RGB_MODE_KNIGHT_RIDER) {
         xTaskCreate(knightrider_task, "Knight Rider Task", 3072, &rgb_manager,
                    RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
    } else if (mode == RGB_MODE_STEALTH) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    } else if (mode == RGB_MODE_RED) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    } else if (mode == RGB_MODE_GREEN) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 255, 0, false);
    } else if (mode == RGB_MODE_BLUE) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 255, false);
    } else if (mode == RGB_MODE_YELLOW) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 255, 0, false);
    } else if (mode == RGB_MODE_PURPLE) {
        rgb_manager_set_color(&rgb_manager, -1, 115, 0, 225, false);
    } else if (mode == RGB_MODE_CYAN) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 255, 255, false);
    } else if (mode == RGB_MODE_ORANGE) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 165, 0, false);
    } else if (mode == RGB_MODE_WHITE) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 255, 255, false);
    } else if (mode == RGB_MODE_PINK) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 192, 203, false);
    } else if (mode == RGB_MODE_MIC_VISUALIZER) {
        // MIC visualizer mode - LEDs are controlled by GhostLink stream
        // Just clear LEDs here, the stream handler will take over
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        ESP_LOGI(TAG, "RGB Mode: MIC Visualizer (controlled via GhostLink)");
    } else {
        // Normal mode
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    }

    update_rainbow_effect(&G_Settings); // Start/stop global timer for status bar if needed
}

void settings_persist_setting(SettingsType setting) {
    esp_err_t err = ESP_OK;
    const char *key = NULL;

    switch (setting) {
        case SETTING_RGB_MODE:
            err = nvs_set_u8(nvsHandle, NVS_RGB_MODE_KEY, (uint8_t)G_Settings.rgb_mode);
            key = NVS_RGB_MODE_KEY;
            break;
        case SETTING_DISPLAY_TIMEOUT:
            err = nvs_set_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, G_Settings.display_timeout_ms);
            key = NVS_DISPLAY_TIMEOUT_KEY;
            break;
        case SETTING_MENU_THEME:
            err = nvs_set_u8(nvsHandle, NVS_MENU_THEME_KEY, G_Settings.menu_theme);
            key = NVS_MENU_THEME_KEY;
            break;
        case SETTING_THIRD_CONTROL:
            err = nvs_set_u8(nvsHandle, NVS_THIRD_CTRL_KEY, G_Settings.third_control_enabled);
            key = NVS_THIRD_CTRL_KEY;
            break;
        case SETTING_TERMINAL_COLOR:
            err = nvs_set_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, G_Settings.terminal_text_color);
            key = NVS_TERMINAL_TEXT_COLOR_KEY;
            break;
        case SETTING_INVERT_COLORS:
            err = nvs_set_u8(nvsHandle, NVS_INVERT_COLORS_KEY, G_Settings.invert_colors);
            key = NVS_INVERT_COLORS_KEY;
            break;
        case SETTING_WEB_AUTH:
            err = nvs_set_u8(nvsHandle, NVS_WEB_AUTH_KEY, G_Settings.web_auth_enabled);
            key = NVS_WEB_AUTH_KEY;
            break;
        case SETTING_WEBUI_AP_ONLY:
            err = nvs_set_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, G_Settings.webui_restrict_to_ap);
            key = NVS_WEBUI_AP_ONLY_KEY;
            break;
        case SETTING_AP_ENABLED:
            err = nvs_set_u8(nvsHandle, NVS_AP_ENABLED_KEY, G_Settings.ap_enabled);
            key = NVS_AP_ENABLED_KEY;
            break;
        case SETTING_POWER_SAVE:
            err = nvs_set_u8(nvsHandle, NVS_POWER_SAVE_KEY, G_Settings.power_save_enabled);
            key = NVS_POWER_SAVE_KEY;
            break;
        case SETTING_MAX_BRIGHTNESS:
            err = nvs_set_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, G_Settings.max_screen_brightness);
            key = NVS_MAX_SCREEN_BRIGHTNESS_KEY;
            break;
        case SETTING_NEOPIXEL_BRIGHTNESS:
            err = nvs_set_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, G_Settings.neopixel_max_brightness);
            key = NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY;
            break;
        case SETTING_ZEBRA_MENUS:
            err = nvs_set_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, G_Settings.zebra_menus_enabled);
            key = NVS_ZEBRA_MENUS_KEY;
            break;
        case SETTING_NAV_BUTTONS:
            err = nvs_set_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, G_Settings.nav_buttons_enabled);
            key = NVS_NAV_BUTTONS_KEY;
            break;
        case SETTING_AUTO_SAVE_SCANS:
            err = nvs_set_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, G_Settings.auto_save_scans);
            key = NVS_AUTO_SAVE_SCANS_KEY;
            break;
        case SETTING_MENU_LAYOUT:
            err = nvs_set_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, G_Settings.menu_layout);
            key = NVS_MENU_LAYOUT_KEY;
            break;
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIMATION:
            err = nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, (uint8_t)G_Settings.status_idle_animation);
            key = NVS_STATUS_IDLE_ANIM_KEY;
            break;
        case SETTING_IDLE_ANIM_DELAY:
            err = nvs_set_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, G_Settings.status_idle_timeout_ms);
            key = NVS_STATUS_IDLE_TIMEOUT_KEY;
            break;
#endif
#ifdef CONFIG_USE_ENCODER
        case SETTING_ENCODER_INVERT:
            err = nvs_set_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, G_Settings.encoder_invert_direction);
            key = NVS_ENCODER_INVERT_KEY;
            break;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
        case SETTING_USB_HOST_MODE:
             break;
#endif
        case SETTING_RUN_SETUP_WIZARD:
        case SETTING_I2C_SCAN:
        case SETTING_WIGLE_TEST_API:
        case SETTING_WIGLE_HELP:
        case SETTING_WIGLE_MANUAL_UPLOAD:
        case SETTING_WIGLE_STATS:
        case SETTING_LOAD_CONFIG:
        case SETTING_EXPORT_SETTINGS_SD:
        case SETTING_IMPORT_SETTINGS_SD:
        case SETTING_FACTORY_RESET:
             // Actions, not saved
             return;
        case SETTING_SETUP_COMPLETE:
            err = nvs_set_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, G_Settings.setup_complete ? 1 : 0);
            key = NVS_SETUP_COMPLETE_KEY;
            break;
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
        case SETTING_BADUSB_VID:
            err = nvs_set_u16(nvsHandle, NVS_BADUSB_VID_KEY, G_Settings.badusb_vid);
            key = NVS_BADUSB_VID_KEY;
            break;
        case SETTING_BADUSB_PID:
            err = nvs_set_u16(nvsHandle, NVS_BADUSB_PID_KEY, G_Settings.badusb_pid);
            key = NVS_BADUSB_PID_KEY;
            break;
        case SETTING_BADUSB_MANUFACTURER:
            err = nvs_set_str(nvsHandle, NVS_BADUSB_MFR_KEY, G_Settings.badusb_manufacturer);
            key = NVS_BADUSB_MFR_KEY;
            break;
        case SETTING_BADUSB_PRODUCT:
            err = nvs_set_str(nvsHandle, NVS_BADUSB_PROD_KEY, G_Settings.badusb_product);
            key = NVS_BADUSB_PROD_KEY;
            break;
        case SETTING_BADUSB_RANDOMIZE:
            err = nvs_set_u8(nvsHandle, NVS_BADUSB_RAND_KEY, G_Settings.badusb_randomize ? 1 : 0);
            key = NVS_BADUSB_RAND_KEY;
            break;
        case SETTING_BADUSB_KB_LAYOUT:
            err = nvs_set_u8(nvsHandle, NVS_BADUSB_KB_KEY, G_Settings.badusb_kb_layout);
            key = NVS_BADUSB_KB_KEY;
            break;
#endif
        case SETTING_WIGLE_API_KEY:
            err = nvs_set_str(nvsHandle, NVS_WIGLE_API_KEY, G_Settings.wigle_api_key);
            key = NVS_WIGLE_API_KEY;
            break;
        case SETTING_WIGLE_AUTO_UPLOAD:
            err = nvs_set_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, G_Settings.wigle_auto_upload ? 1 : 0);
            key = NVS_WIGLE_AUTO_UPLOAD_KEY;
            break;
        case SETTING_WIGLE_DONATE:
            err = nvs_set_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, G_Settings.wigle_donate ? 1 : 0);
            key = NVS_WIGLE_DONATE_KEY;
            break;
        case SETTING_MIC_VISUALIZER_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, (uint8_t)G_Settings.mic_visualizer_mode);
            key = NVS_MIC_VISUALIZER_MODE_KEY;
            break;
        case SETTING_MIC_COLOR_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, (uint8_t)G_Settings.mic_color_mode);
            key = NVS_MIC_COLOR_MODE_KEY;
            break;
        case SETTING_MIC_SENSITIVITY:
            err = nvs_set_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, G_Settings.mic_sensitivity);
            key = NVS_MIC_SENSITIVITY_KEY;
            break;
        case SETTING_MIC_SMOOTHING:
            err = nvs_set_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, G_Settings.mic_smoothing);
            key = NVS_MIC_SMOOTHING_KEY;
            break;
        case SETTING_MIC_CONTRAST:
            err = nvs_set_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, G_Settings.mic_contrast);
            key = NVS_MIC_CONTRAST_KEY;
            break;
        case SETTING_MIC_MIRROR_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, G_Settings.mic_mirror_mode ? 1 : 0);
            key = NVS_MIC_MIRROR_MODE_KEY;
            break;
        case SETTING_MIC_CALIBRATE:
            // Action only, not persisted
            return;
        case SETTING_GHOSTLINK_SPLIT_VIEW:
            err = nvs_set_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, G_Settings.ghostlink_split_view ? 1 : 0);
            key = NVS_GHOSTLINK_SPLIT_VIEW_KEY;
            break;
        case SETTING_MENU_BG_SHADE:
            err = nvs_set_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, G_Settings.menu_bg_shade);
            key = NVS_MENU_BG_SHADE_KEY;
            break;
        case SETTING_MENU_ROUNDED:
            err = nvs_set_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, G_Settings.menu_rounded ? 1 : 0);
            key = NVS_MENU_ROUNDED_KEY;
            break;
        case SETTING_MENU_ITEM_BORDERS:
            err = nvs_set_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, G_Settings.menu_item_borders ? 1 : 0);
            key = NVS_MENU_ITEM_BORDERS_KEY;
            break;
        default:
            ESP_LOGW(TAG, "Unknown setting type to persist: %d", setting);
            return;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NVS val for %s: %s", key ? key : "unknown", esp_err_to_name(err));
    } else if (key) {
        err = nvs_commit(nvsHandle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS for %s: %s", key, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Persisted setting %s", key);
        }
    }
}

// Core Settings Getters and Setters
void settings_set_rgb_mode(FSettings *settings, RGBMode mode) {
  settings->rgb_mode = mode;
}

void settings_set_rts_enabled(FSettings *settings, bool enabled) {
  settings->rts_enabled = enabled;
}

bool settings_get_rts_enabled(const FSettings *settings) {
  return settings->rts_enabled;
}

RGBMode settings_get_rgb_mode(const FSettings *settings) {
  return settings->rgb_mode;
}

void settings_set_channel_delay(FSettings *settings, float delay_ms) {
  settings->channel_delay = delay_ms;
}

float settings_get_channel_delay(const FSettings *settings) {
  return settings->channel_delay;
}

void settings_set_broadcast_speed(FSettings *settings, uint16_t speed) {
  settings->broadcast_speed = speed;
}

uint16_t settings_get_broadcast_speed(const FSettings *settings) {
  return settings->broadcast_speed;
}

void settings_set_flappy_ghost_name(FSettings *settings, const char *Name) {
  strncpy(settings->flappy_ghost_name, Name,
          sizeof(settings->flappy_ghost_name) - 1);
  settings->flappy_ghost_name[sizeof(settings->flappy_ghost_name) - 1] = '\0';
}

const char *settings_get_flappy_ghost_name(const FSettings *settings) {
  return settings->flappy_ghost_name;
}

void settings_set_timezone_str(FSettings *settings, const char *Name) {
  strncpy(settings->selected_timezone, Name,
          sizeof(settings->selected_timezone) - 1);
  settings->selected_timezone[sizeof(settings->selected_timezone) - 1] = '\0';
}

const char *settings_get_timezone_str(const FSettings *settings) {
  return settings->selected_timezone;
}

void settings_set_accent_color_str(FSettings *settings, const char *Name) {
  strncpy(settings->selected_hex_accent_color, Name,
          sizeof(settings->selected_hex_accent_color) - 1);
  settings->selected_hex_accent_color[sizeof(settings->selected_hex_accent_color) - 1] = '\0';
}

const char *settings_get_accent_color_str(const FSettings *settings) {
  return settings->selected_hex_accent_color;
}

void settings_save(const FSettings *settings) {
    if (!settings) return;

    nvs_set_u8(nvsHandle, NVS_RGB_MODE_KEY, (uint8_t)settings->rgb_mode);
    float ch_delay = settings->channel_delay;
    nvs_set_blob(nvsHandle, NVS_CHANNEL_DELAY_KEY, &ch_delay, sizeof(ch_delay));
    nvs_set_u16(nvsHandle, NVS_BROADCAST_SPEED_KEY, settings->broadcast_speed);
    nvs_set_str(nvsHandle, NVS_AP_SSID_KEY, settings->ap_ssid);
    nvs_set_str(nvsHandle, NVS_AP_PASSWORD_KEY, settings->ap_password);
    nvs_set_u8(nvsHandle, NVS_RGB_SPEED_KEY, settings->rgb_speed);
    nvs_set_u16(nvsHandle, NVS_RGB_LED_COUNT_KEY, settings->rgb_led_count);

    nvs_set_str(nvsHandle, NVS_PORTAL_URL_KEY, settings->portal_url);
    nvs_set_str(nvsHandle, NVS_PORTAL_SSID_KEY, settings->portal_ssid);
    nvs_set_str(nvsHandle, NVS_PORTAL_PASSWORD_KEY, settings->portal_password);
    nvs_set_str(nvsHandle, NVS_PORTAL_AP_SSID_KEY, settings->portal_ap_ssid);
    nvs_set_str(nvsHandle, NVS_PORTAL_DOMAIN_KEY, settings->portal_domain);
    nvs_set_u8(nvsHandle, NVS_PORTAL_OFFLINE_KEY, settings->portal_offline_mode ? 1 : 0);

    nvs_set_str(nvsHandle, NVS_PRINTER_IP_KEY, settings->printer_ip);
    nvs_set_str(nvsHandle, NVS_PRINTER_TEXT_KEY, settings->printer_text);
    nvs_set_u8(nvsHandle, NVS_PRINTER_FONT_SIZE_KEY, settings->printer_font_size);
    nvs_set_u8(nvsHandle, NVS_PRINTER_ALIGNMENT_KEY, (uint8_t)settings->printer_alignment);

    nvs_set_str(nvsHandle, NVS_FLAPPY_GHOST_NAME, settings->flappy_ghost_name);
    nvs_set_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone);
    nvs_set_str(nvsHandle, NVS_ACCENT_COLOR, settings->selected_hex_accent_color);
    nvs_set_u8(nvsHandle, NVS_GPS_RX_PIN, (uint8_t)settings->gps_rx_pin);
    nvs_set_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, settings->display_timeout_ms);
    nvs_set_u8(nvsHandle, NVS_ENABLE_RTS_KEY, settings->rts_enabled ? 1 : 0);

    nvs_set_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid);
    nvs_set_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password);

    nvs_set_i32(nvsHandle, NVS_RGB_DATA_PIN_KEY, settings->rgb_data_pin);
    nvs_set_i32(nvsHandle, NVS_RGB_RED_PIN_KEY, settings->rgb_red_pin);
    nvs_set_i32(nvsHandle, NVS_RGB_GREEN_PIN_KEY, settings->rgb_green_pin);
    nvs_set_i32(nvsHandle, NVS_RGB_BLUE_PIN_KEY, settings->rgb_blue_pin);

    nvs_set_u8(nvsHandle, NVS_THIRD_CTRL_KEY, settings->third_control_enabled ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_MENU_THEME_KEY, settings->menu_theme);
    nvs_set_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, settings->terminal_text_color);
    nvs_set_u8(nvsHandle, NVS_INVERT_COLORS_KEY, settings->invert_colors ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_WEB_AUTH_KEY, settings->web_auth_enabled ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, settings->webui_restrict_to_ap ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_AP_ENABLED_KEY, settings->ap_enabled ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_POWER_SAVE_KEY, settings->power_save_enabled ? 1 : 0);
    nvs_set_i32(nvsHandle, NVS_ESP_COMM_TX_PIN_KEY, settings->esp_comm_tx_pin);
    nvs_set_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, settings->esp_comm_rx_pin);
    nvs_set_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, settings->zebra_menus_enabled ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, settings->max_screen_brightness);
    nvs_set_u8(nvsHandle, NVS_INFRARED_EASY_MODE_KEY, settings->infrared_easy_mode ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, settings->nav_buttons_enabled ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, settings->auto_save_scans ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, (uint8_t)settings->menu_layout);
    nvs_set_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone);
    nvs_set_u8(nvsHandle, NVS_WIFI_COUNTRY_KEY, settings->wifi_country);
    nvs_set_str(nvsHandle, NVS_WIGLE_API_KEY, settings->wigle_api_key);
    nvs_set_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, settings->wigle_donate ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, settings->wigle_auto_upload ? 1 : 0);
    nvs_set_str(nvsHandle, NVS_IO_BTN_P10_CMD_KEY, settings->io_btn_p10_cmd);
    nvs_set_str(nvsHandle, NVS_IO_BTN_P11_CMD_KEY, settings->io_btn_p11_cmd);
    nvs_set_str(nvsHandle, NVS_IO_BTN_P12_CMD_KEY, settings->io_btn_p12_cmd);
    nvs_set_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, settings->neopixel_max_brightness);
    nvs_set_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, settings->encoder_invert_direction ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, settings->setup_complete ? 1 : 0);

#ifdef CONFIG_WITH_STATUS_DISPLAY
    nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, (uint8_t)settings->status_idle_animation);
    nvs_set_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, settings->status_idle_timeout_ms);
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    nvs_set_u16(nvsHandle, NVS_BADUSB_VID_KEY, settings->badusb_vid);
    nvs_set_u16(nvsHandle, NVS_BADUSB_PID_KEY, settings->badusb_pid);
    nvs_set_str(nvsHandle, NVS_BADUSB_MFR_KEY, settings->badusb_manufacturer);
    nvs_set_str(nvsHandle, NVS_BADUSB_PROD_KEY, settings->badusb_product);
    nvs_set_u8(nvsHandle, NVS_BADUSB_RAND_KEY, settings->badusb_randomize ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_BADUSB_KB_KEY, settings->badusb_kb_layout);
#endif

    // Save MIC RGB Visualizer settings
    nvs_set_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, (uint8_t)settings->mic_visualizer_mode);
    nvs_set_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, (uint8_t)settings->mic_color_mode);
    nvs_set_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, settings->mic_sensitivity);
    nvs_set_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, settings->mic_smoothing);
    nvs_set_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, settings->mic_contrast);
    nvs_set_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, settings->mic_mirror_mode ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, settings->ghostlink_split_view ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, settings->menu_bg_shade);
    nvs_set_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, settings->menu_rounded ? 1 : 0);
    nvs_set_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, settings->menu_item_borders ? 1 : 0);

    esp_err_t err = nvs_commit(nvsHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit settings_save: %s", esp_err_to_name(err));
    }
}

void settings_set_ap_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->ap_ssid, ssid, sizeof(settings->ap_ssid) - 1);
  settings->ap_ssid[sizeof(settings->ap_ssid) - 1] = '\0';
}

const char *settings_get_ap_ssid(const FSettings *settings) {
  return settings->ap_ssid;
}

void settings_set_ap_password(FSettings *settings, const char *password) {
  strncpy(settings->ap_password, password, sizeof(settings->ap_password) - 1);
  settings->ap_password[sizeof(settings->ap_password) - 1] = '\0';
}

const char *settings_get_ap_password(const FSettings *settings) {
  return settings->ap_password;
}

void settings_set_gps_rx_pin(FSettings *settings, uint8_t RxPin) {
  settings->gps_rx_pin = RxPin;
}

uint8_t settings_get_gps_rx_pin(const FSettings *settings) {
  return settings->gps_rx_pin;
}

void settings_set_rgb_speed(FSettings *settings, uint8_t speed) {
  settings->rgb_speed = speed;
}

uint8_t settings_get_rgb_speed(const FSettings *settings) {
  return settings->rgb_speed;
}

// Evil Portal Getters and Setters
void settings_set_portal_url(FSettings *settings, const char *url) {
  strncpy(settings->portal_url, url, sizeof(settings->portal_url) - 1);
  settings->portal_url[sizeof(settings->portal_url) - 1] = '\0';
}

const char *settings_get_portal_url(const FSettings *settings) {
  return settings->portal_url;
}

void settings_set_portal_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->portal_ssid, ssid, sizeof(settings->portal_ssid) - 1);
  settings->portal_ssid[sizeof(settings->portal_ssid) - 1] = '\0';
}

const char *settings_get_portal_ssid(const FSettings *settings) {
  return settings->portal_ssid;
}

void settings_set_portal_password(FSettings *settings, const char *password) {
  strncpy(settings->portal_password, password,
          sizeof(settings->portal_password) - 1);
  settings->portal_password[sizeof(settings->portal_password) - 1] = '\0';
}

const char *settings_get_portal_password(const FSettings *settings) {
  return settings->portal_password;
}

void settings_set_portal_ap_ssid(FSettings *settings, const char *ap_ssid) {
  strncpy(settings->portal_ap_ssid, ap_ssid,
          sizeof(settings->portal_ap_ssid) - 1);
  settings->portal_ap_ssid[sizeof(settings->portal_ap_ssid) - 1] = '\0';
}

const char *settings_get_portal_ap_ssid(const FSettings *settings) {
  return settings->portal_ap_ssid;
}

void settings_set_portal_domain(FSettings *settings, const char *domain) {
  strncpy(settings->portal_domain, domain, sizeof(settings->portal_domain) - 1);
  settings->portal_domain[sizeof(settings->portal_domain) - 1] = '\0';
}

const char *settings_get_portal_domain(const FSettings *settings) {
  return settings->portal_domain;
}

void settings_set_portal_offline_mode(FSettings *settings, bool offline_mode) {
  settings->portal_offline_mode = offline_mode;
}

bool settings_get_portal_offline_mode(const FSettings *settings) {
  return settings->portal_offline_mode;
}

// Power Printer Getters and Setters
void settings_set_printer_ip(FSettings *settings, const char *ip) {
  strncpy(settings->printer_ip, ip, sizeof(settings->printer_ip) - 1);
  settings->printer_ip[sizeof(settings->printer_ip) - 1] = '\0';
}

const char *settings_get_printer_ip(const FSettings *settings) {
  return settings->printer_ip;
}

void settings_set_printer_text(FSettings *settings, const char *text) {
  strncpy(settings->printer_text, text, sizeof(settings->printer_text) - 1);
  settings->printer_text[sizeof(settings->printer_text) - 1] = '\0';
}

const char *settings_get_printer_text(const FSettings *settings) {
  return settings->printer_text;
}

void settings_set_printer_font_size(FSettings *settings, uint8_t font_size) {
  settings->printer_font_size = font_size;
}

uint8_t settings_get_printer_font_size(const FSettings *settings) {
  return settings->printer_font_size;
}

void settings_set_printer_alignment(FSettings *settings,
                                    PrinterAlignment alignment) {
  settings->printer_alignment = alignment;
}

PrinterAlignment settings_get_printer_alignment(const FSettings *settings) {
  return settings->printer_alignment;
}

void settings_set_display_timeout(FSettings *settings, uint32_t timeout_ms) {
  ESP_LOGI(TAG, "Setting display timeout from %lu to %lu ms",
           settings->display_timeout_ms, timeout_ms);
  if (timeout_ms == 0) { // "Never" option
      settings->display_timeout_ms = UINT32_MAX;
  } else {
      settings->display_timeout_ms = timeout_ms;
  }
}

uint32_t settings_get_display_timeout(const FSettings *settings) {
  return settings->display_timeout_ms;
}

// Station Mode Credentials Implementation
void settings_set_sta_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->sta_ssid, ssid, sizeof(settings->sta_ssid) - 1);
  settings->sta_ssid[sizeof(settings->sta_ssid) - 1] = '\0';
}

const char *settings_get_sta_ssid(const FSettings *settings) {
  return settings->sta_ssid;
}

void settings_set_sta_password(FSettings *settings, const char *password) {
  strncpy(settings->sta_password, password, sizeof(settings->sta_password) - 1);
  settings->sta_password[sizeof(settings->sta_password) - 1] = '\0';
}

const char *settings_get_sta_password(const FSettings *settings) {
  return settings->sta_password;
}

void settings_set_rgb_data_pin(FSettings *settings, int32_t pin) {
  settings->rgb_data_pin = pin;
}

int32_t settings_get_rgb_data_pin(const FSettings *settings) {
  return settings->rgb_data_pin;
}

void settings_set_rgb_separate_pins(FSettings *settings, int32_t red, int32_t green, int32_t blue) {
  settings->rgb_red_pin = red;
  settings->rgb_green_pin = green;
  settings->rgb_blue_pin = blue;
}

void settings_get_rgb_separate_pins(const FSettings *settings, int32_t *red, int32_t *green, int32_t *blue) {
  if (red) *red = settings->rgb_red_pin;
  if (green) *green = settings->rgb_green_pin;
  if (blue) *blue = settings->rgb_blue_pin;
}

void settings_set_rgb_led_count(FSettings *settings, uint16_t count) {
  settings->rgb_led_count = count;
}

uint16_t settings_get_rgb_led_count(const FSettings *settings) {
  return settings->rgb_led_count;
}

void settings_set_thirds_control_enabled(FSettings *settings, bool enabled) {
  settings->third_control_enabled = enabled;
}

bool settings_get_thirds_control_enabled(const FSettings *settings) {
  return settings->third_control_enabled;
}

void settings_set_menu_theme(FSettings *settings, uint8_t theme) {
  settings->menu_theme = theme;
}

uint8_t settings_get_menu_theme(const FSettings *settings) {
  return settings->menu_theme;
}

void settings_set_terminal_text_color(FSettings *settings, uint32_t color) {
  settings->terminal_text_color = color;
}

uint32_t settings_get_terminal_text_color(const FSettings *settings) {
  return settings->terminal_text_color;
}

void settings_set_invert_colors(FSettings *settings, bool enabled) {
  settings->invert_colors = enabled;
}

bool settings_get_invert_colors(const FSettings *settings) {
  return settings->invert_colors;
}

void settings_set_web_auth_enabled(FSettings *settings, bool enabled) {
  settings->web_auth_enabled = enabled;
}

bool settings_get_web_auth_enabled(const FSettings *settings) {
  return settings->web_auth_enabled;
}

void settings_set_webui_restrict_to_ap(FSettings *settings, bool enabled) {
  settings->webui_restrict_to_ap = enabled;
}

bool settings_get_webui_restrict_to_ap(const FSettings *settings) {
  return settings->webui_restrict_to_ap;
}

void settings_set_ap_enabled(FSettings *settings, bool enabled) {
  settings->ap_enabled = enabled;
}

bool settings_get_ap_enabled(const FSettings *settings) {
  return settings->ap_enabled;
}

void settings_set_power_save_enabled(FSettings *settings, bool enabled) {
  settings->power_save_enabled = enabled;
}

bool settings_get_power_save_enabled(const FSettings *settings) {
  return settings->power_save_enabled;
}

void settings_set_esp_comm_pins(FSettings *settings, int32_t tx_pin, int32_t rx_pin) {
  settings->esp_comm_tx_pin = tx_pin;
  settings->esp_comm_rx_pin = rx_pin;
}

void settings_get_esp_comm_pins(const FSettings *settings, int32_t *tx_pin, int32_t *rx_pin) {
  if (tx_pin) *tx_pin = settings->esp_comm_tx_pin;
  if (rx_pin) *rx_pin = settings->esp_comm_rx_pin;
}


void settings_set_max_screen_brightness(FSettings *settings, uint8_t value) {
    if (value > 100) value = 100;
    settings->max_screen_brightness = value;
}
uint8_t settings_get_max_screen_brightness(const FSettings *settings) {
    return settings->max_screen_brightness;
}


// Infrared Settings Getters and Setters
void settings_set_infrared_easy_mode(FSettings *settings, bool enabled) {
  settings->infrared_easy_mode = enabled;
}

bool settings_get_infrared_easy_mode(const FSettings *settings) {
  return settings->infrared_easy_mode;
}

void settings_get_nvs_stats(nvs_stats_t *stats) {
  esp_err_t err = nvs_get_stats(NULL, stats);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get NVS stats: %s", esp_err_to_name(err));
    memset(stats, 0, sizeof(nvs_stats_t));
  }
}

size_t settings_get_nvs_used_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.used_entries;
}

size_t settings_get_nvs_free_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.free_entries;
}

size_t settings_get_nvs_total_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.total_entries;
}

float settings_get_nvs_usage_percentage(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  if (stats.total_entries == 0) {
    return 0.0f;
  }
  return ((float)stats.used_entries / (float)stats.total_entries) * 100.0f;
}

void settings_print_nvs_stats(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  
  ESP_LOGI(TAG, "NVS Storage Statistics:");
  ESP_LOGI(TAG, "  Total entries: %zu", stats.total_entries);
  ESP_LOGI(TAG, "  Used entries: %zu", stats.used_entries);
  ESP_LOGI(TAG, "  Free entries: %zu", stats.free_entries);
  ESP_LOGI(TAG, "  Namespaces: %zu", stats.namespace_count);
  ESP_LOGI(TAG, "  Usage: %.1f%%", settings_get_nvs_usage_percentage());
  
  printf("NVS Storage: %zu/%zu entries used (%.1f%%)\n", 
         stats.used_entries, stats.total_entries, 
         settings_get_nvs_usage_percentage());
}

size_t settings_get_namespace_used_entries(const char *namespace_name) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", 
             namespace_name, esp_err_to_name(err));
    return 0;
  }
  
  size_t used_entries = 0;
  err = nvs_get_used_entry_count(handle, &used_entries);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get used entry count for namespace '%s': %s", 
             namespace_name, esp_err_to_name(err));
    used_entries = 0;
  }
  
  nvs_close(handle);
  return used_entries;
}

void settings_print_namespace_stats(const char *namespace_name) {
  size_t used_entries = settings_get_namespace_used_entries(namespace_name);
  ESP_LOGI(TAG, "Namespace '%s': %zu entries used", namespace_name, used_entries);
  printf("Namespace '%s': %zu entries used\n", namespace_name, used_entries);
}

void settings_set_zebra_menus_enabled(FSettings *settings, bool enabled) {
    settings->zebra_menus_enabled = enabled;
}

bool settings_get_zebra_menus_enabled(const FSettings *settings) {
    return settings->zebra_menus_enabled;
}

void settings_set_nav_buttons_enabled(FSettings *settings, bool enabled) {
    settings->nav_buttons_enabled = enabled;
}

bool settings_get_nav_buttons_enabled(const FSettings *settings) {
    return settings->nav_buttons_enabled;
}

void settings_set_auto_save_scans(FSettings *settings, bool enabled) {
    settings->auto_save_scans = enabled;
}

bool settings_get_auto_save_scans(const FSettings *settings) {
    return settings->auto_save_scans;
}

// Menu layout settings
void settings_set_menu_layout(FSettings *settings, uint8_t layout) {
    settings->menu_layout = layout;
}

uint8_t settings_get_menu_layout(const FSettings *settings) {
    return settings->menu_layout;
}

// Neopixel brightness settings
void settings_set_neopixel_max_brightness(FSettings *settings, uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    settings->neopixel_max_brightness = brightness;
}

uint8_t settings_get_neopixel_max_brightness(const FSettings *settings) {
    return settings->neopixel_max_brightness;
}

void settings_set_encoder_invert_direction(FSettings *settings, bool enabled) {
  settings->encoder_invert_direction = enabled;
}

bool settings_get_encoder_invert_direction(const FSettings *settings) {
  return settings->encoder_invert_direction;
}

void settings_set_setup_complete(FSettings *settings, bool complete) {
  settings->setup_complete = complete;
}

bool settings_get_setup_complete(const FSettings *settings) {
  return settings->setup_complete;
}

void settings_set_wifi_country(FSettings *settings, uint8_t country) {
  settings->wifi_country = country;
}

uint8_t settings_get_wifi_country(const FSettings *settings) {
  return settings->wifi_country;
}

void settings_set_wigle_auto_upload(FSettings *settings, bool enabled) {
  settings->wigle_auto_upload = enabled;
}

bool settings_get_wigle_auto_upload(const FSettings *settings) {
  return settings->wigle_auto_upload;
}

void settings_set_wigle_donate(FSettings *settings, bool enabled) {
  settings->wigle_donate = enabled;
}

bool settings_get_wigle_donate(const FSettings *settings) {
  return settings->wigle_donate;
}

const char *settings_get_io_btn_p10_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p10_cmd : "";
}
void settings_set_io_btn_p10_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p10_cmd, cmd, sizeof(settings->io_btn_p10_cmd) - 1);
  settings->io_btn_p10_cmd[sizeof(settings->io_btn_p10_cmd) - 1] = '\0';
}
const char *settings_get_io_btn_p11_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p11_cmd : "";
}
void settings_set_io_btn_p11_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p11_cmd, cmd, sizeof(settings->io_btn_p11_cmd) - 1);
  settings->io_btn_p11_cmd[sizeof(settings->io_btn_p11_cmd) - 1] = '\0';
}
const char *settings_get_io_btn_p12_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p12_cmd : "";
}
void settings_set_io_btn_p12_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p12_cmd, cmd, sizeof(settings->io_btn_p12_cmd) - 1);
  settings->io_btn_p12_cmd[sizeof(settings->io_btn_p12_cmd) - 1] = '\0';
}

#ifdef CONFIG_WITH_STATUS_DISPLAY
void settings_set_status_idle_animation(FSettings *settings, IdleAnimation anim) {
  settings->status_idle_animation = anim;
}

IdleAnimation settings_get_status_idle_animation(const FSettings *settings) {
  return settings->status_idle_animation;
}

void settings_set_status_idle_timeout_ms(FSettings *settings, uint32_t timeout_ms) {
  settings->status_idle_timeout_ms = timeout_ms;
}

uint32_t settings_get_status_idle_timeout_ms(const FSettings *settings) {
  return settings->status_idle_timeout_ms;
}
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
void settings_set_badusb_vid(FSettings *settings, uint16_t vid) {
  settings->badusb_vid = vid;
}
uint16_t settings_get_badusb_vid(const FSettings *settings) {
  return settings->badusb_vid;
}
void settings_set_badusb_pid(FSettings *settings, uint16_t pid) {
  settings->badusb_pid = pid;
}
uint16_t settings_get_badusb_pid(const FSettings *settings) {
  return settings->badusb_pid;
}
void settings_set_badusb_manufacturer(FSettings *settings, const char *name) {
  strncpy(settings->badusb_manufacturer, name, sizeof(settings->badusb_manufacturer) - 1);
  settings->badusb_manufacturer[sizeof(settings->badusb_manufacturer) - 1] = '\0';
}
const char *settings_get_badusb_manufacturer(const FSettings *settings) {
  return settings->badusb_manufacturer;
}
void settings_set_badusb_product(FSettings *settings, const char *name) {
  strncpy(settings->badusb_product, name, sizeof(settings->badusb_product) - 1);
  settings->badusb_product[sizeof(settings->badusb_product) - 1] = '\0';
}
const char *settings_get_badusb_product(const FSettings *settings) {
  return settings->badusb_product;
}
void settings_set_badusb_randomize(FSettings *settings, bool enabled) {
  settings->badusb_randomize = enabled;
}
bool settings_get_badusb_randomize(const FSettings *settings) {
  return settings->badusb_randomize;
}
void settings_set_badusb_kb_layout(FSettings *settings, uint8_t layout) {
  settings->badusb_kb_layout = layout;
}
uint8_t settings_get_badusb_kb_layout(const FSettings *settings) {
  return settings->badusb_kb_layout;
}

void settings_reset_badusb_defaults(FSettings *settings) {
  settings->badusb_vid = 0x1209;
  settings->badusb_pid = 0x0001;
  strcpy(settings->badusb_manufacturer, "USB Device");
  strcpy(settings->badusb_product, "HID Keyboard");
  settings->badusb_randomize = false;
  settings->badusb_kb_layout = KB_LAYOUT_US;
}

// MIC RGB Visualizer getters and setters
void settings_set_mic_visualizer_mode(FSettings *settings, MicVisualizerMode mode) {
  if (settings) {
    settings->mic_visualizer_mode = mode;
  }
}

MicVisualizerMode settings_get_mic_visualizer_mode(const FSettings *settings) {
  return settings ? settings->mic_visualizer_mode : MIC_MODE_4BAND_SPECTRUM;
}

void settings_set_mic_color_mode(FSettings *settings, MicColorMode mode) {
  if (settings) {
    settings->mic_color_mode = mode;
  }
}

MicColorMode settings_get_mic_color_mode(const FSettings *settings) {
  return settings ? settings->mic_color_mode : MIC_COLOR_RAINBOW;
}

void settings_set_mic_sensitivity(FSettings *settings, uint8_t sensitivity) {
  if (settings) {
    if (sensitivity > 100) sensitivity = 100;
    settings->mic_sensitivity = sensitivity;
  }
}

uint8_t settings_get_mic_sensitivity(const FSettings *settings) {
  return settings ? settings->mic_sensitivity : 50;
}

void settings_set_mic_smoothing(FSettings *settings, uint8_t smoothing) {
  if (settings) {
    if (smoothing > 100) smoothing = 100;
    settings->mic_smoothing = smoothing;
  }
}

uint8_t settings_get_mic_smoothing(const FSettings *settings) {
  return settings ? settings->mic_smoothing : 30;
}

void settings_set_mic_contrast(FSettings *settings, uint8_t contrast) {
  if (settings) {
    if (contrast < 1) contrast = 1;
    if (contrast > 5) contrast = 5;
    settings->mic_contrast = contrast;
  }
}

uint8_t settings_get_mic_contrast(const FSettings *settings) {
  return settings ? settings->mic_contrast : 2;
}

void settings_set_mic_mirror_mode(FSettings *settings, bool enabled) {
  if (settings) {
    settings->mic_mirror_mode = enabled;
  }
}

bool settings_get_mic_mirror_mode(const FSettings *settings) {
  return settings ? settings->mic_mirror_mode : false;
}

void settings_set_mic_calibrate(FSettings *settings, bool calibrate) {
  // This is an action trigger, actual calibration happens elsewhere
  if (settings && calibrate) {
    // Trigger calibration via goertzel_restart_cal() or similar
    extern void goertzel_restart_cal(void);
    goertzel_restart_cal();
  }
}

bool settings_get_mic_calibrate(const FSettings *settings) {
  // Calibration is a one-shot action, always returns false
  return false;
}
#endif

void settings_set_ghostlink_split_view(FSettings *settings, bool enabled) {
  if (settings) {
    settings->ghostlink_split_view = enabled;
  }
}

bool settings_get_ghostlink_split_view(const FSettings *settings) {
  return settings ? settings->ghostlink_split_view : true;
}

void settings_set_menu_bg_shade(FSettings *settings, uint8_t shade) {
  if (settings) {
    settings->menu_bg_shade = shade;
  }
}

uint8_t settings_get_menu_bg_shade(const FSettings *settings) {
  return settings ? settings->menu_bg_shade : 1;
}

void settings_set_menu_rounded(FSettings *settings, bool enabled) {
  if (settings) {
    settings->menu_rounded = enabled;
  }
}

bool settings_get_menu_rounded(const FSettings *settings) {
  return settings ? settings->menu_rounded : false;
}

void settings_set_menu_item_borders(FSettings *settings, bool enabled) {
  if (settings) {
    settings->menu_item_borders = enabled;
  }
}

bool settings_get_menu_item_borders(const FSettings *settings) {
  return settings ? settings->menu_item_borders : true;
}
