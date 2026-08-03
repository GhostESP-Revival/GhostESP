#include "managers/plugin_installer.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lgfx/utility/lgfx_miniz.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#define APPS_DIR "/mnt/ghostesp/apps"
#define APPDATA_DIR "/mnt/ghostesp/appdata"
#define STAGING_DIR "/mnt/ghostesp/app_install_staging"
#define BACKUP_DIR "/mnt/ghostesp/apps_backup"
#define GAPP_EXTRACT_DIR "/mnt/ghostesp/app_install_staging/_gapp_extract"
#define PATH_MAX_LOCAL 384
#define MANIFEST_MAX_BYTES 8192
#define GAPP_METHOD_STORE 0
#define GAPP_METHOD_DEFLATE 1
#define GAPP_MAX_FILE_BYTES (2u * 1024u * 1024u)

static const char *TAG = "PluginInstaller";
static char s_last_error[PLUGIN_INSTALLER_ERROR_MAX];

static esp_err_t set_error(esp_err_t err, const char *fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
        va_end(ap);
    } else {
        snprintf(s_last_error, sizeof(s_last_error), "plugin install error");
    }
    ESP_LOGE(TAG, "%s", s_last_error);
    return err;
}

static bool safe_id(const char *id) {
    if (!id || id[0] == '\0') return false;
    for (const char *p = id; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool join_path(char *dst, size_t dst_len, const char *base, const char *name) {
    if (!dst || !base || !name) return false;
    int n = snprintf(dst, dst_len, "%s/%s", base, name);
    return n > 0 && (size_t)n < dst_len;
}

static bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool has_gapp_extension(const char *path) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".gapp") == 0;
}

static int mkdir_if_missing(const char *path) {
    if (path_is_dir(path)) return 0;
    return mkdir(path, 0775);
}

static bool mkdir_recursive_for_dir(const char *path) {
    if (!path || path[0] == '\0') return false;
    char tmp[PATH_MAX_LOCAL];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir_if_missing(tmp) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir_if_missing(tmp) == 0 || errno == EEXIST;
}

