#include "managers/sd_card_manager.h"
#include "core/utils.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "vendor/drivers/CH422G.h"
#include "vendor/pcap.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/status_display_manager.h"

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

static const char *TAG = "SD_Card_Manager";
static const char *NVS_NAMESPACE = "sd_config";

/* time multiplex spi when display and sd share the spi bus */
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI) && !defined(CONFIG_USE_TDISPLAY_S3)
#include "lvgl_helpers.h"
#include "lvgl_tft/disp_spi.h"
#include "lvgl_spi_conf.h"
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include "managers/display_manager.h"
static bool s_display_spi_suspended_flag = false;
static bool is_shared_display_sd_spi(void) {
#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_LV_TFT_DISPLAY_SPI2_HOST)
  return true;
#elif defined(CONFIG_LV_DISP_SPI_MOSI) && defined(CONFIG_LV_DISP_SPI_CLK)
  bool mosi_match = (sd_card_manager.spi_mosi_pin == CONFIG_LV_DISP_SPI_MOSI);
  bool clk_match = (sd_card_manager.spi_clk_pin == CONFIG_LV_DISP_SPI_CLK);
#if defined(CONFIG_LV_DISP_SPI_MISO)
  bool miso_match = (sd_card_manager.spi_miso_pin == CONFIG_LV_DISP_SPI_MISO);
  return mosi_match && clk_match && miso_match;
#else
  return mosi_match && clk_match;
#endif
#else
  return false;
#endif
}
static bool display_spi_suspend_for_sd(void) {
  if (!is_shared_display_sd_spi()) {
    return false;
  }
  /* pause lvgl refresh to stop flush() while we steal the bus */
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) {
    lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
    if (refr) lv_timer_pause(refr);
  }
  /* wait all pending transactions, drop device, free bus */
  display_manager_suspend_lvgl_task();
  disp_wait_for_pending_transactions();
  disp_spi_remove_device();
  spi_bus_free(TFT_SPI_HOST);
  /* assert CS high so that panel stays quiet */
  #ifdef CONFIG_LV_DISP_SPI_CS
  gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
  #endif
  s_display_spi_suspended_flag = true;
  return true;
}
static void display_spi_resume_after_sd(void) {
  if (!is_shared_display_sd_spi()) {
    return;
  }
  if (!s_display_spi_suspended_flag) {
    return;
  }
  (void)lvgl_spi_driver_init(TFT_SPI_HOST, DISP_SPI_MISO, DISP_SPI_MOSI, DISP_SPI_CLK,
                             SPI_BUS_MAX_TRANSFER_SZ, 1, DISP_SPI_IO2, DISP_SPI_IO3);
  disp_spi_add_device(TFT_SPI_HOST);
  /* resume lvgl refresh */
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) {
    lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
    if (refr) lv_timer_resume(refr);
  }
  display_manager_resume_lvgl_task();
  s_display_spi_suspended_flag = false;
}
#else
static bool display_spi_suspend_for_sd(void) { return false; }
static void display_spi_resume_after_sd(void) {}
#endif



sd_card_manager_t sd_card_manager = { // Change this based on board config
    .card = NULL,
    .is_initialized = false,
    .clkpin = 19,
    .cmdpin = 18,
    .d0pin = 20,
    .d1pin = 21,
    .d2pin = 22,
    .d3pin = 23,
#ifdef CONFIG_USING_SPI
    .spi_cs_pin = CONFIG_SD_SPI_CS_PIN,
    .spi_clk_pin = CONFIG_SD_SPI_CLK_PIN,
    .spi_miso_pin = CONFIG_SD_SPI_MISO_PIN,
    .spi_mosi_pin = CONFIG_SD_SPI_MOSI_PIN
#endif
};

