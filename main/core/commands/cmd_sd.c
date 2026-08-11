// cmd_sd.c
// SD card file-system and configuration commands.

#include "core/commands.h"
#include "core/glog.h"
#include "esp_vfs_fat.h"
#include "managers/sd_card_manager.h"
#include "managers/status_display_manager.h"
#include "sdkconfig.h"
#include "mbedtls/base64.h"
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void handle_sd_config(int argc, char **argv) {
  sd_card_print_config();
  status_display_show_status("SD Config");
}

void handle_sd_pins_mmc(int argc, char **argv) {
  if (argc != 7) {
    glog("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    glog("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
    glog("Example: sd_pins_mmc 19 18 20 21 22 23\n");
    status_display_show_status("SD MMC Usage");
    return;
  }
  
  int clk = atoi(argv[1]);
  int cmd = atoi(argv[2]);
  int d0 = atoi(argv[3]);
  int d1 = atoi(argv[4]);
  int d2 = atoi(argv[5]);
  int d3 = atoi(argv[6]);
  
  if (clk < 0 || cmd < 0 || d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 ||
      clk > 40 || cmd > 40 || d0 > 40 || d1 > 40 || d2 > 40 || d3 > 40) {
    glog("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    status_display_show_status("Pins Invalid");
    return;
  }
  
  sd_card_set_mmc_pins(clk, cmd, d0, d1, d2, d3);
  status_display_show_status("SD MMC Set");
}

void handle_sd_pins_spi(int argc, char **argv) {
  if (argc != 5) {
    glog("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    glog("Sets pins for SPI mode (only effective if compiled for SPI).\n");
    glog("Example: sd_pins_spi 5 18 19 23\n");
    status_display_show_status("SD SPI Usage");
    return;
  }
  
  int cs = atoi(argv[1]);
  int clk = atoi(argv[2]);
  int miso = atoi(argv[3]);
  int mosi = atoi(argv[4]);
  
  if (cs < 0 || clk < 0 || miso < 0 || mosi < 0 ||
      cs > 40 || clk > 40 || miso > 40 || mosi > 40) {
    glog("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    status_display_show_status("Pins Invalid");
    return;
  }
  
  sd_card_set_spi_pins(cs, clk, miso, mosi);
  status_display_show_status("SD SPI Set");
}

void handle_sd_save_config(int argc, char **argv) {
  sd_card_save_config();
  status_display_show_status("SD Saved");
}

#define SD_CLI_MAX_ENTRIES 128
static char *g_sd_cli_paths[SD_CLI_MAX_ENTRIES];
static uint8_t g_sd_cli_types[SD_CLI_MAX_ENTRIES];
static size_t g_sd_cli_count = 0;

static void sd_cli_clear_index(void) {
    for (size_t i = 0; i < g_sd_cli_count; ++i) {
        free(g_sd_cli_paths[i]);
        g_sd_cli_paths[i] = NULL;
    }
    g_sd_cli_count = 0;
}

static bool sd_cli_is_number(const char *s) {
    if (!s || !*s) return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

static const char *sd_cli_resolve_path(const char *arg, char *buf, size_t bufsize) {
    if (sd_cli_is_number(arg) && g_sd_cli_count > 0) {
        int idx = atoi(arg);
        if (idx >= 0 && (size_t)idx < g_sd_cli_count) {
            strncpy(buf, g_sd_cli_paths[idx], bufsize - 1);
            buf[bufsize - 1] = '\0';
            return buf;
        }
        return NULL;
    }
    if (arg[0] == '/') {
        strncpy(buf, arg, bufsize - 1);
    } else {
        snprintf(buf, bufsize, "/mnt/ghostesp/%s", arg);
    }
    buf[bufsize - 1] = '\0';
    return buf;
}

static bool sd_cli_jit_mounted = false;
static bool sd_cli_display_suspended = false;

static bool sd_cli_ensure_mounted(void) {
    if (sd_card_manager.is_initialized) return true;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (sd_card_mount_for_flush(&sd_cli_display_suspended) == ESP_OK) {
            sd_cli_jit_mounted = true;
            return true;
        }
    }
#endif
    return false;
}

static void sd_cli_cleanup(void) {
    for (size_t i = 0; i < g_sd_cli_count; i++) {
        free(g_sd_cli_paths[i]);
        g_sd_cli_paths[i] = NULL;
    }
    g_sd_cli_count = 0;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (sd_cli_jit_mounted) {
        sd_card_unmount_after_flush(sd_cli_display_suspended);
        sd_cli_jit_mounted = false;
        sd_cli_display_suspended = false;
    }
#endif
}

void handle_sd_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("SD:USAGE\n");
        glog("  sd status                        - Show SD card status\n");
        glog("  sd list [path]                   - List files/dirs with indices\n");
        glog("  sd info <idx|path>               - Show file/dir info\n");
        glog("  sd size <idx|path>               - Get file size\n");
        glog("  sd read <idx|path> [off] [len] [--base64] - Read file (offset, length)\n");
        glog("  sd write <path> <base64>         - Write base64 data to file\n");
        glog("  sd append <path> <base64>        - Append base64 data to file\n");
        glog("  sd mkdir <path>                  - Create directory\n");
        glog("  sd rm <idx|path>                 - Delete file or empty directory\n");
        glog("  sd tree [path] [depth]           - Recursive listing\n");
        return;
    }

    const char *sub = argv[1];
    char path[256];

    if (strcmp(sub, "status") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:STATUS:mounted=false\n");
            sd_cli_cleanup();
            return;
        }
        glog("SD:STATUS:mounted=true\n");
        if (sd_card_is_virtual_storage()) {
            glog("SD:STATUS:type=virtual\n");
        } else if (sd_card_manager.card) {
            glog("SD:STATUS:type=physical\n");
            glog("SD:STATUS:name=%s\n", sd_card_manager.card->cid.name);
            uint64_t cap_mb = ((uint64_t)sd_card_manager.card->csd.capacity * 
                               sd_card_manager.card->csd.sector_size) / (1024 * 1024);
            glog("SD:STATUS:capacity_mb=%llu\n", (unsigned long long)cap_mb);
        }
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info("/mnt", &total, &free_bytes) == ESP_OK && total > 0) {
            glog("SD:STATUS:total=%llu\n", (unsigned long long)total);
            glog("SD:STATUS:free=%llu\n", (unsigned long long)free_bytes);
            glog("SD:STATUS:total_mb=%llu\n", (unsigned long long)(total / (1024 * 1024)));
            glog("SD:STATUS:free_mb=%llu\n", (unsigned long long)(free_bytes / (1024 * 1024)));
            glog("SD:STATUS:used_pct=%d\n", (int)(((total - free_bytes) * 100) / total));
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "list") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        const char *list_path = (argc >= 3) ? argv[2] : "/mnt/ghostesp";
        if (list_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", list_path);
        } else {
            strncpy(path, list_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        DIR *d = opendir(path);
        if (!d) {
            glog("SD:ERR:cannot_open:%s\n", path);
            sd_cli_clear_index();
            return;
        }

        sd_cli_clear_index();
        glog("SD:LIST:%s\n", path);

        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

            struct stat st;
            bool is_dir = false;
            long fsize = 0;

            if (stat(fullpath, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                fsize = is_dir ? 0 : (long)st.st_size;
            } else if (entry->d_type == DT_DIR) {
                is_dir = true;
            }

            int idx = (int)g_sd_cli_count;
            if (g_sd_cli_count < SD_CLI_MAX_ENTRIES) {
                g_sd_cli_paths[g_sd_cli_count] = strdup(fullpath);
                g_sd_cli_types[g_sd_cli_count] = is_dir ? 1 : 0;
                if (g_sd_cli_paths[g_sd_cli_count]) g_sd_cli_count++;
            }

            if (is_dir) {
                glog("SD:DIR:[%d] %s\n", idx, entry->d_name);
            } else {
                glog("SD:FILE:[%d] %s %ld\n", idx, entry->d_name, fsize);
            }
        }
        closedir(d);

        if (g_sd_cli_count == 0) {
            glog("SD:EMPTY\n");
        }
        glog("SD:OK:listed %zu entries\n", g_sd_cli_count);
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "info") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }

        glog("SD:INFO:path=%s\n", resolved);
        glog("SD:INFO:type=%s\n", S_ISDIR(st.st_mode) ? "dir" : "file");
        glog("SD:INFO:size=%ld\n", (long)st.st_size);
        glog("SD:OK\n");
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "cat") == 0 || strcmp(sub, "read") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        bool raw_output = (strcmp(sub, "cat") == 0);
        bool base64_output = false;
        long offset = 0;
        size_t max_bytes = 0;
        int numeric_arg_count = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--raw") == 0) {
                raw_output = true;
                base64_output = false;
                continue;
            }
            if (strcmp(argv[i], "--base64") == 0) {
                raw_output = false;
                base64_output = true;
                continue;
            }

            char *endptr = NULL;
            long v = strtol(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0') {
                glog("SD:ERR:invalid_arg:%s\n", argv[i]);
                sd_cli_cleanup();
                return;
            }

            if (numeric_arg_count == 0) {
                offset = v;
                if (offset < 0) offset = 0;
            } else if (numeric_arg_count == 1) {
                if (v > 0) {
                    max_bytes = (size_t)v;
                }
            } else {
                glog("SD:ERR:too_many_args\n");
                sd_cli_cleanup();
                return;
            }
            numeric_arg_count++;
        }

        FILE *f = fopen(resolved, "rb");
        if (!f) {
            glog("SD:ERR:cannot_open:%s\n", resolved);
            return;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        if (offset > file_size) offset = file_size;
        fseek(f, offset, SEEK_SET);

        if (max_bytes == 0 || max_bytes > (size_t)(file_size - offset)) {
            max_bytes = (size_t)(file_size - offset);
        }

        if (!raw_output) {
            glog("SD:READ:BEGIN:%s\n", resolved);
            glog("SD:READ:SIZE:%ld\n", file_size);
            glog("SD:READ:OFFSET:%ld\n", offset);
            glog("SD:READ:LENGTH:%zu\n", max_bytes);
            if (base64_output) {
                glog("SD:READ:ENCODING:base64\n");
            }
        }

        size_t read_buf_size = base64_output ? 768 : 1024; /* 768 keeps base64 chunks aligned. */
        unsigned char *buf = malloc(read_buf_size);
        if (!buf) {
            fclose(f);
            glog("SD:ERR:oom\n");
            return;
        }
        char *b64 = NULL;
        size_t b64_size = ((read_buf_size + 2) / 3) * 4 + 1;
        if (base64_output) {
            b64 = malloc(b64_size);
            if (!b64) {
                free(buf);
                fclose(f);
                glog("SD:ERR:oom\n");
                return;
            }
        }

        size_t total_read = 0;
        size_t n;
        bool read_failed = false;
        while (total_read < max_bytes) {
            size_t remaining = max_bytes - total_read;
            size_t to_write = remaining < read_buf_size ? remaining : read_buf_size;
            n = fread(buf, 1, to_write, f);
            if (n == 0) {
                break;
            }
            to_write = n;

            if (base64_output) {
                size_t written = 0;
                int ret = mbedtls_base64_encode((unsigned char *)b64,
                                                b64_size,
                                                &written,
                                                buf,
                                                to_write);
                if (ret != 0) {
                    glog("SD:ERR:base64_encode_failed\n");
                    read_failed = true;
                    break;
                }
                b64[written] = '\0';
                /* glog truncates output at 511 bytes, so a 1024-char base64 line
                 * would be cut mid-character. Emit 4-aligned pieces that each fit
                 * the glog buffer; the host parser decodes and concatenates them. */
                const size_t b64_line_max = 480; /* multiple of 4, 13 + 480 + 1 < 512 */
                size_t pos = 0;
                while (pos < written) {
                    size_t piece = written - pos;
                    if (piece > b64_line_max) piece = b64_line_max;
                    glog("SD:READ:DATA:%.*s\n", (int)piece, b64 + pos);
                    pos += piece;
                }
            } else {
                size_t out_written = fwrite(buf, 1, to_write, stdout);
                if (out_written != to_write) {
                    if (raw_output) {
                        glog("SD:ERR:stdout_write_failed\n");
                    } else {
                        glog("\nSD:ERR:stdout_write_failed\n");
                    }
                    read_failed = true;
                    break;
                }
            }
            total_read += to_write;
        }
        if (ferror(f)) {
            if (raw_output) {
                glog("SD:ERR:file_read_failed\n");
            } else {
                glog("\nSD:ERR:file_read_failed\n");
            }
            read_failed = true;
        }
        free(b64);
        free(buf);
        fclose(f);

        if (!raw_output) {
            glog(base64_output ? "SD:READ:END:bytes=%zu\n" : "\nSD:READ:END:bytes=%zu\n", total_read);
            if (!read_failed) {
                glog("SD:OK\n");
            }
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "write") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 4) {
            glog("SD:ERR:usage: sd write <path> <base64data>\n");
            sd_cli_cleanup();
            return;
        }
        const char *write_path = argv[2];
        if (write_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", write_path);
        } else {
            strncpy(path, write_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        const char *b64data = argv[3];
        size_t b64len = strlen(b64data);
        size_t decoded_len = (b64len * 3) / 4 + 4;
        unsigned char *decoded = malloc(decoded_len);
        if (!decoded) {
            glog("SD:ERR:oom\n");
            sd_cli_cleanup();
            return;
        }

        size_t olen = 0;
        int ret = mbedtls_base64_decode(decoded, decoded_len, &olen, (const unsigned char *)b64data, b64len);
        if (ret != 0) {
            free(decoded);
            glog("SD:ERR:base64_decode_failed\n");
            sd_cli_cleanup();
            return;
        }

        FILE *f = fopen(path, "wb");
        if (!f) {
            free(decoded);
            glog("SD:ERR:cannot_create:%s\n", path);
            sd_cli_cleanup();
            return;
        }

        size_t written = fwrite(decoded, 1, olen, f);
        int write_failed = ferror(f);
        fclose(f);
        free(decoded);

        glog("SD:WRITE:bytes=%zu\n", written);
        if (write_failed || written != olen) {
            glog("SD:ERR:short_write:%s\n", path);
        } else {
            glog("SD:OK:created:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "append") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 4) {
            glog("SD:ERR:usage: sd append <path> <base64data>\n");
            sd_cli_cleanup();
            return;
        }
        const char *append_path = argv[2];
        if (append_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", append_path);
        } else {
            strncpy(path, append_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        const char *b64data = argv[3];
        size_t b64len = strlen(b64data);
        size_t decoded_len = (b64len * 3) / 4 + 4;
        unsigned char *decoded = malloc(decoded_len);
        if (!decoded) {
            glog("SD:ERR:oom\n");
            sd_cli_cleanup();
            return;
        }

        size_t olen = 0;
        int ret = mbedtls_base64_decode(decoded, decoded_len, &olen, (const unsigned char *)b64data, b64len);
        if (ret != 0) {
            free(decoded);
            glog("SD:ERR:base64_decode_failed\n");
            sd_cli_cleanup();
            return;
        }

        FILE *f = fopen(path, "ab");
        if (!f) {
            free(decoded);
            glog("SD:ERR:cannot_open:%s\n", path);
            sd_cli_cleanup();
            return;
        }

        size_t written = fwrite(decoded, 1, olen, f);
        int write_failed = ferror(f);
        fclose(f);
        free(decoded);

        glog("SD:APPEND:bytes=%zu\n", written);
        if (write_failed || written != olen) {
            glog("SD:ERR:short_write:%s\n", path);
        } else {
            glog("SD:OK:appended:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "size") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }
        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }
        glog("SD:SIZE:%ld\n", (long)st.st_size);
        glog("SD:OK\n");
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "mkdir") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *mk_path = argv[2];
        if (mk_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", mk_path);
        } else {
            strncpy(path, mk_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        if (mkdir(path, 0777) == 0) {
            glog("SD:OK:created:%s\n", path);
        } else {
            glog("SD:ERR:mkdir_failed:%s\n", path);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "rm") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        if (argc < 3) {
            glog("SD:ERR:missing_path\n");
            return;
        }
        const char *resolved = sd_cli_resolve_path(argv[2], path, sizeof(path));
        if (!resolved) {
            glog("SD:ERR:invalid_index\n");
            return;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            glog("SD:ERR:not_found:%s\n", resolved);
            return;
        }

        int ret;
        if (S_ISDIR(st.st_mode)) {
            ret = rmdir(resolved);
        } else {
            ret = unlink(resolved);
        }

        if (ret == 0) {
            glog("SD:OK:removed:%s\n", resolved);
        } else {
            glog("SD:ERR:rm_failed:%s\n", resolved);
        }
        sd_cli_cleanup();
        return;
    }

    if (strcmp(sub, "tree") == 0) {
        if (!sd_cli_ensure_mounted()) {
            glog("SD:ERR:not_mounted\n");
            sd_cli_cleanup();
            return;
        }
        const char *tree_path = (argc >= 3) ? argv[2] : "/mnt/ghostesp";
        int max_depth = 2;
        if (argc >= 4) {
            int d = atoi(argv[3]);
            if (d > 0 && d <= 10) max_depth = d;
        }

        if (tree_path[0] != '/') {
            snprintf(path, sizeof(path), "/mnt/ghostesp/%s", tree_path);
        } else {
            strncpy(path, tree_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        glog("SD:TREE:%s\n", path);

        /* Pack a directory path and its level into one heap block per stack
         * slot. We only ever need max_depth+1 slots (current dir + each
         * nested level), and the path buffer can be smaller than 256 since
         * paths deeper than ~200 chars are not realistic for a handheld
         * device's SD card. Path lives inline; lvl is a single byte. */
        typedef struct {
            char p[192];
            uint8_t lvl;
        } stack_item_t;

        size_t stack_cap = (size_t)max_depth + 1;
        stack_item_t *stack = malloc(stack_cap * sizeof(stack_item_t));
        if (!stack) {
            glog("SD:ERR:oom\n");
            return;
        }
        int sp = 0;
        strncpy(stack[sp].p, path, sizeof(stack[sp].p) - 1);
        stack[sp].p[sizeof(stack[sp].p) - 1] = '\0';
        stack[sp].lvl = 0;
        sp++;

        size_t count = 0;
        while (sp > 0 && count < 500) {
            sp--;
            char *cur = stack[sp].p;
            int lvl = stack[sp].lvl;

            DIR *d = opendir(cur);
            if (!d) continue;

            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && count < 500) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

                /* full must be at least sizeof(stack_item_t.p) + 1 + NAME_MAX
                 * to safely format cur + "/" + entry->d_name without -Wformat-truncation
                 * tripping on the compiler. 512 is plenty (192 + 1 + 255). */
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", cur, entry->d_name);

                struct stat st;
                bool is_dir = false;
                if (stat(full, &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                } else if (entry->d_type == DT_DIR) {
                    is_dir = true;
                }

                for (int i = 0; i < lvl; i++) printf("  ");
                if (is_dir) {
                    printf("[D] %s/\n", entry->d_name);
                    if (lvl + 1 < max_depth && (size_t)sp < stack_cap) {
                        strncpy(stack[sp].p, full, sizeof(stack[sp].p) - 1);
                        stack[sp].p[sizeof(stack[sp].p) - 1] = '\0';
                        stack[sp].lvl = (uint8_t)(lvl + 1);
                        sp++;
                    }
                } else {
                    printf("[F] %s (%ld)\n", entry->d_name, (long)st.st_size);
                }
                count++;
            }
            closedir(d);
        }
        free(stack);
        glog("SD:OK:tree %zu items\n", count);
        sd_cli_cleanup();
        return;
    }

    glog("SD:ERR:unknown_subcommand:%s\n", sub);
    sd_cli_cleanup();
}