static bool mkdir_parent_dirs(const char *path) {
    char tmp[PATH_MAX_LOCAL];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    return mkdir_recursive_for_dir(tmp);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint64_t checksum_bytes(const uint8_t *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Streams an existing extracted file to check whether it already matches
   the archive entry, so re-extraction can skip files that are unchanged
   between materialize passes (e.g. rebuilding just doom_port.so shouldn't
   force every multi-megabyte WAD part to be re-decompressed and rewritten
   to the SD cache). */
static bool existing_file_matches(const char *path, uint32_t expected_size, uint64_t expected_checksum) {
    struct stat st;
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != expected_size) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[512];
    uint64_t hash = 14695981039346656037ULL;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    bool read_ok = !ferror(f);
    fclose(f);
    return read_ok && hash == expected_checksum;
}

static bool archive_path_safe(const char *name) {
    if (!name || name[0] == '\0' || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

static bool write_bytes_to_file(const char *path, const uint8_t *data, size_t len) {
    if (!mkdir_parent_dirs(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool read_exact(FILE *f, void *buf, size_t len) {
    return len == 0 || fread(buf, 1, len, f) == len;
}

#define GAPP_ASSET_PREFIX "assets/"
#define GAPP_ASSET_PREFIX_LEN 7

typedef struct {
    char name[96];
    uint32_t offset;
    uint32_t length;
} gapp_direct_entry_t;

/* Writes the ".direct_index" sidecar so plugin_api.c's asset reader can serve
   these entries straight out of the .gapp instead of a materialized copy.
   Format: "DIDX" magic, u16 version=1, u16 gapp_path_len, gapp_path bytes,
   u32 count, then per entry: u16 name_len, name bytes, u32 offset, u32 length. */
static bool write_direct_index(const char *dst_dir, const char *gapp_path,
                               const gapp_direct_entry_t *entries, uint32_t count) {
    char index_path[PATH_MAX_LOCAL];
    if (!join_path(index_path, sizeof(index_path), dst_dir, ".direct_index")) return false;
    if (count == 0) {
        unlink(index_path);
        return true;
    }
    size_t gapp_len = strlen(gapp_path);
    if (gapp_len == 0 || gapp_len > 0xFFFF) return false;

    FILE *f = fopen(index_path, "wb");
    if (!f) return false;
    bool ok = fwrite("DIDX", 1, 4, f) == 4;
    uint16_t version = 1, gapp_len16 = (uint16_t)gapp_len;
    ok = ok && fwrite(&version, sizeof(version), 1, f) == 1;
    ok = ok && fwrite(&gapp_len16, sizeof(gapp_len16), 1, f) == 1;
    ok = ok && fwrite(gapp_path, 1, gapp_len, f) == gapp_len;
    ok = ok && fwrite(&count, sizeof(count), 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; ++i) {
        uint16_t name_len = (uint16_t)strlen(entries[i].name);
        ok = fwrite(&name_len, sizeof(name_len), 1, f) == 1 &&
             fwrite(entries[i].name, 1, name_len, f) == name_len &&
             fwrite(&entries[i].offset, sizeof(uint32_t), 1, f) == 1 &&
             fwrite(&entries[i].length, sizeof(uint32_t), 1, f) == 1;
    }
    fclose(f);
    if (!ok) unlink(index_path);
    return ok;
}

static esp_err_t extract_gapp_to_dir(const char *gapp_path, const char *dst_dir) {
    FILE *f = fopen(gapp_path, "rb");
    if (!f) { ESP_LOGE(TAG, "extract: cannot open %s", gapp_path); return ESP_FAIL; }
    if (mkdir_recursive_for_dir(dst_dir) == false) { fclose(f); ESP_LOGE(TAG, "extract: cannot create dir %s", dst_dir); return ESP_FAIL; }

    uint8_t file_hdr[24];
    if (!read_exact(f, file_hdr, 12) || memcmp(file_hdr, "GAPP", 4) != 0 || rd16(file_hdr + 4) != 1) {
        fclose(f);
        ESP_LOGE(TAG, "extract: invalid GAPP header in %s", gapp_path);
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t file_count = rd32(file_hdr + 8);
    if (file_count == 0 || file_count > 128) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    /* STORE-method entries under "assets/" can be served straight out of the
       .gapp at read time instead of being decompressed+copied to the SD
       cache (see write_direct_index / plugin_api.c's direct-index reader).
       Falling back gracefully (direct_entries == NULL) just means every
       entry extracts the old way, so an allocation failure here never
       breaks materialize, only the size optimization. */
    gapp_direct_entry_t *direct_entries = heap_caps_malloc(sizeof(gapp_direct_entry_t) * file_count,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!direct_entries) direct_entries = malloc(sizeof(gapp_direct_entry_t) * file_count);
    uint32_t direct_count = 0;

    /* Diagnostic-only timing to pinpoint where materialize time actually
       goes (source read vs decompress CPU vs cache write vs skip-check),
       instead of guessing further at the SD/SPI layer. */
    int64_t t_skipcheck_us = 0, t_read_us = 0, t_decompress_us = 0, t_write_us = 0;
    uint64_t bytes_read = 0, bytes_written = 0;
    int extracted_count = 0, skipped_count = 0;
    int64_t extract_start_us = esp_timer_get_time();

    for (uint32_t i = 0; i < file_count; ++i) {
        if (!read_exact(f, file_hdr, sizeof(file_hdr)) || memcmp(file_hdr, "FILE", 4) != 0) {
            free(direct_entries);
            fclose(f);
            ESP_LOGE(TAG, "extract: bad FILE header at entry %u in %s", i, gapp_path);
            return ESP_FAIL;
        }
        uint16_t method = rd16(file_hdr + 4);
        uint16_t name_len = rd16(file_hdr + 6);
        uint32_t uncomp_size = rd32(file_hdr + 8);
        uint32_t comp_size = rd32(file_hdr + 12);
        uint64_t expected_checksum = rd64(file_hdr + 16);
        if (name_len == 0 || name_len >= PATH_MAX_LOCAL || uncomp_size > GAPP_MAX_FILE_BYTES || comp_size > GAPP_MAX_FILE_BYTES) {
            free(direct_entries);
            fclose(f);
            ESP_LOGE(TAG, "extract: invalid sizes at entry %u (name=%u uncomp=%u comp=%u)", i, name_len, uncomp_size, comp_size);
            return ESP_ERR_INVALID_SIZE;
        }

        char name[PATH_MAX_LOCAL];
        if (!read_exact(f, name, name_len)) { free(direct_entries); fclose(f); return ESP_FAIL; }
        name[name_len] = '\0';
        if (!archive_path_safe(name)) { free(direct_entries); fclose(f); return ESP_ERR_INVALID_ARG; }

        char out_path[PATH_MAX_LOCAL];
        if (!join_path(out_path, sizeof(out_path), dst_dir, name)) { free(direct_entries); fclose(f); return ESP_ERR_INVALID_SIZE; }

        if (direct_entries && method == GAPP_METHOD_STORE &&
            strncmp(name, GAPP_ASSET_PREFIX, GAPP_ASSET_PREFIX_LEN) == 0 &&
            direct_count < file_count &&
            strlen(name + GAPP_ASSET_PREFIX_LEN) < sizeof(direct_entries[0].name)) {
            int64_t payload_offset = ftell(f);
            if (payload_offset < 0) { free(direct_entries); fclose(f); return ESP_FAIL; }
            gapp_direct_entry_t *entry = &direct_entries[direct_count++];
            /* Length already checked above, so this can never truncate. */
            memcpy(entry->name, name + GAPP_ASSET_PREFIX_LEN, strlen(name + GAPP_ASSET_PREFIX_LEN) + 1);
            entry->offset = (uint32_t)payload_offset;
            entry->length = uncomp_size;
            /* Some apps' own code (e.g. doomgeneric's IWAD lookup) does a
               plain fopen()-based existence check on the asset's nominal
               path before ever asking the host to read it. A 0-byte
               placeholder satisfies that cheaply — it's never actually read,
               since asset_storage_read_at/size answer from the index first. */
            write_bytes_to_file(out_path, NULL, 0);
            if (fseek(f, (long)comp_size, SEEK_CUR) != 0) { free(direct_entries); fclose(f); return ESP_FAIL; }
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        bool matches = existing_file_matches(out_path, uncomp_size, expected_checksum);
        t_skipcheck_us += esp_timer_get_time() - t0;
        if (matches) {
            /* Already correct on disk: skip the read/decompress/write for
               this entry entirely, just seek past its compressed payload. */
            if (fseek(f, (long)comp_size, SEEK_CUR) != 0) { free(direct_entries); fclose(f); return ESP_FAIL; }
            skipped_count++;
            continue;
        }

        uint8_t *comp = heap_caps_malloc(comp_size ? comp_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!comp) comp = malloc(comp_size ? comp_size : 1);
        if (!comp) { free(direct_entries); fclose(f); return ESP_ERR_NO_MEM; }
        t0 = esp_timer_get_time();
        bool read_ok = read_exact(f, comp, comp_size);
        t_read_us += esp_timer_get_time() - t0;
        bytes_read += comp_size;
        if (!read_ok) { free(comp); free(direct_entries); fclose(f); return ESP_FAIL; }

        bool ok = false;
        if (method == GAPP_METHOD_STORE) {
            uint64_t actual = checksum_bytes(comp, comp_size);
            if (actual != expected_checksum) {
                ESP_LOGE(TAG, "extract: checksum mismatch for %s (entry %u) expected=0x%016llx actual=0x%016llx", name, i, (unsigned long long)expected_checksum, (unsigned long long)actual);
            } else if (comp_size != uncomp_size) {
                ESP_LOGE(TAG, "extract: size mismatch for %s (entry %u)", name, i);
            } else {
                t0 = esp_timer_get_time();
                ok = write_bytes_to_file(out_path, comp, comp_size);
                t_write_us += esp_timer_get_time() - t0;
                bytes_written += comp_size;
                if (!ok) ESP_LOGE(TAG, "extract: write failed for %s", out_path);
            }
        } else if (method == GAPP_METHOD_DEFLATE) {
            uint8_t *out = heap_caps_malloc(uncomp_size ? uncomp_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!out) out = malloc(uncomp_size ? uncomp_size : 1);
            if (out) {
                t0 = esp_timer_get_time();
                size_t out_len = lgfx_tinfl_decompress_mem_to_mem(out, uncomp_size, comp, comp_size, 0);
                t_decompress_us += esp_timer_get_time() - t0;
                if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
                    ESP_LOGE(TAG, "extract: decompression failed for %s (entry %u) comp_size=%u uncomp_size=%u", name, i, comp_size, uncomp_size);
                } else if (out_len != uncomp_size) {
                    ESP_LOGE(TAG, "extract: decompress size mismatch for %s (%u vs %u)", name, (unsigned)out_len, uncomp_size);
                } else {
                    uint64_t actual = checksum_bytes(out, uncomp_size);
                    if (actual != expected_checksum) {
                        ESP_LOGE(TAG, "extract: checksum mismatch (deflate) for %s (entry %u) expected=0x%016llx actual=0x%016llx", name, i, (unsigned long long)expected_checksum, (unsigned long long)actual);
                    } else {
                        t0 = esp_timer_get_time();
                        ok = write_bytes_to_file(out_path, out, uncomp_size);
                        t_write_us += esp_timer_get_time() - t0;
                        bytes_written += uncomp_size;
                        if (!ok) ESP_LOGE(TAG, "extract: write failed for %s", out_path);
                    }
                }
                free(out);
            } else {
                ESP_LOGE(TAG, "extract: malloc(%u) failed for decompression of %s", uncomp_size, name);
            }
        } else {
            free(comp);
            free(direct_entries);
            fclose(f);
            ESP_LOGE(TAG, "extract: unknown method %u for %s (entry %u)", method, name, i);
            return ESP_ERR_NOT_SUPPORTED;
        }
        free(comp);
        if (!ok) { free(direct_entries); fclose(f); return ESP_FAIL; }
        extracted_count++;
    }

    fclose(f);
    if (!write_direct_index(dst_dir, gapp_path, direct_entries, direct_count)) {
        ESP_LOGW(TAG, "extract %s: failed to write direct-read index (%u entries)", dst_dir, (unsigned)direct_count);
    }
    free(direct_entries);
    ESP_LOGI(TAG, "extract %s: total=%lldms skipcheck=%lldms read=%lldms(%lluB) decompress=%lldms write=%lldms(%lluB) extracted=%d skipped=%d direct=%u",
             dst_dir,
             (long long)((esp_timer_get_time() - extract_start_us) / 1000),
             (long long)(t_skipcheck_us / 1000), (long long)(t_read_us / 1000), (unsigned long long)bytes_read,
             (long long)(t_decompress_us / 1000), (long long)(t_write_us / 1000), (unsigned long long)bytes_written,
             extracted_count, skipped_count, (unsigned)direct_count);
    return ESP_OK;
}

static bool read_file(const char *path, char **out, size_t max_bytes) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_bytes) { fclose(f); return false; }
    rewind(f);
    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return false; }
    *out = buf;
    return true;
}

static bool checksum_file_hex(const char *path, char out_hex[17]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[512];
    uint64_t hash = 14695981039346656037ULL;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
    snprintf(out_hex, 17, "%016llx", (unsigned long long)hash);
    return true;
}

static bool validate_checksums(const char *package_path) {
    char checksums_path[PATH_MAX_LOCAL];
    if (!join_path(checksums_path, sizeof(checksums_path), package_path, "checksums.json")) return false;
    if (!path_exists(checksums_path)) return true;
    char *buf = NULL;
    if (!read_file(checksums_path, &buf, MANIFEST_MAX_BYTES)) return false;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsString(item) || !item->string || strstr(item->string, "..") || item->string[0] == '/') {
            cJSON_Delete(root);
            return false;
        }
        char file_path[PATH_MAX_LOCAL];
        char actual[17];
        if (!join_path(file_path, sizeof(file_path), package_path, item->string) ||
            !checksum_file_hex(file_path, actual) || strcasecmp(actual, item->valuestring) != 0) {
            cJSON_Delete(root);
            return false;
        }
    }
    cJSON_Delete(root);
    return true;
}

static esp_err_t remove_recursive(const char *path) {
    if (!path_exists(path)) return ESP_OK;
    if (!path_is_dir(path)) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    DIR *dir = opendir(path);
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX_LOCAL];
        if (!join_path(child, sizeof(child), path, entry->d_name)) { closedir(dir); return ESP_FAIL; }
        if (remove_recursive(child) != ESP_OK) { closedir(dir); return ESP_FAIL; }
    }
    closedir(dir);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return ESP_FAIL;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return ESP_FAIL; }
    char buf[512];
    size_t n;
    esp_err_t ret = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ret = ESP_FAIL; break; }
    }
    if (ferror(in)) ret = ESP_FAIL;
    fclose(out);
    fclose(in);
    return ret;
}