// track SPI bus initialization and mount type locally so we only free what we
// initialized and always clear initialized state on unmount
static bool s_spi_bus_initialized = false;
static int s_spi_host_id = -1;
typedef enum { MOUNT_NONE = 0, MOUNT_VIRTUAL, MOUNT_SDMMC, MOUNT_SPI } sd_mount_type_t;
static sd_mount_type_t s_mount_type = MOUNT_NONE;
static TickType_t s_next_unmount_tick = 0;

static sd_card_cached_stats_t s_cached_stats = { .valid = false, .used_pct = 0 };

static void sd_card_update_cached_stats(void) {
    if (!sd_card_manager.is_initialized) {
        s_cached_stats.valid = false;
        return;
    }
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info("/mnt", &total_bytes, &free_bytes);
    if (ret == ESP_OK && total_bytes > 0) {
        uint64_t used_bytes = total_bytes - free_bytes;
        s_cached_stats.used_pct = (int)((used_bytes * 100) / total_bytes);
        if (s_cached_stats.used_pct < 0) s_cached_stats.used_pct = 0;
        if (s_cached_stats.used_pct > 100) s_cached_stats.used_pct = 100;
        s_cached_stats.valid = true;
    }
}

void sd_card_get_cached_stats(sd_card_cached_stats_t *out) {
    if (out) {
        *out = s_cached_stats;
    }
}

#ifdef CONFIG_IS_S3TWATCH
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_virtual_storage_mounted = false;

static esp_err_t mount_virtual_storage(void) {
    if (s_virtual_storage_mounted) {
        ESP_LOGI(TAG, "Virtual storage already mounted");
        return ESP_OK;
    }

    const esp_partition_t* storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!storage_partition) {
        ESP_LOGE(TAG, "Storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found storage partition at offset 0x%lx with size %lu KB", 
             (unsigned long)storage_partition->address, (unsigned long)(storage_partition->size / 1024));
    
    if (storage_partition->size < 64 * 1024) {
        ESP_LOGE(TAG, "Storage partition too small: %lu bytes", (unsigned long)storage_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 4 * 1024
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl("/mnt", "storage", &mount_config, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount virtual storage: %s", esp_err_to_name(ret));
        return ret;
    }

    s_virtual_storage_mounted = true;
    ESP_LOGI(TAG, "Virtual storage mounted successfully at /mnt");
    s_mount_type = MOUNT_VIRTUAL;
    status_display_show_status("Virtual SD OK");
    return ESP_OK;
}

static void unmount_virtual_storage(void) {
    if (!s_virtual_storage_mounted) {
        return;
    }

    esp_vfs_fat_spiflash_unmount_rw_wl("/mnt", s_wl_handle);
    s_virtual_storage_mounted = false;
    s_wl_handle = WL_INVALID_HANDLE;
    ESP_LOGI(TAG, "Virtual storage unmounted");
    s_mount_type = MOUNT_NONE;
    status_display_show_status("Virtual SD Off");
}
#endif

void list_files_recursive(const char *dirname, int level) {
  DIR *dir = opendir(dirname);
  if (!dir) {
    printf("Failed to open directory: %s\n", dirname);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);

    if (written < 0 || written >= sizeof(path)) {
      printf("Path was truncated: %s/%s\n", dirname, entry->d_name);
      continue;
    }

    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
      for (int i = 0; i < level; i++) {
        printf("  ");
      }

      if (S_ISDIR(statbuf.st_mode)) {
        printf("[Dir] %s/\n", entry->d_name);
        list_files_recursive(path, level + 1);
      } else {
        printf("[File] %s\n", entry->d_name);
      }
    }
  }
  closedir(dir);
}

