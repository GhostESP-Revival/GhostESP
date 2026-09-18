#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_err.h"
#include <stdbool.h>

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

/* Public SD-card storage contract. Keep these paths stable: they are exposed
 * through the WebUI, CLI, documentation, and native-app storage APIs. */
#define SD_MOUNT_POINT "/mnt"
#define SD_GHOSTESP_ROOT SD_MOUNT_POINT "/ghostesp"
#define SD_STORAGE_LAYOUT_VERSION 1

#define SD_DIR_LOGS SD_GHOSTESP_ROOT "/logs"
#define SD_DIR_COREDUMPS SD_DIR_LOGS "/coredumps"
#define SD_DIR_DEBUG SD_GHOSTESP_ROOT "/debug"
#define SD_DIR_PCAPS SD_GHOSTESP_ROOT "/pcaps"
#define SD_DIR_CAPTURES SD_GHOSTESP_ROOT "/captures"
#define SD_DIR_SCANS SD_GHOSTESP_ROOT "/scans"
#define SD_DIR_SWEEPS SD_GHOSTESP_ROOT "/sweeps"
#define SD_DIR_GPS SD_GHOSTESP_ROOT "/gps"
#define SD_DIR_GHOSTCHI SD_GHOSTESP_ROOT "/ghostchi"
#define SD_DIR_GHOSTCHI_PCAPS SD_DIR_GHOSTCHI "/pcaps"
#define SD_DIR_GHOSTCHI_SESSIONS SD_DIR_GHOSTCHI "/sessions"
#define SD_DIR_APPS SD_GHOSTESP_ROOT "/apps"
#define SD_DIR_APP_CACHE SD_GHOSTESP_ROOT "/app_cache"
#define SD_DIR_APPDATA SD_GHOSTESP_ROOT "/appdata"
#define SD_DIR_SCRIPTS SD_GHOSTESP_ROOT "/scripts"
#define SD_DIR_SCRIPTDATA SD_GHOSTESP_ROOT "/scriptdata"
#define SD_DIR_THEMES SD_GHOSTESP_ROOT "/themes"
#define SD_DIR_DOWNLOADS SD_GHOSTESP_ROOT "/downloads"
#define SD_DIR_COMICS SD_GHOSTESP_ROOT "/comics"

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

/*
 * Returns true when the active build keeps the SD card unmounted after
 * init and individual callers need to mount on demand. Returns false when
 * the SD is mounted once at boot and stays accessible.
 */
bool sd_card_needs_jit_mount(void);

/*
 * Returns true when SD file I/O contends with the display on one SPI host.
 * Streaming callers can stage writes in RAM to avoid a small SD transaction
 * for every received network chunk.
 */
bool sd_card_uses_shared_display_spi(void);

/*
 * sd_card_jit_begin / sd_card_jit_end wrap the JIT mount/unmount pattern
 * used by views and managers that may run on builds without a permanent SD
 * mount. On builds where the SD card stays mounted after init, these are
 * no-ops and return true.
 *
 * Use:
 *   bool display_was_suspended = false;
 *   if (!sd_card_jit_begin(&display_was_suspended, false)) { handle_error(); }
 *   ...file I/O...
 *   sd_card_jit_end(display_was_suspended);
 *
 * Pass ensure_dirs=true to also call sd_card_setup_directory_structure()
 * after a successful mount (needed by audio / IR / GhostChi flows that
 * expect the directory tree to exist).
 */
bool sd_card_jit_begin(bool *display_was_suspended, bool ensure_dirs);
void sd_card_jit_end(bool display_was_suspended);

// cached SD stats for HUD (updated during mount operations)
typedef struct {
    bool valid;
    int used_pct;
} sd_card_cached_stats_t;
void sd_card_get_cached_stats(sd_card_cached_stats_t *out);

/*
 * USB MSC passthrough support. While the SD card is handed to the USB host
 * (mass-storage mode), the card's host controller stays initialized but the
 * FatFS VFS at SD_MOUNT_POINT is released. Callers must treat the card as
 * unavailable and re-mount through sd_card_resume_from_usb_msc() when done.
 */
esp_err_t sd_card_suspend_for_usb_msc(sdmmc_card_t **out_card);
esp_err_t sd_card_resume_from_usb_msc(void);
bool sd_card_usb_msc_active(void);

// List evil portal directories from SD card (legacy — capped at MAX_PORTALS)
int get_evil_portal_list(char portal_names[MAX_PORTALS][MAX_PORTAL_NAME]);

/**
 * sd_card_list_dir_paged - generic paginated directory listing.
 *
 * Opens @dir_path, filters entries by @ext (e.g. ".html", NULL = all files),
 * skips the first @offset matching entries, then copies up to @max_count
 * filenames into @out_names.  Each element of @out_names is a char array of
 * MAX_PORTAL_NAME bytes (same width used throughout the portal pipeline).
 *
 * If @out_has_more is non-NULL it is set to true when at least one additional
 * matching entry exists beyond the returned page, allowing callers to show a
 * "Next" navigation item without a separate full-directory count pass.
 *
 * Returns the number of entries written to @out_names, or -1 on error.
 * Reusable for any SD directory (BadUSB scripts, IR remotes, NFC files, …).
 */
int sd_card_list_dir_paged(const char *dir_path, const char *ext,
                            int offset, int max_count,
                            char (*out_names)[MAX_PORTAL_NAME],
                            bool *out_has_more);

#endif // SD_CARD_MANAGER_H
