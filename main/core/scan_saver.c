#include "core/scan_saver.h"
#include "core/memory_debug.h"
#include "core/utils.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "gui/toast.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SCANS_DIR "/mnt/ghostesp/scans"
#define FLUSH_INTERVAL 16
#define BUFFERED_FLUSH_INTERVAL 64
#if CONFIG_SPIRAM
#define SCAN_BUFFER_CAPACITY (16 * 1024)
#else
#define SCAN_BUFFER_CAPACITY (2 * 1024)
#endif

static bool try_jit_mount(scan_file_t *sf) {
    if (sd_card_needs_jit_mount()) {
        bool disp_suspended = false;
        if (sd_card_mount_for_flush(&disp_suspended) == ESP_OK) {
            sf->jit_mounted = true;
            sf->display_suspended = disp_suspended;
            return true;
        }
        if (disp_suspended) sd_card_unmount_after_flush(disp_suspended);
    }
    return false;
}

static void ensure_scans_dir(void) {
    struct stat st;
    if (stat(SCANS_DIR, &st) != 0) mkdir(SCANS_DIR, 0777);
}

static bool append_buffered_data(scan_file_t *sf, const char *data, size_t len) {
    if (!sf || !data || len == 0 || sf->path[0] == '\0') return false;

    bool display_suspended = false;
    if (sd_card_mount_for_flush(&display_suspended) != ESP_OK) return false;

    ensure_scans_dir();
    FILE *fp = fopen(sf->path, "a");
    bool ok = fp != NULL;
    if (fp) {
        ok = fwrite(data, 1, len, fp) == len;
        fflush(fp);
        fclose(fp);
    }

    sd_card_unmount_after_flush(display_suspended);
    return ok;
}

static bool flush_staging_buffer(scan_file_t *sf) {
    if (!sf || !sf->buffered || sf->buffer_len == 0) return true;
    if (!append_buffered_data(sf, sf->buffer, sf->buffer_len)) return false;
    sf->buffer_len = 0;
    sf->write_count = 0;
    return true;
}

bool scan_file_is_open(const scan_file_t *sf) {
    return sf && (sf->fp != NULL || sf->buffered);
}

esp_err_t scan_file_open(scan_file_t *sf, const char *prefix, const char *extension) {
    if (!sf || !prefix || !extension) return ESP_ERR_INVALID_ARG;
#ifdef CONFIG_IS_S3TWATCH
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!settings_get_auto_save_scans(&G_Settings)) return ESP_ERR_NOT_SUPPORTED;

    if (scan_file_is_open(sf)) scan_file_close(sf);

    sf->fp = NULL;
    sf->jit_mounted = false;
    sf->display_suspended = false;
    sf->buffered = false;
    sf->write_count = 0;
    sf->buffer = NULL;
    sf->buffer_len = 0;
    sf->buffer_capacity = 0;
    sf->path[0] = '\0';

    if (sd_card_needs_jit_mount()) {
        if (!try_jit_mount(sf)) return ESP_ERR_NOT_FOUND;
    } else if (!sd_card_manager.is_initialized) {
        return ESP_ERR_NOT_FOUND;
    }

    ensure_scans_dir();

    int idx = get_next_file_index(SCANS_DIR, prefix, extension);
    if (idx < 0) idx = 0;
    snprintf(sf->path, sizeof(sf->path), "%s/%s_%d.%s", SCANS_DIR, prefix, idx, extension);

    sf->fp = fopen(sf->path, "w");
    if (!sf->fp) {
        if (sf->jit_mounted) {
            sd_card_unmount_after_flush(sf->display_suspended);
            sf->jit_mounted = false;
        }
        sf->path[0] = '\0';
        return ESP_FAIL;
    }

    if (sf->jit_mounted) {
        /* A JIT mount may suspend the shared display SPI bus. Create the file,
         * resume the display, and stage scan output until short flush windows. */
        fclose(sf->fp);
        sf->fp = NULL;
        sd_card_unmount_after_flush(sf->display_suspended);
        sf->jit_mounted = false;
        sf->display_suspended = false;

        sf->buffer = spiram_malloc(SCAN_BUFFER_CAPACITY + 1);
        if (!sf->buffer) {
            sf->path[0] = '\0';
            return ESP_ERR_NO_MEM;
        }
        sf->buffer_capacity = SCAN_BUFFER_CAPACITY;
        sf->buffered = true;
    }

    printf("Scan file opened: %s%s\n", sf->path, sf->buffered ? " (buffered)" : "");
    return ESP_OK;
}

void scan_file_printf(scan_file_t *sf, const char *fmt, ...) {
    if (!scan_file_is_open(sf) || !fmt) return;

    va_list args;
    va_start(args, fmt);

    if (sf->fp) {
        vfprintf(sf->fp, fmt, args);
        va_end(args);
        if (++sf->write_count % FLUSH_INTERVAL == 0) fflush(sf->fp);
        return;
    }

    va_list count_args;
    va_copy(count_args, args);
    int needed = vsnprintf(NULL, 0, fmt, count_args);
    va_end(count_args);
    if (needed <= 0) {
        va_end(args);
        return;
    }

    size_t needed_size = (size_t)needed;
    if (needed_size > sf->buffer_capacity) {
        char *line = spiram_malloc(needed_size + 1);
        if (line) {
            vsnprintf(line, needed_size + 1, fmt, args);
            if (flush_staging_buffer(sf)) append_buffered_data(sf, line, needed_size);
            free(line);
        }
        va_end(args);
        return;
    }

    if (sf->buffer_len + needed_size > sf->buffer_capacity && !flush_staging_buffer(sf)) {
        va_end(args);
        return;
    }

    vsnprintf(sf->buffer + sf->buffer_len,
              sf->buffer_capacity - sf->buffer_len + 1, fmt, args);
    sf->buffer_len += needed_size;
    va_end(args);

    if (++sf->write_count >= BUFFERED_FLUSH_INTERVAL) flush_staging_buffer(sf);
}

void scan_file_close(scan_file_t *sf) {
    if (!sf) return;
    bool saved = false;

    if (sf->fp) {
        fflush(sf->fp);
        fclose(sf->fp);
        sf->fp = NULL;
        saved = true;
    }

    if (sf->buffered) {
        saved = flush_staging_buffer(sf);
        free(sf->buffer);
        sf->buffer = NULL;
        sf->buffered = false;
        sf->buffer_len = 0;
        sf->buffer_capacity = 0;
    }

    if (sf->jit_mounted) {
        sd_card_unmount_after_flush(sf->display_suspended);
        sf->jit_mounted = false;
    }

    sf->display_suspended = false;
    sf->write_count = 0;
    sf->path[0] = '\0';

    if (saved) {
        printf("Scan file saved\n");
        toast_show("Scan saved", TOAST_SUCCESS);
    }
}
