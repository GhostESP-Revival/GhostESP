#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_err.h"
#include <stdbool.h>

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

// SD card unmount context types
typedef enum {
    SD_UNMOUNT_CONTEXT_USER = 0,    // User-initiated unmount
    SD_UNMOUNT_CONTEXT_JIT,         // JIT unmount after operation
    SD_UNMOUNT_CONTEXT_ERROR,       // Unmount due to error
    SD_UNMOUNT_CONTEXT_SHUTDOWN     // System shutdown
} sd_unmount_context_t;

typedef struct {
  sdmmc_card_t *card;
  bool is_initialized;
  int clkpin;
  int cmdpin;
  int d0pin;
  int d1pin;
  int d2pin;
  int d3pin;

  // SPI
  int spi_cs_pin;
  int spi_miso_pin;
  int spi_mosi_pin;
  int spi_clk_pin;
} sd_card_manager_t;

extern sd_card_manager_t sd_card_manager;

esp_err_t sd_card_init();
void sd_card_unmount();
void sd_card_unmount_with_context(sd_unmount_context_t context);
esp_err_t sd_card_append_file(const char *path, const void *data, size_t size);
esp_err_t sd_card_write_file(const char *path, const void *data, size_t size);
esp_err_t sd_card_read_file(const char *path);
esp_err_t sd_card_create_directory(const char *path);
bool sd_card_exists(const char *path);
esp_err_t sd_card_setup_directory_structure();

// New functions for SD card pin configuration
esp_err_t sd_card_set_mmc_pins(int clk, int cmd, int d0, int d1, int d2, int d3);
esp_err_t sd_card_set_spi_pins(int cs, int clk, int miso, int mosi);
esp_err_t sd_card_save_config();
esp_err_t sd_card_load_config();
void sd_card_print_config();
bool sd_card_is_virtual_storage();

// mount sd just-in-time for short io, then unmount after
esp_err_t sd_card_mount_for_flush(bool *display_was_suspended);
void sd_card_unmount_after_flush(bool display_was_suspended);

// cached SD stats for HUD (updated during mount operations)
typedef struct {
    bool valid;
    int used_pct;
} sd_card_cached_stats_t;
void sd_card_get_cached_stats(sd_card_cached_stats_t *out);

// List evil portal directories from SD card
int get_evil_portal_list(char portal_names[MAX_PORTALS][MAX_PORTAL_NAME]);

#endif // SD_CARD_MANAGER_H