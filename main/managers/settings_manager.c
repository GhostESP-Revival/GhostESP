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
static const char *NVS_PRINTER_FONT_SIZE_KEY = "printer_font_size";
static const char *NVS_PRINTER_ALIGNMENT_KEY = "printer_alignment";
static const char *NVS_PRINTER_CONNECTED_KEY = "printer_connected";
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
static const char *NVS_WEB_AUTH_KEY = "web_auth";
static const char *NVS_ESP_COMM_TX_PIN_KEY = "esp_comm_tx";
static const char *NVS_ESP_COMM_RX_PIN_KEY = "esp_comm_rx";
static const char *NVS_AP_ENABLED_KEY = "ap_enabled";
static const char *NVS_POWER_SAVE_KEY = "power_save";

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
  settings->menu_theme = 0;
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
  settings->display_timeout_ms = UINT32_MAX; // Default to never timeout
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
  settings->web_auth_enabled = true;
  settings->esp_comm_tx_pin = 6;
  settings->esp_comm_rx_pin = 7;
  settings->ap_enabled = true; // Default to enabled
  settings->power_save_enabled = false;
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

#ifdef CONFIG_HAS_RTC_CLOCK
  str_size = sizeof(settings->selected_timezone);
  err = nvs_get_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Timezone String\n");
  }
#endif

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
    settings->display_timeout_ms = UINT32_MAX; // Default to never timeout if not found
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
    settings->web_auth_enabled = (value_u8 != 0);
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
    settings->esp_comm_tx_pin = 6;
  }
  
  err = nvs_get_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->esp_comm_rx_pin = tmp;
  } else {
    settings->esp_comm_rx_pin = 7;
  }
}

static void update_rainbow_effect(const FSettings *settings) {
#ifndef CONFIG_WITH_SCREEN
  return;
#endif

  if (settings_get_rgb_mode(settings) == RGB_MODE_RAINBOW) {
    if (rainbow_timer == NULL) {
      rainbow_timer = lv_timer_create(rainbow_effect_cb, 50, NULL);
      rainbow_hue = 0;
    }
  } else {
    if (rainbow_timer != NULL) {
      lv_timer_del(rainbow_timer);
      rainbow_timer = NULL;
      // Reset status bar color when leaving rainbow mode
      display_manager_update_status_bar_color();
    }
  }
}


