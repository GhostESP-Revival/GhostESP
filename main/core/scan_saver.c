#include "core/scan_saver.h"
#include "core/utils.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

#define SCANS_DIR "/mnt/ghostesp/scans"
#define FLUSH_INTERVAL 16

static bool try_jit_mount(scan_file_t *sf) {
    bool disp_suspended = false;
    if (sd_card_mount_for_flush(&disp_suspended) == ESP_OK) {
        sf->jit_mounted = true;
        sf->display_suspended = disp_suspended;
        return true;
    }
    if (disp_suspended) {
        sd_card_unmount_after_flush(disp_suspended);
    }
    return false;
}

static void ensure_scans_dir(void) {
    struct stat st;
    if (stat(SCANS_DIR, &st) != 0) {
        mkdir(SCANS_DIR, 0777);
    }
}

esp_err_t scan_file_open_ex(scan_file_t *sf, const char *prefix, const char *extension, bool force_save) {
    if (!sf || !prefix || !extension) return ESP_ERR_INVALID_ARG;
    if (!force_save && !settings_get_auto_save_scans(&G_Settings)) return ESP_ERR_NOT_SUPPORTED;

    if (sf->fp) {
        fclose(sf->fp);
        sf->fp = NULL;
    }

    sf->fp = NULL;
    sf->jit_mounted = false;
    sf->display_suspended = false;
    sf->write_count = 0;
    sf->path[0] = '\0';

    if (!sd_card_manager.is_initialized) {
        if (!try_jit_mount(sf)) return ESP_ERR_NOT_FOUND;
    }

    ensure_scans_dir();

    int idx = get_next_file_index(SCANS_DIR, prefix, extension);
    if (idx < 0) idx = 0;

    snprintf(sf->path, sizeof(sf->path), "%s/%s_%d.%s", SCANS_DIR, prefix, idx, extension);

    sf->fp = fopen(sf->path, "w");
    if (!sf->fp) {
        sf->path[0] = '\0';
        if (sf->jit_mounted) {
            sd_card_unmount_after_flush(sf->display_suspended);
            sf->jit_mounted = false;
        }
        return ESP_FAIL;
    }

    printf("Scan file opened: %s\n", sf->path);
    return ESP_OK;
}

esp_err_t scan_file_open(scan_file_t *sf, const char *prefix, const char *extension) {
    return scan_file_open_ex(sf, prefix, extension, false);
}

void scan_file_printf(scan_file_t *sf, const char *fmt, ...) {
    if (!sf || !sf->fp) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(sf->fp, fmt, args);
    va_end(args);
    if (++sf->write_count % FLUSH_INTERVAL == 0) {
        fflush(sf->fp);
    }
}

void scan_file_close(scan_file_t *sf) {
    if (!sf) return;
    if (sf->fp) {
        fflush(sf->fp);
        fclose(sf->fp);
        sf->fp = NULL;
        printf("Scan file saved\n");
    }
    if (sf->jit_mounted) {
        sd_card_unmount_after_flush(sf->display_suspended);
        sf->jit_mounted = false;
    }
    sf->write_count = 0;
}