static esp_err_t copy_recursive(const char *src, const char *dst) {
    if (!path_is_dir(src)) return copy_file(src, dst);
    if (mkdir_if_missing(dst) != 0 && errno != EEXIST) return ESP_FAIL;
    DIR *dir = opendir(src);
    if (!dir) return ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child_src[PATH_MAX_LOCAL];
        char child_dst[PATH_MAX_LOCAL];
        if (!join_path(child_src, sizeof(child_src), src, entry->d_name) ||
            !join_path(child_dst, sizeof(child_dst), dst, entry->d_name)) {
            closedir(dir);
            return ESP_FAIL;
        }
        if (copy_recursive(child_src, child_dst) != ESP_OK) { closedir(dir); return ESP_FAIL; }
    }
    closedir(dir);
    return ESP_OK;
}

static bool parse_manifest_identity(const char *package_path, char *app_id, size_t app_id_len, char *entry, size_t entry_len) {
    char manifest_path[PATH_MAX_LOCAL];
    if (!join_path(manifest_path, sizeof(manifest_path), package_path, "manifest.json")) return false;
    char *buf = NULL;
    if (!read_file(manifest_path, &buf, MANIFEST_MAX_BYTES)) return false;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *entry_item = cJSON_GetObjectItemCaseSensitive(root, "entry");
    cJSON *api_version = cJSON_GetObjectItemCaseSensitive(root, "api_version");
    bool ok = cJSON_IsString(id) && cJSON_IsString(entry_item) && cJSON_IsNumber(api_version) && api_version->valueint == 1;
    if (ok) {
        snprintf(app_id, app_id_len, "%s", id->valuestring);
        snprintf(entry, entry_len, "%s", entry_item->valuestring);
        ok = safe_id(app_id) && !strstr(entry, "..") && !strchr(entry, '/') && !strchr(entry, '\\');
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t install_package(const char *package_path, bool validate_package_checksums) {
    s_last_error[0] = '\0';
    if (has_gapp_extension(package_path) && !path_is_dir(package_path)) {
        return plugin_installer_install_gapp(package_path);
    }
    if (!path_is_dir(package_path)) return set_error(ESP_ERR_INVALID_ARG, "package path must be an extracted .gapp folder");

    char app_id[64];
    char entry[96];
    if (!parse_manifest_identity(package_path, app_id, sizeof(app_id), entry, sizeof(entry))) {
        return set_error(ESP_ERR_INVALID_ARG, "package manifest invalid");
    }
    if (validate_package_checksums && !validate_checksums(package_path)) {
        return set_error(ESP_ERR_INVALID_CRC, "package checksum validation failed");
    }

    char entry_path[PATH_MAX_LOCAL];
    if (!join_path(entry_path, sizeof(entry_path), package_path, entry) || !path_exists(entry_path)) {
        return set_error(ESP_ERR_NOT_FOUND, "package entry binary missing");
    }

    mkdir_if_missing(APPS_DIR);
    mkdir_if_missing(APPDATA_DIR);
    mkdir_if_missing(STAGING_DIR);
    mkdir_if_missing(BACKUP_DIR);

    char staging[PATH_MAX_LOCAL];
    char app_dst[PATH_MAX_LOCAL];
    char backup[PATH_MAX_LOCAL];
    if (!join_path(staging, sizeof(staging), STAGING_DIR, app_id) ||
        !join_path(app_dst, sizeof(app_dst), APPS_DIR, app_id) ||
        !join_path(backup, sizeof(backup), BACKUP_DIR, app_id)) {
        return set_error(ESP_ERR_INVALID_SIZE, "install path too long");
    }

    remove_recursive(staging);
    remove_recursive(backup);
    if (copy_recursive(package_path, staging) != ESP_OK) return set_error(ESP_FAIL, "failed to copy package to staging");
    if (path_exists(app_dst) && rename(app_dst, backup) != 0) {
        remove_recursive(staging);
        return set_error(ESP_FAIL, "failed to backup installed app");
    }
    if (rename(staging, app_dst) != 0) {
        if (path_exists(backup)) rename(backup, app_dst);
        remove_recursive(staging);
        return set_error(ESP_FAIL, "failed to activate staged app");
    }
    remove_recursive(backup);
    ESP_LOGI(TAG, "Installed app package %s", app_id);
    return ESP_OK;
}

esp_err_t plugin_installer_install_package(const char *package_path) {
    return install_package(package_path, true);
}

esp_err_t plugin_installer_install_gapp(const char *gapp_path) {
    s_last_error[0] = '\0';
    if (!gapp_path || !has_gapp_extension(gapp_path) || !path_exists(gapp_path)) {
        return set_error(ESP_ERR_INVALID_ARG, "gapp path must point to a .gapp package file");
    }
    mkdir_if_missing(STAGING_DIR);
    remove_recursive(GAPP_EXTRACT_DIR);
    esp_err_t err = extract_gapp_to_dir(gapp_path, GAPP_EXTRACT_DIR);
    if (err != ESP_OK) {
        remove_recursive(GAPP_EXTRACT_DIR);
        return set_error(err, "failed to extract .gapp package");
    }
    /* GAPP extraction already verifies every uncompressed entry checksum. */
    err = install_package(GAPP_EXTRACT_DIR, false);
    remove_recursive(GAPP_EXTRACT_DIR);
    return err;
}

esp_err_t plugin_installer_extract_gapp_to_dir(const char *gapp_path, const char *dst_dir) {
    s_last_error[0] = '\0';
    if (!gapp_path || !has_gapp_extension(gapp_path) || !path_exists(gapp_path) || !dst_dir || dst_dir[0] == '\0') {
        return set_error(ESP_ERR_INVALID_ARG, "invalid .gapp extraction request");
    }
    remove_recursive(dst_dir);
    esp_err_t err = extract_gapp_to_dir(gapp_path, dst_dir);
    if (err != ESP_OK) {
        remove_recursive(dst_dir);
        const char *reason = esp_err_to_name(err);
        return set_error(err, "failed to extract .gapp package: %s", reason);
    }
    return ESP_OK;
}

esp_err_t plugin_installer_uninstall_app(const char *app_id, plugin_installer_data_mode_t data_mode) {
    s_last_error[0] = '\0';
    if (!safe_id(app_id)) return set_error(ESP_ERR_INVALID_ARG, "invalid app id");
    char app_path[PATH_MAX_LOCAL];
    if (!join_path(app_path, sizeof(app_path), APPS_DIR, app_id)) return set_error(ESP_ERR_INVALID_SIZE, "app path too long");
    if (remove_recursive(app_path) != ESP_OK) return set_error(ESP_FAIL, "failed to remove app package");
    if (data_mode == PLUGIN_INSTALLER_DELETE_DATA) {
        char data_path[PATH_MAX_LOCAL];
        if (!join_path(data_path, sizeof(data_path), APPDATA_DIR, app_id)) return set_error(ESP_ERR_INVALID_SIZE, "appdata path too long");
        if (remove_recursive(data_path) != ESP_OK) return set_error(ESP_FAIL, "failed to remove app data");
    }
    ESP_LOGI(TAG, "Uninstalled app %s", app_id);
    return ESP_OK;
}

const char *plugin_installer_last_error(void) {
    return s_last_error;
}