static void sdmmc_card_print_info(const sdmmc_card_t *card) {
  if (card == NULL) {
    printf("Card is NULL\n");
    return;
  }

  printf("Name: %s\n", card->cid.name);
  printf("Type: %s\n", (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
  printf("Capacity: %lluMB\n", ((uint64_t)card->csd.capacity) *
                                   card->csd.sector_size / (1024 * 1024));
  printf("Sector size: %dB\n", card->csd.sector_size);
  printf("Speed: %s\n",
         (card->csd.tr_speed > 25000000) ? "high speed" : "default speed");

  if (card->is_mem) {
    printf("Card is memory card\n");
    printf("CSD version: %d\n", card->csd.csd_ver);
    printf("Manufacture ID: %02x\n", card->cid.mfg_id);
    printf("Serial number: %08x\n", card->cid.serial);
  } else {
    printf("Card is not a memory card\n");
  }
}

esp_err_t sd_card_init(void) {
  esp_err_t ret = ESP_FAIL;


#ifdef CONFIG_IS_S3TWATCH
  ESP_LOGI(TAG, "S3TWatch detected - attempting virtual storage mount");
  
  vTaskDelay(pdMS_TO_TICKS(100));
  
  ret = mount_virtual_storage();
  if (ret == ESP_OK) {
    sd_card_manager.is_initialized = true;
    ESP_LOGI(TAG, "Virtual storage initialized successfully");
    sd_card_setup_directory_structure();
    return ESP_OK;
  } else {
    ESP_LOGW(TAG, "Virtual storage mount failed (%s), falling back to physical SD card", esp_err_to_name(ret));
  }
#endif

  // Load configuration from NVS first
  sd_card_load_config();
  sd_card_print_config(); // Print loaded/default config

  // Backup current config in case init fails
  sd_card_manager_t backup_config = sd_card_manager;

#ifdef CONFIG_USING_MMC_1_BIT
  printf("Initializing SD card in SDMMC mode (1-bit) using configured pins...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;

  slot_config.clk = CONFIG_SD_MMC_CLK;
  slot_config.cmd = CONFIG_SD_MMC_CMD;
  slot_config.d0 = CONFIG_SD_MMC_D0;

  gpio_set_pull_mode(CONFIG_SD_MMC_D0, GPIO_PULLUP_ONLY);  // CLK
  gpio_set_pull_mode(CONFIG_SD_MMC_CLK, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(CONFIG_SD_MMC_CMD, GPIO_PULLUP_ONLY); // D0

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    return ret;
  }

  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SDMMC;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();

#elif defined(CONFIG_USING_MMC)

  printf("Initializing SD card in SDMMC mode (4-bit) using configured pins...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  slot_config.clk = sd_card_manager.clkpin;
  slot_config.cmd = sd_card_manager.cmdpin; // SDMMC_CMD -> GPIO 16
  slot_config.d0 = sd_card_manager.d0pin;   // SDMMC_D0  -> GPIO 14
  slot_config.d1 = sd_card_manager.d1pin;   // SDMMC_D1  -> GPIO 17
  slot_config.d2 = sd_card_manager.d2pin;   // SDMMC_D2  -> GPIO 21
  slot_config.d3 = sd_card_manager.d3pin;   // SDMMC_D3  -> GPIO 18

  host.flags = SDMMC_HOST_FLAG_4BIT;

  gpio_set_pull_mode(sd_card_manager.clkpin, GPIO_PULLUP_ONLY); // CLK
  gpio_set_pull_mode(sd_card_manager.cmdpin, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(sd_card_manager.d0pin, GPIO_PULLUP_ONLY);  // D0
  gpio_set_pull_mode(sd_card_manager.d1pin, GPIO_PULLUP_ONLY);  // D1
  gpio_set_pull_mode(sd_card_manager.d2pin, GPIO_PULLUP_ONLY);  // D2
  gpio_set_pull_mode(sd_card_manager.d3pin, GPIO_PULLUP_ONLY);  // D3

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();
#elif CONFIG_USING_SPI

  printf("Initializing SD card in SPI mode using configured pins...\n");

  bool gating_template = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
  gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#endif
  bool display_was_suspended = false;
  if (gating_template) {
    display_was_suspended = display_spi_suspend_for_sd();
  }



#ifdef CONFIG_Waveshare_LCD
#define I2C_NUM I2C_NUM_0
#define I2C_ADDRESS 0x24
#define EXIO4_BIT (1 << 4)
#define EXIO1_BIT (1 << 1)

  esp_io_expander_ch422g_t *ch422g_dev = NULL;
  esp_err_t err;

  err = ch422g_new_device(I2C_NUM, I2C_ADDRESS, &ch422g_dev);
  if (err != ESP_OK) {
    printf("Failed to initialize CH422G: %s\n", esp_err_to_name(err));
    return err;
  }

  uint32_t direction, output_value;

  err = ch422g_read_direction_reg(ch422g_dev, &direction);
  if (err != ESP_OK) {
    printf("Failed to read direction register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial direction register: 0x%03lX\n", direction);

  err = ch422g_read_output_reg(ch422g_dev, &output_value);
  if (err != ESP_OK) {
    printf("Failed to read output register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial output register: 0x%03lX\n", output_value);

  direction &= ~EXIO1_BIT;
  output_value |= EXIO1_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  direction &= ~EXIO4_BIT;
  output_value &= ~EXIO4_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  printf("Final direction register: 0x%03lX\n", direction);
  printf("Final output register: 0x%03lX\n", output_value);

  cleanup_resources(ch422g_dev, I2C_NUM);
#endif

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ENCODER_INA)
  host.max_freq_khz = 4000;       /* 4 MHz for first probe – increase later if needed */
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  host.max_freq_khz = 4000;       /* 4 MHz for ESP32-C5 to avoid timeout issues */
#endif
  /* select spi host slot for target */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
  host.slot = SPI2_HOST;
#endif

  spi_bus_config_t bus_config;

  memset(&bus_config, 0, sizeof(spi_bus_config_t));

  bus_config.miso_io_num = sd_card_manager.spi_miso_pin;
  bus_config.mosi_io_num = sd_card_manager.spi_mosi_pin;
  bus_config.sclk_io_num = sd_card_manager.spi_clk_pin;
  /* reduce dma pressure for sd spi */
  bus_config.max_transfer_sz = 8192;

#ifdef CONFIG_IDF_TARGET_ESP32
  int dmabus = 2;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  int dmabus = SPI_DMA_CH_AUTO;
#else
  int dmabus = SPI_DMA_CH_AUTO;
#endif

  bool bus_init_success = false;

#if defined(CONFIG_IDF_TARGET_ESP32C5)
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      s_spi_bus_initialized = true;
      s_spi_host_id = SPI2_HOST;
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#elif !defined(CONFIG_USE_TDECK)
#if !defined(CONFIG_ENCODER_INA)
#if defined(CONFIG_IDF_TARGET_ESP32)
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI3_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      s_spi_bus_initialized = true;
      s_spi_host_id = SPI2_HOST;
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#else
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      s_spi_bus_initialized = true;
      s_spi_host_id = SPI2_HOST;
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#endif
#endif
#endif

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 4 * 1024};

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_card_manager.spi_cs_pin;
#if defined(CONFIG_IDF_TARGET_ESP32)
  slot_config.host_id = SPI3_HOST;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#if defined(CONFIG_ENCODER_INA)
  slot_config.host_id = SPI3_HOST; // use spi3_host (vspi) for sd if encoder is active on esp32s3
#else
  slot_config.host_id = SPI2_HOST;
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  slot_config.host_id = SPI2_HOST;
#else
  slot_config.host_id = SPI2_HOST;
#endif

  ret = esp_vfs_fat_sdspi_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    printf("Failed to mount filesystem: %s\n", esp_err_to_name(ret));
    if (bus_init_success) {
      if (s_spi_bus_initialized && s_spi_host_id >= 0) {
        spi_bus_free(s_spi_host_id);
        s_spi_bus_initialized = false;
        s_spi_host_id = -1;
      }
    }
    if (display_was_suspended) {
      display_spi_resume_after_sd();
    }
    return ret;
  }

  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SPI;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully in SPI mode.\n");

  sd_card_setup_directory_structure();

  if (gating_template) {
    sd_card_update_cached_stats();
    sd_card_unmount();
    if (display_was_suspended) {
      display_spi_resume_after_sd();
    }
    return ESP_OK;
  }

#endif

  // Common failure handling
  if (ret != ESP_OK) {
      // Restore backup config if init failed with loaded pins
      sd_card_manager = backup_config;
      printf("SD Card init failed with loaded pins. Check configuration.\n");
      // Optionally: attempt init with known defaults here as a fallback?
      return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);
  printf("SD card initialized successfully\n");

  sd_card_setup_directory_structure();

  return ESP_OK;
}

// mount sd just-in-time for short io, then unmount after
esp_err_t sd_card_mount_for_flush(bool *display_was_suspended) {
  if (display_was_suspended) *display_was_suspended = false;
  // If already mounted, nothing to do
  if (sd_card_manager.is_initialized) {
    return ESP_OK;
  }

#if defined(CONFIG_USING_SPI)
  // always pause display SPI if the display shares the same SPI bus with SD
  if (display_was_suspended) *display_was_suspended = display_spi_suspend_for_sd();
  // Minimal SPI mount path for flush: reuse sd_card_init SPI branch logic
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32C5)
  host.slot = SPI2_HOST;
  host.max_freq_khz = 4000;       /* 4 MHz for ESP32-C5 to avoid timeout issues */
#endif

  spi_bus_config_t bus_config; memset(&bus_config, 0, sizeof(spi_bus_config_t));
  bus_config.miso_io_num = sd_card_manager.spi_miso_pin;
  bus_config.mosi_io_num = sd_card_manager.spi_mosi_pin;
  bus_config.sclk_io_num = sd_card_manager.spi_clk_pin;
  bus_config.max_transfer_sz = 8192;

#if defined(CONFIG_IDF_TARGET_ESP32)
  int dmabus = 2;
#else
  int dmabus = SPI_DMA_CH_AUTO;
#endif

  if (!s_spi_bus_initialized) {
    int host_id =
#if defined(CONFIG_IDF_TARGET_ESP32)
      SPI3_HOST;
#else
      SPI2_HOST;
#endif
    esp_err_t bus_ret = spi_bus_initialize(host_id, &bus_config, dmabus);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
      if (display_was_suspended && *display_was_suspended) display_spi_resume_after_sd();
      return bus_ret;
    }
    s_spi_bus_initialized = true;
    s_spi_host_id = host_id;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
      .allocation_unit_size = 4 * 1024};

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_card_manager.spi_cs_pin;
#if defined(CONFIG_IDF_TARGET_ESP32)
  slot_config.host_id = SPI3_HOST;
#else
  slot_config.host_id = SPI2_HOST;
#endif

  esp_err_t ret = esp_vfs_fat_sdspi_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (s_spi_bus_initialized && s_spi_host_id >= 0) {
      spi_bus_free(s_spi_host_id);
      s_spi_bus_initialized = false;
      s_spi_host_id = -1;
    }
    if (display_was_suspended && *display_was_suspended) display_spi_resume_after_sd();
    return ret;
  }
  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SPI;
  sd_card_update_cached_stats();
  s_next_unmount_tick = xTaskGetTickCount() + pdMS_TO_TICKS(300);
  return ESP_OK;
#else
  // For SDMMC, if not mounted try normal init path quickly
  return sd_card_init();
#endif
}

void sd_card_unmount_after_flush(bool display_was_suspended) {
  /* fuck it, unmount now so the display can safely resume without bus contention */
  if (sd_card_manager.is_initialized) {
    sd_card_unmount();
  }
  /* always attempt resume; it's idempotent and guards internally */
  display_spi_resume_after_sd();
}

void sd_card_unmount(void) {
#ifdef CONFIG_IS_S3TWATCH
  if (s_virtual_storage_mounted) {
    unmount_virtual_storage();
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
    status_display_show_status("SD Unmounted");
    return;
  }
#endif

#if SOC_SDMMC_HOST_SUPPORTED && SOC_SDMMC_USE_GPIO_MATRIX
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdcard_unmount("/mnt", sd_card_manager.card);
    printf("SD card unmounted\n");
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
    status_display_show_status("SD Unmounted");
  } else {
    status_display_show_status("SD Not Mounted");
  }
#else
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdcard_unmount("/mnt", sd_card_manager.card);
    if (s_spi_bus_initialized && s_spi_host_id >= 0) {
      spi_bus_free(s_spi_host_id);
      s_spi_bus_initialized = false;
      s_spi_host_id = -1;
    }
    printf("SD card unmounted\n");
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
    status_display_show_status("SD Unmounted");
  } else {
    status_display_show_status("SD Not Mounted");
  }