void settings_save(const FSettings *settings) {
  ESP_LOGI(TAG, "Starting settings save process");
  ESP_LOGI(TAG, "Current display timeout: %lu ms",
           settings->display_timeout_ms);
  ESP_LOGI(TAG, "Current timezone: %s", settings->selected_timezone);

  esp_err_t err;

  // Save RGB Mode
  err = nvs_set_u8(nvsHandle, NVS_RGB_MODE_KEY, (uint8_t)settings->rgb_mode);
  if (err != ESP_OK) {
    printf("Failed to save RGB Mode\n");
  }

  // Save Channel Delay
  err = nvs_set_blob(nvsHandle, NVS_CHANNEL_DELAY_KEY, &settings->channel_delay,
                     sizeof(settings->channel_delay));
  if (err != ESP_OK) {
    printf("Failed to save Channel Delay\n");
  }

  // Save Broadcast Speed
  err = nvs_set_u16(nvsHandle, NVS_BROADCAST_SPEED_KEY,
                    settings->broadcast_speed);
  if (err != ESP_OK) {
    printf("Failed to save Broadcast Speed\n");
  }

  // Save AP SSID
  err = nvs_set_str(nvsHandle, NVS_AP_SSID_KEY, settings->ap_ssid);
  if (err != ESP_OK) {
    printf("Failed to save AP SSID\n");
  }

  // Save AP Password
  err = nvs_set_str(nvsHandle, NVS_AP_PASSWORD_KEY, settings->ap_password);
  if (err != ESP_OK) {
    printf("Failed to save AP Password\n");
  }

  // Save RGB Speed
  err = nvs_set_u8(nvsHandle, NVS_RGB_SPEED_KEY, settings->rgb_speed);
  if (err != ESP_OK) {
    printf("Failed to save RGB Speed\n");
  }

  // Save RTS Enabled
  err = nvs_set_u8(nvsHandle, NVS_ENABLE_RTS_KEY, settings->rts_enabled);
  if (err != ESP_OK) {
    printf("Failed to save RTS Enabled\n");
  }

  // Save Third Control Enabled
  err = nvs_set_u8(nvsHandle, NVS_THIRD_CTRL_KEY, settings->third_control_enabled);
  if (err != ESP_OK) {
    printf("Failed to save Third Control Enabled\n");
  }

  // Save Evil Portal settings
  err = nvs_set_str(nvsHandle, NVS_PORTAL_URL_KEY, settings->portal_url);
  if (err != ESP_OK) {
    printf("Failed to save Portal URL\n");
  }

  err = nvs_set_str(nvsHandle, NVS_PORTAL_SSID_KEY, settings->portal_ssid);
  if (err != ESP_OK) {
    printf("Failed to save Portal SSID\n");
  }

  err = nvs_set_str(nvsHandle, NVS_PORTAL_PASSWORD_KEY,
                    settings->portal_password);
  if (err != ESP_OK) {
    printf("Failed to save Portal Password\n");
  }

  err =
      nvs_set_str(nvsHandle, NVS_PORTAL_AP_SSID_KEY, settings->portal_ap_ssid);
  if (err != ESP_OK) {
    printf("Failed to save Portal AP SSID\n");
  }

  err = nvs_set_str(nvsHandle, NVS_PORTAL_DOMAIN_KEY, settings->portal_domain);
  if (err != ESP_OK) {
    printf("Failed to save Portal Domain\n");
  }

  err = nvs_set_u8(nvsHandle, NVS_PORTAL_OFFLINE_KEY,
                   settings->portal_offline_mode);
  if (err != ESP_OK) {
    printf("Failed to save Portal Offline Mode\n");
  }

  // Save Power Printer settings
  err = nvs_set_str(nvsHandle, NVS_PRINTER_IP_KEY, settings->printer_ip);
  if (err != ESP_OK) {
    printf("Failed to save Printer IP\n");
  }

  err = nvs_set_str(nvsHandle, NVS_PRINTER_TEXT_KEY, settings->printer_text);
  if (err != ESP_OK) {
    printf("Failed to save Printer Text\n");
  }

  err = nvs_set_u8(nvsHandle, NVS_PRINTER_FONT_SIZE_KEY,
                   settings->printer_font_size);
  if (err != ESP_OK) {
    printf("Failed to save Printer Font Size\n");
  }

  err = nvs_set_u8(nvsHandle, NVS_PRINTER_ALIGNMENT_KEY,
                   (uint8_t)settings->printer_alignment);
  if (err != ESP_OK) {
    printf("Failed to save Printer Alignment\n");
  }

  err = nvs_set_str(nvsHandle, NVS_FLAPPY_GHOST_NAME,
                    settings->flappy_ghost_name);
  if (err != ESP_OK) {
    printf("Failed to save Flappy Ghost Name\n");
  }

  err = nvs_set_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone);
  if (err != ESP_OK) {
    printf("Failed to Save Timezone String %s\n", esp_err_to_name(err));
  }

  err = nvs_set_str(nvsHandle, NVS_ACCENT_COLOR,
                    settings->selected_hex_accent_color);
  if (err != ESP_OK) {
    printf("Failed to Save Hex Accent Color %s", esp_err_to_name(err));
  }

  err = nvs_set_u8(nvsHandle, NVS_GPS_RX_PIN, (uint8_t)settings->gps_rx_pin);
  if (err != ESP_OK) {
    printf("Failed to save Printer Alignment\n");
  }

  err = nvs_set_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY,
                    settings->display_timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save Display Timeout");
  }

  // Save Station SSID
  err = nvs_set_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid);
  if (err != ESP_OK) {
    printf("Failed to save STA SSID\n");
  }

  // Save Station Password
  err = nvs_set_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password);
  if (err != ESP_OK) {
    printf("Failed to save STA Password\n");
  }

  err = nvs_set_i32(nvsHandle, NVS_RGB_DATA_PIN_KEY, settings->rgb_data_pin);
  if (err != ESP_OK) {
    printf("Failed to save RGB data pin\n");
  }
  err = nvs_set_i32(nvsHandle, NVS_RGB_RED_PIN_KEY, settings->rgb_red_pin);
  if (err != ESP_OK) {
    printf("Failed to save RGB red pin\n");
  }
  err = nvs_set_i32(nvsHandle, NVS_RGB_GREEN_PIN_KEY, settings->rgb_green_pin);
  if (err != ESP_OK) {
    printf("Failed to save RGB green pin\n");
  }
  err = nvs_set_i32(nvsHandle, NVS_RGB_BLUE_PIN_KEY, settings->rgb_blue_pin);
  if (err != ESP_OK) {
    printf("Failed to save RGB blue pin\n");
  }

  if (settings_get_rgb_mode(settings) == RGB_MODE_NORMAL) {
    // Normal: static color/off
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }
    rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false);
} else if (settings_get_rgb_mode(settings) == RGB_MODE_RAINBOW) {
    // Rainbow: animated
    if (rgb_effect_task_handle == NULL) {
        xTaskCreate(rainbow_task, "Rainbow Task", 8192, &rgb_manager, 1, &rgb_effect_task_handle);
    }
} else if (settings_get_rgb_mode(settings) == RGB_MODE_STEALTH) {
    // Stealth: LEDs always off
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }
    rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false); // Ensure LEDs are off
  }


  update_rainbow_effect(settings);

  // Commit all changes
  err = nvs_commit(nvsHandle);
  if (err != ESP_OK) {
    printf("Failed to commit NVS changes\n");
  } else {
    printf("Settings saved to NVS.\n");
  }