#endif
}

esp_err_t sd_card_append_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot append to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "ab");
  if (f == NULL) {
    printf("Failed to open file for appending\n");
    return ESP_FAIL;
  }
  fwrite(data, 1, size, f);
  fclose(f);
  printf("Data appended to file: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot write to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    printf("Failed to open file for writing\n");
    return ESP_FAIL;
  }
  fwrite(data, 1, size, f);
  fclose(f);
  printf("File written: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot read from file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    printf("Failed to open file for reading\n");
    return ESP_FAIL;
  }
  char line[64];
  while (fgets(line, sizeof(line), f) != NULL) {
    printf("%s", line);
  }
  fclose(f);
  printf("File read: %s\n", path);
  return ESP_OK;
}

static bool has_full_permissions(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if ((st.st_mode & 0777) == 0777) {
      return true;
    }
  }
  return false;
}

esp_err_t sd_card_create_directory(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot create directory.\n");
    return ESP_FAIL;
  }

  if (sd_card_exists(path)) {
    printf("Directory already exists: %s\n", path);

    if (!has_full_permissions(path)) {
      printf("Directory %s does not have full permissions. Deleting and "
             "recreating.\n",
             path);

      if (rmdir(path) != 0) {
        printf("Failed to remove directory: %s\n", path);
        return ESP_FAIL;
      }

      int res = mkdir(path, 0777);
      if (res != 0) {
        printf("Failed to create directory: %s\n", path);
        return ESP_FAIL;
      }

      printf("Directory created: %s\n", path);

    } else {
      printf("Directory %s has correct permissions.\n", path);
      return ESP_OK;
    }
    return ESP_OK;
  }

  int res = mkdir(path, 0777);
  if (res != 0) {
    printf("Failed to create directory: %s\n", path);
    return ESP_FAIL;
  }

  printf("Directory created: %s\n", path);
  return ESP_OK;
}

bool sd_card_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return true;
  } else {
    return false;
  }
}

static esp_err_t ensure_sd_dir_exists(const char *path) {
  if (!sd_card_exists(path)) {
    printf("Creating directory: %s\n", path);
    esp_err_t ret = sd_card_create_directory(path);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", path, esp_err_to_name(ret));
      return ret;
    }
  } else {
    printf("Directory %s already exists\n", path);
  }
  return ESP_OK;
}

esp_err_t sd_card_setup_directory_structure() {
  const char *root_dir = "/mnt/ghostesp";
  const char *debug_dir = "/mnt/ghostesp/debug";
  const char *pcaps_dir = "/mnt/ghostesp/pcaps";
  const char *scans_dir = "/mnt/ghostesp/scans";
  const char *gps_dir = "/mnt/ghostesp/gps";
  const char *games_dir = "/mnt/ghostesp/games";
  const char *evil_portal_dir = "/mnt/ghostesp/evil_portal";
  const char *evil_portal_portals_dir = "/mnt/ghostesp/evil_portal/portals"; 
  const char *universals_dir = "/mnt/ghostesp/infrared/universals";
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
  const char *nfc_dir = "/mnt/ghostesp/nfc";
#endif

  esp_err_t ret = ensure_sd_dir_exists(root_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(games_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(gps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(debug_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(pcaps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(scans_dir);
  if (ret != ESP_OK) return ret;

  // Create evil_portal directory
  ret = ensure_sd_dir_exists(evil_portal_dir);
  if (ret != ESP_OK) return ret;

  // Create evil_portal/portals directory
  ret = ensure_sd_dir_exists(evil_portal_portals_dir);
  if (ret != ESP_OK) return ret;

  const char *infrared_dir = "/mnt/ghostesp/infrared";
  ret = ensure_sd_dir_exists(infrared_dir);
  if (ret != ESP_OK) return ret;

  const char *remotes_dir = "/mnt/ghostesp/infrared/remotes";
  ret = ensure_sd_dir_exists(remotes_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(universals_dir);
  if (ret != ESP_OK) return ret;

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
  ret = ensure_sd_dir_exists(nfc_dir);
  if (ret != ESP_OK) return ret;
#endif

  printf("Directory structure successfully set up.\n");
  return ESP_OK;
}

// New SD card pin configuration functions

esp_err_t sd_card_set_mmc_pins(int clk, int cmd, int d0, int d1, int d2, int d3) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.clkpin = clk;
  sd_card_manager.cmdpin = cmd;
  sd_card_manager.d0pin = d0;
  sd_card_manager.d1pin = d1;
  sd_card_manager.d2pin = d2;
  sd_card_manager.d3pin = d3;
  
  printf("SD card MMC pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_set_spi_pins(int cs, int clk, int miso, int mosi) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.spi_cs_pin = cs;
  sd_card_manager.spi_clk_pin = clk;
  sd_card_manager.spi_miso_pin = miso;
  sd_card_manager.spi_mosi_pin = mosi;
  
  printf("SD card SPI pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_save_config() {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    return err;
  }

  // Write MMC pins
  err = nvs_set_i32(nvs_handle, "mmc_clk", sd_card_manager.clkpin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_cmd", sd_card_manager.cmdpin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d0", sd_card_manager.d0pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d1", sd_card_manager.d1pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d2", sd_card_manager.d2pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d3", sd_card_manager.d3pin);
  if (err != ESP_OK) goto nvs_write_error;

  // Write SPI pins
  err = nvs_set_i32(nvs_handle, "spi_cs", sd_card_manager.spi_cs_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_clk", sd_card_manager.spi_clk_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_miso", sd_card_manager.spi_miso_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_mosi", sd_card_manager.spi_mosi_pin);
  if (err != ESP_OK) goto nvs_write_error;

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) committing NVS changes!\n", esp_err_to_name(err));
  } else {
    printf("SD card pin configuration saved to NVS.\n");
  }

  // Close NVS handle and return
  nvs_close(nvs_handle);
  return err; // Return the result of nvs_commit or nvs_open

  // error handling label for write failures (kept inside function scope)
nvs_write_error:
  printf("Error (%s) writing NVS key!\n", esp_err_to_name(err));
  nvs_close(nvs_handle);
  return err;
}

esp_err_t sd_card_load_config() {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("NVS namespace '%s' not found. Using default SD pins.\n", NVS_NAMESPACE);
        // Keep default pins already set in sd_card_manager struct definition
        return ESP_OK; // Not an error if first boot
    } else {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return err;
    }
  }

  int32_t temp_val;

  // Read MMC pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "mmc_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.clkpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_cmd", &temp_val);
  if (err == ESP_OK) sd_card_manager.cmdpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d0", &temp_val);
  if (err == ESP_OK) sd_card_manager.d0pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d1", &temp_val);
  if (err == ESP_OK) sd_card_manager.d1pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d2", &temp_val);
  if (err == ESP_OK) sd_card_manager.d2pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d3", &temp_val);
  if (err == ESP_OK) sd_card_manager.d3pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Read SPI pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "spi_cs", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_cs_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_clk_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_miso", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_miso_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_mosi", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_mosi_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Success path
  printf("SD card pin configuration loaded from NVS.\n");
  nvs_close(nvs_handle);
  return ESP_OK;

read_error:
  printf("Error (%s) reading NVS key! Using default SD pins.\n", esp_err_to_name(err));
  nvs_close(nvs_handle);
  // Keep default pins already set in sd_card_manager struct definition
  return err; // Return the actual read error
}

void sd_card_print_config() {
#ifdef CONFIG_IS_S3TWATCH
  if (s_virtual_storage_mounted) {
    printf("Storage Configuration: Virtual Flash Storage (S3TWatch)\n");
    printf("Mount Point: /mnt\n");
    printf("Storage Type: Internal Flash Partition (4MB)\n");
    
    const esp_partition_t* storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (storage_partition) {
      printf("Partition Size: %lu KB\n", (unsigned long)(storage_partition->size / 1024));
      printf("Partition Offset: 0x%lx\n", (unsigned long)storage_partition->address);
    }
    return;
  }
#endif

  printf("SD Card Pin Configuration:\n");
  printf("MMC Mode:\n");
  printf("  CLK: GPIO%d\n", sd_card_manager.clkpin);
  printf("  CMD: GPIO%d\n", sd_card_manager.cmdpin);
  printf("  D0:  GPIO%d\n", sd_card_manager.d0pin);
  printf("  D1:  GPIO%d\n", sd_card_manager.d1pin);
  printf("  D2:  GPIO%d\n", sd_card_manager.d2pin);
  printf("  D3:  GPIO%d\n", sd_card_manager.d3pin);
  printf("SPI Mode:\n");
  printf("  CS:   GPIO%d\n", sd_card_manager.spi_cs_pin);
  printf("  CLK:  GPIO%d\n", sd_card_manager.spi_clk_pin);
  printf("  MISO: GPIO%d\n", sd_card_manager.spi_miso_pin);
  printf("  MOSI: GPIO%d\n", sd_card_manager.spi_mosi_pin);
}

bool sd_card_is_virtual_storage() {
#ifdef CONFIG_IS_S3TWATCH
  return s_virtual_storage_mounted;
#else
  return false;
#endif
}

int get_evil_portal_list(char portal_names[MAX_PORTALS][MAX_PORTAL_NAME]) {
    const char *portal_dir = "/mnt/ghostesp/evil_portal/portals";
    DIR *dir = opendir(portal_dir);
    if (!dir){
        ESP_LOGW(TAG, "Failed to open directory: %s\n", portal_dir);
        return -1; // Return -1 if directory cannot be opened
    }
    ESP_LOGI(TAG, "Listing portals in directory: %s\n", portal_dir);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) && count < MAX_PORTALS) {
        bool is_reg = false;
        if (entry->d_type == DT_REG) {
            is_reg = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            // fallback to stat when d_type is unknown
            char fullpath[256];
            int written = snprintf(fullpath, sizeof(fullpath), "%s/%s", portal_dir, entry->d_name);
            if (written > 0 && written < (int)sizeof(fullpath)) {
                struct stat st;
                if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
                    is_reg = true;
                }
            }
        }

        if (is_reg) {
            const char *dot = strrchr(entry->d_name, '.');
            if (dot && strcmp(dot, ".html") == 0) {
                strncpy(portal_names[count], entry->d_name, MAX_PORTAL_NAME - 1);
                portal_names[count][MAX_PORTAL_NAME - 1] = '\0';
                count++;
            }
        }
    }
    closedir(dir);
    return count;
}