#ifdef CONFIG_HAS_RTC_CLOCK
  // Apply timezone change immediately
  ESP_LOGI(TAG, "Applying timezone change: %s", settings->selected_timezone);
  setenv("TZ", settings->selected_timezone, 1);
  tzset();
#endif

  // Update global settings immediately
  ESP_LOGI(TAG, "Updating global settings");
  ESP_LOGI(TAG, "Old display timeout: %lu ms", G_Settings.display_timeout_ms);
  memcpy(&G_Settings, settings, sizeof(FSettings));
  ESP_LOGI(TAG, "New display timeout: %lu ms", G_Settings.display_timeout_ms);

  err = nvs_commit(nvsHandle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Settings saved to NVS successfully");
  }

  err = nvs_set_u8(nvsHandle, NVS_MENU_THEME_KEY, settings->menu_theme);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save menu_theme: %s", esp_err_to_name(err));
  err = nvs_set_u8(nvsHandle, NVS_INVERT_COLORS_KEY, settings->invert_colors);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save invert_colors: %s", esp_err_to_name(err));
  err = nvs_set_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, settings->terminal_text_color);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save terminal_text_color: %s", esp_err_to_name(err));
  err = nvs_set_u8(nvsHandle, NVS_WEB_AUTH_KEY, settings->web_auth_enabled);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save web_auth_enabled: %s", esp_err_to_name(err));
  err = nvs_set_u8(nvsHandle, NVS_AP_ENABLED_KEY, settings->ap_enabled);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save ap_enabled: %s", esp_err_to_name(err));
  err = nvs_set_u8(nvsHandle, NVS_POWER_SAVE_KEY, settings->power_save_enabled);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save power_save_enabled: %s", esp_err_to_name(err));
  
  err = nvs_set_i32(nvsHandle, NVS_ESP_COMM_TX_PIN_KEY, settings->esp_comm_tx_pin);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save esp_comm_tx_pin: %s", esp_err_to_name(err));
  
  err = nvs_set_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, settings->esp_comm_rx_pin);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to save esp_comm_rx_pin: %s", esp_err_to_name(err));
  
  err = nvs_commit(nvsHandle);
  if (err != ESP_OK) ESP_LOGE(S_TAG, "Failed to commit settings: %s", esp_err_to_name(err));
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
  settings
      ->selected_hex_accent_color[sizeof(settings->selected_hex_accent_color) -
                                  1] = '\0';
}

const char *settings_get_accent_color_str(const FSettings *settings) {
  return settings->selected_hex_accent_color;
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
