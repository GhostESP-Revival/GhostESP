#include "managers/views/book_reader_view.h"

#ifdef CONFIG_CROWPANEL_EPAPER_42

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#define LGFX_MINIZ_ENABLE_ARCHIVE 1
#include "lgfx/utility/lgfx_miniz.h"
#include "lgfx/utility/lgfx_tjpgd.h"
#if CONFIG_ESP_ROM_HAS_JPEG_DECODE
#include "esp_jpg_decode.h"
#endif

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define READER_TAG "BookReader"

/* This is deliberately the only content root used by the reader.  Keeping
 * books separate from the general SD browser makes the first-use workflow
 * predictable and prevents a large SD scan from making the reader sluggish. */
#define READER_ROOT SD_DIR_COMICS

#define READER_MAX_BOOKS       64
#define READER_MAX_PAGES       256
#define READER_MAX_PATH        256
#define READER_MAX_BOOK_NAME   96
#define READER_MAX_TEXT_PAGES  2048
#define READER_TEXT_LINES      12
#define READER_TEXT_COLUMNS    48
#define READER_TEXT_BUFFER     2048
#define READER_MAX_FILE_BYTES  (12u * 1024u * 1024u)
#define READER_JPEG_POOL_BYTES (32u * 1024u)
#define READER_FOOTER_H        30
#define READER_COMIC_INVERT    1
#define READER_COMIC_INK_THRESHOLD 192
#define READER_PAGE_CACHE_MAX_BYTES (4u * 1024u * 1024u)
#define READER_COMIC_DETAIL_MAX_TILES 32
#define READER_EPUB_CONTAINER_MAX_BYTES (64u * 1024u)
#define READER_EPUB_OPF_MAX_BYTES       (256u * 1024u)
#define READER_EPUB_CHAPTER_MAX_BYTES  (2u * 1024u * 1024u)
#define READER_EPUB_TEXT_MAX_BYTES     (2u * 1024u * 1024u)
#define READER_EPUB_MAX_MANIFEST       192

typedef enum {
    READER_BOOK_CBZ,
    READER_BOOK_FOLDER,
    READER_BOOK_TEXT,
    READER_BOOK_IMAGE,
    READER_BOOK_EPUB,
} reader_book_kind_t;

typedef enum {
    READER_MODE_LIBRARY,
    READER_MODE_READING,
} reader_mode_t;

typedef struct {
    char path[READER_MAX_PATH];
    char name[READER_MAX_BOOK_NAME];
    reader_book_kind_t kind;
} reader_book_t;

typedef struct {
    char path[READER_MAX_PATH];
    char member[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    bool from_zip;
} reader_page_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t position;
    int x;
    int y;
    int width;
    int height;
    int source_width;
    int source_height;
    int render_width;
    int render_height;
} reader_jpeg_session_t;

static const char *TAG = READER_TAG;

static lv_obj_t *s_root;
static options_view_t *s_options;
static lv_obj_t *s_page_area;
static lv_obj_t *s_page_footer;
static lv_obj_t *s_page_image;
static lv_obj_t *s_page_text;
static lv_obj_t *s_page_message;
static lv_obj_t *s_page_counter;

static reader_mode_t s_mode = READER_MODE_LIBRARY;
static reader_book_t *s_books;
static int s_book_count;
static int s_book_index;
static reader_page_t *s_pages;
static int s_page_count;
static int s_page_index;
static long *s_text_offsets;
static char s_text_page[READER_TEXT_BUFFER];
static char *s_epub_text;
static size_t s_epub_text_length;

static lv_color_t *s_image_pixels;
static uint16_t s_image_width;
static uint16_t s_image_height;
static lv_img_dsc_t s_image_dsc;
static bool s_rendering;
static int64_t s_last_page_action_us;
static bool s_comic_split_active;
/* Section is either a normal overview section or a detail tile. Detail
 * tiles are ordered row-major so NEXT reads left-to-right, top-to-bottom. */
static uint8_t s_comic_section;
static uint8_t s_comic_section_count;
static uint8_t s_comic_tile_columns;
static bool s_comic_detail_active;
static uint8_t *s_page_data_cache;
static size_t s_page_data_cache_length;
static int s_page_data_cache_index = -1;
static uint8_t *s_prefetch_data_cache;
static size_t s_prefetch_data_cache_length;
static int s_prefetch_data_cache_index = -1;
static SemaphoreHandle_t s_prefetch_lock;
static SemaphoreHandle_t s_reader_io_lock;
static TaskHandle_t s_prefetch_task;
static volatile bool s_prefetch_stop;
static reader_page_t s_prefetch_page;
static int s_prefetch_target = -1;
static uint8_t *s_comic_render_cache;
static uint16_t s_comic_render_cache_width;
static uint16_t s_comic_render_cache_height;
static int s_comic_render_cache_page = -1;

static void reader_show_library(void);
static void reader_show_reading(void);
static void reader_activate_selected(void);
static void reader_render_page(void);
static bool reader_load_page_data(const reader_page_t *page, uint8_t **data, size_t *length);

static void *reader_alloc(size_t size) {
    if (!size) size = 1;
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return ptr;
}

static void reader_free(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

static int reader_strcasecmp(const char *left, const char *right) {
    if (!left) return right ? -1 : 0;
    if (!right) return 1;
    while (*left && *right) {
        int a = tolower((unsigned char)*left);
        int b = tolower((unsigned char)*right);
        if (a != b) return a - b;
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

static const char *reader_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *reader_extension(const char *name) {
    const char *base = reader_basename(name);
    const char *dot = strrchr(base, '.');
    return (dot && dot != base) ? dot + 1 : "";
}

static bool reader_extension_is(const char *name, const char *extension) {
    return reader_strcasecmp(reader_extension(name), extension) == 0;
}

static bool reader_is_image_name(const char *name) {
    return reader_extension_is(name, "jpg") ||
           reader_extension_is(name, "jpeg") ||
           reader_extension_is(name, "bmp") ||
           reader_extension_is(name, "pbm");
}

static bool reader_is_text_name(const char *name) {
    return reader_extension_is(name, "txt") || reader_extension_is(name, "md");
}

static bool reader_is_cbz_name(const char *name) {
    return reader_extension_is(name, "cbz");
}

static bool reader_is_epub_name(const char *name) {
    return reader_extension_is(name, "epub");
}

static bool reader_join_path(char *out, size_t out_size, const char *dir, const char *name) {
    if (!out || !out_size || !dir || !name) return false;
    int written = snprintf(out, out_size, "%s/%s", dir, name);
    return written > 0 && (size_t)written < out_size;
}

static void reader_book_name_from_path(char *out, size_t out_size, const char *path) {
    const char *base = reader_basename(path);
    snprintf(out, out_size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static int reader_book_compare(const void *a, const void *b) {
    const reader_book_t *left = (const reader_book_t *)a;
    const reader_book_t *right = (const reader_book_t *)b;
    return reader_strcasecmp(left->name, right->name);
}

static int reader_page_compare(const void *a, const void *b) {
    const reader_page_t *left = (const reader_page_t *)a;
    const reader_page_t *right = (const reader_page_t *)b;
    const char *left_name = left->from_zip ? left->member : reader_basename(left->path);
    const char *right_name = right->from_zip ? right->member : reader_basename(right->path);
    return reader_strcasecmp(left_name, right_name);
}

static void reader_free_library(void) {
    reader_free(s_books);
    s_books = NULL;
    s_book_count = 0;
}

static void reader_free_image(void) {
    reader_free(s_image_pixels);
    s_image_pixels = NULL;
    s_image_width = 0;
    s_image_height = 0;
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
}

static void reader_free_page_cache(void) {
    reader_free(s_page_data_cache);
    s_page_data_cache = NULL;
    s_page_data_cache_length = 0;
    s_page_data_cache_index = -1;
}

static void reader_free_prefetch_cache(void) {
    if (s_prefetch_lock) xSemaphoreTake(s_prefetch_lock, portMAX_DELAY);
    reader_free(s_prefetch_data_cache);
    s_prefetch_data_cache = NULL;
    s_prefetch_data_cache_length = 0;
    s_prefetch_data_cache_index = -1;
    if (s_prefetch_lock) xSemaphoreGive(s_prefetch_lock);
}

static void reader_free_comic_render_cache(void) {
    reader_free(s_comic_render_cache);
    s_comic_render_cache = NULL;
    s_comic_render_cache_width = 0;
    s_comic_render_cache_height = 0;
    s_comic_render_cache_page = -1;
}

static void reader_fill_white(void);
static void reader_stop_prefetch(void);

static void reader_free_pages(void) {
    reader_stop_prefetch();
    reader_free(s_pages);
    s_pages = NULL;
    reader_free(s_text_offsets);
    s_text_offsets = NULL;
    s_page_count = 0;
    s_page_index = 0;
    reader_free(s_epub_text);
    s_epub_text = NULL;
    s_epub_text_length = 0;
    reader_free_page_cache();
    reader_free_comic_render_cache();
    reader_free_image();
}

static bool reader_sd_begin(bool *mounted_here, bool *display_was_suspended) {
    if (mounted_here) *mounted_here = false;
    if (display_was_suspended) *display_was_suspended = false;
    if (sd_card_manager.is_initialized) return true;

    esp_err_t err = sd_card_mount_for_flush(display_was_suspended);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }
    if (mounted_here) *mounted_here = true;
    return true;
}

static void reader_sd_end(bool mounted_here, bool display_was_suspended) {
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

static void reader_prefetch_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_prefetch_stop) break;

        reader_page_t page;
        int target;
        if (!s_prefetch_lock || xSemaphoreTake(s_prefetch_lock, portMAX_DELAY) != pdTRUE) continue;
        page = s_prefetch_page;
        target = s_prefetch_target;
        xSemaphoreGive(s_prefetch_lock);

        uint8_t *data = NULL;
        size_t length = 0;
        bool mounted_here = false;
        bool suspended = false;
        /* FATFS mount state and file handles are not safe to use from the
         * foreground reader and the prefetch task simultaneously. */
        if (!s_reader_io_lock || xSemaphoreTake(s_reader_io_lock, portMAX_DELAY) != pdTRUE) continue;
        bool loaded = reader_sd_begin(&mounted_here, &suspended);
        if (loaded) loaded = reader_load_page_data(&page, &data, &length);
        reader_sd_end(mounted_here, suspended);
        xSemaphoreGive(s_reader_io_lock);
        if (!loaded) {
            reader_free(data);
            continue;
        }

        if (s_prefetch_lock && xSemaphoreTake(s_prefetch_lock, portMAX_DELAY) == pdTRUE) {
            if (s_prefetch_stop || target != s_prefetch_target) {
                reader_free(data);
            } else {
                reader_free(s_prefetch_data_cache);
                s_prefetch_data_cache = data;
                s_prefetch_data_cache_length = length;
                s_prefetch_data_cache_index = target;
                data = NULL;
                ESP_LOGI(TAG, "Prefetched CBZ page %d: %u bytes", target + 1, (unsigned)length);
            }
            xSemaphoreGive(s_prefetch_lock);
        } else {
            reader_free(data);
        }
    }
    s_prefetch_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void reader_schedule_prefetch(int page_index) {
    if (page_index < 0 || page_index >= s_page_count || !s_pages[page_index].from_zip) return;
    if (!s_prefetch_lock) s_prefetch_lock = xSemaphoreCreateMutex();
    if (!s_prefetch_lock) return;
    if (xSemaphoreTake(s_prefetch_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_prefetch_data_cache_index == page_index || s_prefetch_target == page_index) {
        xSemaphoreGive(s_prefetch_lock);
        return;
    }
    s_prefetch_page = s_pages[page_index];
    s_prefetch_target = page_index;
    s_prefetch_stop = false;
    if (!s_prefetch_task) {
        /* miniz/FATFS has a deep call chain while opening the archive. Keep
         * this stack in PSRAM so it does not consume the S3's scarce internal
         * heap, and give it enough headroom for the central-directory seek. */
        BaseType_t created = xTaskCreateWithCaps(reader_prefetch_task, "reader_prefetch",
                                                 12288, NULL, 2, &s_prefetch_task,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            created = xTaskCreate(reader_prefetch_task, "reader_prefetch", 12288,
                                  NULL, 2, &s_prefetch_task);
        }
        if (created != pdPASS) s_prefetch_task = NULL;
    }
    if (s_prefetch_task) xTaskNotifyGive(s_prefetch_task);
    xSemaphoreGive(s_prefetch_lock);
}

static void reader_stop_prefetch(void) {
    if (s_prefetch_task) {
        s_prefetch_stop = true;
        xTaskNotifyGive(s_prefetch_task);
        for (int i = 0; i < 20 && s_prefetch_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    reader_free_prefetch_cache();
    if (s_prefetch_lock) {
        vSemaphoreDelete(s_prefetch_lock);
        s_prefetch_lock = NULL;
    }
    s_prefetch_target = -1;
}

static bool reader_add_book(const char *path, const char *name, reader_book_kind_t kind) {
    if (!path || !name || !s_books || s_book_count >= READER_MAX_BOOKS) return false;
    reader_book_t *book = &s_books[s_book_count++];
    snprintf(book->path, sizeof(book->path), "%s", path);
    snprintf(book->name, sizeof(book->name), "%s", name);
    book->kind = kind;
    return true;
}

static bool reader_directory_has_pages(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return false;

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !reader_is_image_name(entry->d_name)) continue;
        char page_path[READER_MAX_PATH];
        if (!reader_join_path(page_path, sizeof(page_path), path, entry->d_name)) continue;
        struct stat st;
        if (stat(page_path, &st) == 0 && S_ISREG(st.st_mode)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static bool reader_scan_library(void) {
    reader_free_library();
    s_books = (reader_book_t *)reader_alloc(sizeof(reader_book_t) * READER_MAX_BOOKS);
    if (!s_books) return false;

    bool mounted_here = false;
    bool display_was_suspended = false;
    if (!reader_sd_begin(&mounted_here, &display_was_suspended)) return false;

    /* Older cards may predate the comics folder.  The normal SD boot path
     * creates it, but creating it lazily also handles an existing card cleanly. */
    struct stat root_stat;
    if (stat(READER_ROOT, &root_stat) != 0) {
        if (sd_card_setup_directory_structure() != ESP_OK) {
            reader_sd_end(mounted_here, display_was_suspended);
            return false;
        }
    }

    DIR *dir = opendir(READER_ROOT);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s: errno=%d", READER_ROOT, errno);
        reader_sd_end(mounted_here, display_was_suspended);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' ||
            strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        char path[READER_MAX_PATH];
        if (!reader_join_path(path, sizeof(path), READER_ROOT, entry->d_name)) continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (reader_directory_has_pages(path)) {
                reader_add_book(path, entry->d_name, READER_BOOK_FOLDER);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (reader_is_cbz_name(entry->d_name)) {
                char title[READER_MAX_BOOK_NAME];
                reader_book_name_from_path(title, sizeof(title), entry->d_name);
                reader_add_book(path, title, READER_BOOK_CBZ);
            } else if (reader_is_epub_name(entry->d_name)) {
                char title[READER_MAX_BOOK_NAME];
                reader_book_name_from_path(title, sizeof(title), entry->d_name);
                reader_add_book(path, title, READER_BOOK_EPUB);
            } else if (reader_is_text_name(entry->d_name)) {
                char title[READER_MAX_BOOK_NAME];
                reader_book_name_from_path(title, sizeof(title), entry->d_name);
                reader_add_book(path, title, READER_BOOK_TEXT);
            } else if (reader_is_image_name(entry->d_name)) {
                reader_add_book(path, entry->d_name, READER_BOOK_IMAGE);
            }
        }
    }
    closedir(dir);
    reader_sd_end(mounted_here, display_was_suspended);

    if (s_book_count > 1) qsort(s_books, (size_t)s_book_count, sizeof(*s_books), reader_book_compare);
    if (s_book_count == 0) s_book_index = 0;
    else if (s_book_index >= s_book_count) s_book_index = s_book_count - 1;
    return true;
}

static bool reader_add_page(const char *path, const char *member, bool from_zip) {
    if (!s_pages || s_page_count >= READER_MAX_PAGES) return false;
    reader_page_t *page = &s_pages[s_page_count++];
    snprintf(page->path, sizeof(page->path), "%s", path ? path : "");
    snprintf(page->member, sizeof(page->member), "%s", member ? member : "");
    page->from_zip = from_zip;
    return true;
}

static bool reader_build_text_pages(const char *path) {
    s_text_offsets = (long *)reader_alloc(sizeof(long) * READER_MAX_TEXT_PAGES);
    if (!s_text_offsets) return false;

    FILE *file = fopen(path, "rb");
    if (!file) return false;
    s_text_offsets[0] = 0;
    s_page_count = 1;

    int line = 0;
    int column = 0;
    int value;
    while ((value = fgetc(file)) != EOF) {
        if (value == '\r') continue;
        if (value == '\n') {
            ++line;
            column = 0;
        } else if (++column >= READER_TEXT_COLUMNS) {
            ++line;
            column = 0;
        }

        if (line >= READER_TEXT_LINES) {
            long offset = ftell(file);
            if (s_page_count < READER_MAX_TEXT_PAGES) {
                s_text_offsets[s_page_count++] = offset >= 0 ? offset : 0;
            }
            line = 0;
            column = 0;
        }
    }
    fclose(file);
    return true;
}

typedef struct {
    char id[64];
    char href[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    char media_type[64];
    bool nav;
} reader_epub_manifest_item_t;

static bool reader_zip_extract_named(lgfx_mz_zip_archive *zip, const char *name,
                                     size_t max_size, uint8_t **data, size_t *length) {
    if (!zip || !name || !data || !length) return false;
    *data = NULL; *length = 0;
    int index = lgfx_mz_zip_reader_locate_file(zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) return false;
    lgfx_mz_zip_archive_file_stat stat;
    memset(&stat, 0, sizeof(stat));
    if (!lgfx_mz_zip_reader_file_stat(zip, (lgfx_mz_uint)index, &stat) ||
        lgfx_mz_zip_reader_is_file_a_directory(zip, (lgfx_mz_uint)index) ||
        lgfx_mz_zip_reader_is_file_encrypted(zip, (lgfx_mz_uint)index) ||
        stat.m_uncomp_size > max_size || stat.m_uncomp_size > SIZE_MAX) return false;
    size_t size = (size_t)stat.m_uncomp_size;
    uint8_t *buffer = (uint8_t *)reader_alloc(size + 1);
    if (!buffer || !lgfx_mz_zip_reader_extract_to_mem(zip, (lgfx_mz_uint)index, buffer, size, 0)) {
        reader_free(buffer);
        return false;
    }
    buffer[size] = 0;
    *data = buffer; *length = size;
    return true;
}

static bool reader_xml_attr(const char *start, const char *end, const char *attribute,
                            char *out, size_t out_size) {
    size_t wanted = strlen(attribute);
    const char *p = start;
    while (p && p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        bool same = (p + wanted <= end);
        for (size_t i = 0; same && i < wanted; ++i) if (tolower((unsigned char)p[i]) != tolower((unsigned char)attribute[i])) same = false;
        if (p + wanted >= end || !same) {
            const char *next = memchr(p, ' ', (size_t)(end - p));
            const char *tab = memchr(p, '\t', (size_t)(end - p));
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!next || (tab && tab < next)) next = tab;
            if (!next || (nl && nl < next)) next = nl;
            if (!next) break;
            p = next + 1;
            continue;
        }
        const char *q = p + wanted;
        if (q < end && (*q == ' ' || *q == '\t' || *q == '=')) {
            while (q < end && isspace((unsigned char)*q)) ++q;
            if (q < end && *q == '=') ++q;
            while (q < end && isspace((unsigned char)*q)) ++q;
            if (q >= end || (*q != '\'' && *q != '"')) return false;
            char quote = *q++;
            const char *value_end = memchr(q, quote, (size_t)(end - q));
            if (!value_end) return false;
            size_t n = (size_t)(value_end - q);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, q, n); out[n] = 0;
            return true;
        }
        p += wanted;
    }
    return false;
}

static int reader_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void reader_epub_decode_href(char *text) {
    char *src = text, *dst = text;
    while (*src) {
        if (src[0] == '%' && reader_hex_value(src[1]) >= 0 && reader_hex_value(src[2]) >= 0) {
            *dst++ = (char)((reader_hex_value(src[1]) << 4) | reader_hex_value(src[2])); src += 3;
        } else *dst++ = *src++;
    }
    *dst = 0;
}

static bool reader_epub_resolve_path(const char *base_file, const char *href, char *out, size_t out_size) {
    if (!base_file || !href || !out || !out_size) return false;
    char relative[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    snprintf(relative, sizeof(relative), "%s", href);
    char *cut = strpbrk(relative, "?#"); if (cut) *cut = 0;
    reader_epub_decode_href(relative);
    char combined[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    const char *slash = strrchr(base_file, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - base_file + 1);
        if (dir_len + strlen(relative) >= sizeof(combined)) return false;
        memcpy(combined, base_file, dir_len); strcpy(combined + dir_len, relative);
    } else snprintf(combined, sizeof(combined), "%s", relative);
    char *parts[64]; int count = 0;
    char *cursor = combined;
    while (*cursor && count < 64) {
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        parts[count++] = cursor;
        char *next = strchr(cursor, '/');
        if (!next) break;
        *next = 0; cursor = next + 1;
    }
    out[0] = 0;
    for (int i = 0; i < count; ++i) {
        if (!strcmp(parts[i], ".")) continue;
        if (!strcmp(parts[i], "..")) { if (out[0]) { char *p = strrchr(out, '/'); if (p) *p = 0; else out[0] = 0; } continue; }
        size_t used = strlen(out);
        int written = snprintf(out + used, out_size - used, "%s%s", used ? "/" : "", parts[i]);
        if (written < 0 || (size_t)written >= out_size - used) return false;
    }
    return out[0] != 0;
}

static void reader_epub_append(char c) {
    if (s_epub_text_length + 1 >= READER_EPUB_TEXT_MAX_BYTES) return;
    s_epub_text[s_epub_text_length++] = c;
}

static size_t reader_utf8_length(const char *text, size_t remaining) {
    if (!text || !remaining) return 0;
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF && remaining >= 2 && ((unsigned char)text[1] & 0xC0) == 0x80) return 2;
    if (c >= 0xE0 && c <= 0xEF && remaining >= 3 && ((unsigned char)text[1] & 0xC0) == 0x80 && ((unsigned char)text[2] & 0xC0) == 0x80) return 3;
    if (c >= 0xF0 && c <= 0xF4 && remaining >= 4 && ((unsigned char)text[1] & 0xC0) == 0x80 && ((unsigned char)text[2] & 0xC0) == 0x80 && ((unsigned char)text[3] & 0xC0) == 0x80) return 4;
    return 1;
}

static void reader_epub_append_text(const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c < 32) continue;
        if (c == ' ' && s_epub_text_length && s_epub_text[s_epub_text_length - 1] == ' ') continue;
        reader_epub_append((char)c);
    }
}

static bool reader_epub_block_tag(const char *name) {
    return !reader_strcasecmp(name, "p") || !reader_strcasecmp(name, "div") ||
           !reader_strcasecmp(name, "section") || !reader_strcasecmp(name, "article") ||
           !reader_strcasecmp(name, "li") || !reader_strcasecmp(name, "blockquote") ||
           !reader_strcasecmp(name, "br") || !reader_strcasecmp(name, "tr") ||
           (name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
}

static void reader_epub_append_entity(const char *entity, size_t length) {
    if (length == 3 && !memcmp(entity, "amp", 3)) { reader_epub_append('&'); return; }
    if (length == 2 && !memcmp(entity, "lt", 2)) { reader_epub_append('<'); return; }
    if (length == 2 && !memcmp(entity, "gt", 2)) { reader_epub_append('>'); return; }
    if (length == 4 && !memcmp(entity, "quot", 4)) { reader_epub_append('"'); return; }
    if (length == 4 && !memcmp(entity, "apos", 4)) { reader_epub_append('\''); return; }
    if (length > 2 && entity[0] == '#') {
        unsigned long value = 0;
        int base = (entity[1] == 'x' || entity[1] == 'X') ? 16 : 10;
        size_t start = base == 16 ? 2 : 1;
        for (size_t i = start; i < length; ++i) {
            int digit = base == 16 ? reader_hex_value(entity[i]) : (entity[i] >= '0' && entity[i] <= '9' ? entity[i] - '0' : -1);
            if (digit < 0) { value = 0; break; }
            value = value * (unsigned)base + (unsigned)digit;
        }
        if (value > 0 && value <= 0x10FFFF) {
            if (value < 0x80) reader_epub_append((char)value);
            else if (value < 0x800) { reader_epub_append((char)(0xC0 | (value >> 6))); reader_epub_append((char)(0x80 | (value & 0x3F))); }
            else if (value < 0x10000) { reader_epub_append((char)(0xE0 | (value >> 12))); reader_epub_append((char)(0x80 | ((value >> 6) & 0x3F))); reader_epub_append((char)(0x80 | (value & 0x3F))); }
            else { reader_epub_append((char)(0xF0 | (value >> 18))); reader_epub_append((char)(0x80 | ((value >> 12) & 0x3F))); reader_epub_append((char)(0x80 | ((value >> 6) & 0x3F))); reader_epub_append((char)(0x80 | (value & 0x3F))); }
            return;
        }
    }
    reader_epub_append('&'); reader_epub_append_text(entity, length); reader_epub_append(';');
}

static void reader_epub_append_xhtml(const uint8_t *data, size_t length) {
    bool hidden = false;
    size_t i = 0;
    while (i < length) {
        if (data[i] == '<') {
            const uint8_t *end = memchr(data + i, '>', length - i);
            if (!end) break;
            char tag[24] = {0}; const char *p = (const char *)data + i + 1;
            while (p < (const char *)end && isspace((unsigned char)*p)) ++p;
            if (*p == '/') ++p;
            size_t n = 0; while (p < (const char *)end && isalpha((unsigned char)*p) && n + 1 < sizeof(tag)) tag[n++] = (char)tolower((unsigned char)*p++);
            tag[n] = 0;
            bool closing = data[i + 1] == '/';
            if (!reader_strcasecmp(tag, "script") || !reader_strcasecmp(tag, "style") || !reader_strcasecmp(tag, "head") || !reader_strcasecmp(tag, "svg")) hidden = !closing;
            if (!hidden && reader_epub_block_tag(tag) && s_epub_text_length && s_epub_text[s_epub_text_length - 1] != '\n') reader_epub_append('\n');
            i = (size_t)(end - data) + 1; continue;
        }
        if (!hidden) {
            if (data[i] == '&') {
                const uint8_t *end = memchr(data + i + 1, ';', length - i - 1);
                if (end) { reader_epub_append_entity((const char *)data + i + 1, (size_t)(end - (data + i + 1))); i = (size_t)(end - data) + 1; continue; }
            }
            reader_epub_append((char)data[i]);
        }
        ++i;
    }
}

static bool reader_build_memory_text_pages(void) {
    s_text_offsets = (long *)reader_alloc(sizeof(long) * READER_MAX_TEXT_PAGES);
    if (!s_text_offsets || !s_epub_text_length) return false;
    s_text_offsets[0] = 0; s_page_count = 1;
    int line = 0, column = 0;
    for (size_t i = 0; i < s_epub_text_length; ) {
        size_t char_len = reader_utf8_length(s_epub_text + i, s_epub_text_length - i);
        if (s_epub_text[i] == '\n') { ++line; column = 0; }
        else if (++column >= READER_TEXT_COLUMNS) { ++line; column = 0; }
        i += char_len;
        if (line >= READER_TEXT_LINES) {
            if (s_page_count < READER_MAX_TEXT_PAGES) s_text_offsets[s_page_count++] = (long)i;
            line = 0; column = 0;
        }
    }
    return true;
}

static bool reader_build_epub_pages(const char *path) {
    bool mounted_here = false, suspended = false;
    if (!reader_sd_begin(&mounted_here, &suspended)) return false;
    /* Keep miniz state off the 8 KiB LVGL task stack. */
    lgfx_mz_zip_archive *zip = reader_alloc(sizeof(*zip));
    if (!zip) { reader_sd_end(mounted_here, suspended); return false; }
    memset(zip, 0, sizeof(*zip));
    uint8_t *container = NULL, *opf = NULL; size_t container_len = 0, opf_len = 0;
    bool ok = lgfx_mz_zip_reader_init_file(zip, path, 0);
    char opf_path[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE] = {0};
    if (ok && reader_zip_extract_named(zip, "META-INF/container.xml", READER_EPUB_CONTAINER_MAX_BYTES, &container, &container_len)) {
        const char *root = strstr((const char *)container, "<rootfile");
        const char *root_end = root ? strchr(root, '>') : NULL;
        if (root && root_end) reader_xml_attr(root, root_end, "full-path", opf_path, sizeof(opf_path));
    }
    if (ok && !opf_path[0]) {
        lgfx_mz_uint total = lgfx_mz_zip_reader_get_num_files(zip);
        for (lgfx_mz_uint i = 0; i < total; ++i) { lgfx_mz_zip_archive_file_stat st; memset(&st, 0, sizeof(st)); if (lgfx_mz_zip_reader_file_stat(zip, i, &st) && reader_extension_is(st.m_filename, "opf")) { snprintf(opf_path, sizeof(opf_path), "%s", st.m_filename); break; } }
    }
    if (ok && opf_path[0]) ok = reader_zip_extract_named(zip, opf_path, READER_EPUB_OPF_MAX_BYTES, &opf, &opf_len);
    reader_free(container);
    if (!ok || !opf) { if (ok) lgfx_mz_zip_reader_end(zip); reader_free(opf); reader_free(zip); reader_sd_end(mounted_here, suspended); ESP_LOGW(TAG, "EPUB package metadata unavailable: %s", path); return false; }

    reader_epub_manifest_item_t *manifest = reader_alloc(sizeof(*manifest) * READER_EPUB_MAX_MANIFEST);
    char (*spine_ids)[64] = reader_alloc(sizeof(*spine_ids) * READER_MAX_PAGES);
    if (!manifest || !spine_ids) { reader_free(manifest); reader_free(spine_ids); reader_free(opf); lgfx_mz_zip_reader_end(zip); reader_free(zip); reader_sd_end(mounted_here, suspended); return false; }
    int manifest_count = 0, spine_count = 0;
    const char *manifest_start = strstr((const char *)opf, "<manifest"); const char *manifest_end = manifest_start ? strstr(manifest_start, "</manifest") : NULL;
    for (const char *p = manifest_start; p && manifest_end && p < manifest_end && manifest_count < READER_EPUB_MAX_MANIFEST; ) {
        const char *tag = strstr(p, "<item"); if (!tag || tag >= manifest_end) break; const char *tag_end = strchr(tag, '>'); if (!tag_end) break;
        reader_epub_manifest_item_t *item = &manifest[manifest_count]; memset(item, 0, sizeof(*item));
        reader_xml_attr(tag + 5, tag_end, "id", item->id, sizeof(item->id)); reader_xml_attr(tag + 5, tag_end, "href", item->href, sizeof(item->href)); reader_xml_attr(tag + 5, tag_end, "media-type", item->media_type, sizeof(item->media_type));
        char properties[64] = {0}; reader_xml_attr(tag + 5, tag_end, "properties", properties, sizeof(properties)); item->nav = strstr(properties, "nav") != NULL;
        if (item->id[0] && item->href[0]) {
            ++manifest_count;
        }
        p = tag_end + 1;
    }
    const char *spine_start = strstr((const char *)opf, "<spine"); const char *spine_end = spine_start ? strstr(spine_start, "</spine") : NULL;
    for (const char *p = spine_start; p && spine_end && p < spine_end && spine_count < READER_MAX_PAGES; ) {
        const char *tag = strstr(p, "<itemref"); if (!tag || tag >= spine_end) break; const char *tag_end = strchr(tag, '>'); if (!tag_end) break;
        char idref[64] = {0}, linear[16] = {0}; reader_xml_attr(tag + 8, tag_end, "idref", idref, sizeof(idref)); reader_xml_attr(tag + 8, tag_end, "linear", linear, sizeof(linear));
        if (idref[0] && reader_strcasecmp(linear, "no")) {
            snprintf(spine_ids[spine_count++], 64, "%s", idref);
        }
        p = tag_end + 1;
    }
    if (!spine_count) for (int i = 0; i < manifest_count && spine_count < READER_MAX_PAGES; ++i) if (!manifest[i].nav && (strstr(manifest[i].media_type, "xhtml") || reader_extension_is(manifest[i].href, "html") || reader_extension_is(manifest[i].href, "xhtml"))) snprintf(spine_ids[spine_count++], 64, "%s", manifest[i].id);
    s_epub_text = reader_alloc(READER_EPUB_TEXT_MAX_BYTES + 1); s_epub_text_length = 0;
    if (!s_epub_text) {
        reader_free(manifest); reader_free(spine_ids); reader_free(opf);
        lgfx_mz_zip_reader_end(zip); reader_free(zip); reader_sd_end(mounted_here, suspended);
        return false;
    }
    for (int s = 0; s < spine_count; ++s) for (int m = 0; m < manifest_count; ++m) if (!strcmp(spine_ids[s], manifest[m].id) && !manifest[m].nav) {
        char chapter_path[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE]; if (!reader_epub_resolve_path(opf_path, manifest[m].href, chapter_path, sizeof(chapter_path))) continue;
        uint8_t *chapter = NULL; size_t chapter_len = 0; if (reader_zip_extract_named(zip, chapter_path, READER_EPUB_CHAPTER_MAX_BYTES, &chapter, &chapter_len)) { if (s_epub_text_length) reader_epub_append('\n'); reader_epub_append_xhtml(chapter, chapter_len); reader_free(chapter); }
    }
    if (s_epub_text) s_epub_text[s_epub_text_length] = 0;
    bool built = reader_build_memory_text_pages();
    reader_free(manifest); reader_free(spine_ids); reader_free(opf); lgfx_mz_zip_reader_end(zip); reader_free(zip); reader_sd_end(mounted_here, suspended);
    ESP_LOGI(TAG, "EPUB spine ready: %d chapters, %d text pages", spine_count, built ? s_page_count : 0);
    return built;
}

static bool reader_build_pages(const reader_book_t *book) {
    reader_free_pages();
    if (!book) return false;

    if (book->kind == READER_BOOK_TEXT) return reader_build_text_pages(book->path);
    if (book->kind == READER_BOOK_EPUB) return reader_build_epub_pages(book->path);

    s_pages = (reader_page_t *)reader_alloc(sizeof(reader_page_t) * READER_MAX_PAGES);
    if (!s_pages) return false;

    if (book->kind == READER_BOOK_IMAGE) {
        reader_add_page(book->path, NULL, false);
    } else if (book->kind == READER_BOOK_FOLDER) {
        bool mounted_here = false;
        bool display_was_suspended = false;
        if (!reader_sd_begin(&mounted_here, &display_was_suspended)) return false;
        DIR *dir = opendir(book->path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.' || !reader_is_image_name(entry->d_name)) continue;
                char page_path[READER_MAX_PATH];
                if (!reader_join_path(page_path, sizeof(page_path), book->path, entry->d_name)) continue;
                struct stat st;
                if (stat(page_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    reader_add_page(page_path, NULL, false);
                }
            }
            closedir(dir);
        }
        reader_sd_end(mounted_here, display_was_suspended);
    } else if (book->kind == READER_BOOK_CBZ) {
        bool mounted_here = false;
        bool display_was_suspended = false;
        if (!reader_sd_begin(&mounted_here, &display_was_suspended)) return false;

        ESP_LOGI(TAG, "Opening CBZ: %s", book->path);
        lgfx_mz_zip_archive *zip = reader_alloc(sizeof(*zip));
        if (zip) memset(zip, 0, sizeof(*zip));
        if (zip && lgfx_mz_zip_reader_init_file(zip, book->path, 0)) {
            lgfx_mz_uint total = lgfx_mz_zip_reader_get_num_files(zip);
            ESP_LOGI(TAG, "CBZ index ready: %u entries", (unsigned)total);
            for (lgfx_mz_uint i = 0; i < total && s_page_count < READER_MAX_PAGES; ++i) {
                lgfx_mz_zip_archive_file_stat stat;
                memset(&stat, 0, sizeof(stat));
                if (!lgfx_mz_zip_reader_file_stat(zip, i, &stat) ||
                    lgfx_mz_zip_reader_is_file_a_directory(zip, i) ||
                    lgfx_mz_zip_reader_is_file_encrypted(zip, i) ||
                    !reader_is_image_name(stat.m_filename)) continue;
                reader_add_page(book->path, stat.m_filename, true);
            }
            lgfx_mz_zip_reader_end(zip);
            ESP_LOGI(TAG, "CBZ page list ready: %d image pages", s_page_count);
        } else {
            ESP_LOGW(TAG, "Cannot open CBZ %s", book->path);
        }
        reader_free(zip);
        reader_sd_end(mounted_here, display_was_suspended);
    }

    if (s_page_count > 1) qsort(s_pages, (size_t)s_page_count, sizeof(*s_pages), reader_page_compare);
    return s_page_count > 0;
}

static bool reader_load_file(const char *path, uint8_t **data, size_t *length) {
    if (!path || !data || !length) return false;
    *data = NULL;
    *length = 0;

    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < 0 || (uint64_t)file_size > READER_MAX_FILE_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    size_t size = (size_t)file_size;
    uint8_t *buffer = (uint8_t *)reader_alloc(size ? size : 1);
    if (!buffer || (size && fread(buffer, 1, size, file) != size)) {
        reader_free(buffer);
        fclose(file);
        return false;
    }
    fclose(file);
    *data = buffer;
    *length = size;
    return true;
}

static bool reader_load_page_data(const reader_page_t *page, uint8_t **data, size_t *length) {
    if (!page || !data || !length) return false;
    if (!page->from_zip) return reader_load_file(page->path, data, length);

    *data = NULL;
    *length = 0;
    ESP_LOGI(TAG, "Extracting CBZ page: %s", page->member);
    lgfx_mz_zip_archive *zip = reader_alloc(sizeof(*zip));
    if (!zip) return false;
    memset(zip, 0, sizeof(*zip));
    bool ok = false;
    if (!lgfx_mz_zip_reader_init_file(zip, page->path, 0)) { reader_free(zip); return false; }

    int file_index = lgfx_mz_zip_reader_locate_file(zip, page->member, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (file_index >= 0) {
        lgfx_mz_zip_archive_file_stat stat;
        memset(&stat, 0, sizeof(stat));
        if (lgfx_mz_zip_reader_file_stat(zip, (lgfx_mz_uint)file_index, &stat) &&
            !lgfx_mz_zip_reader_is_file_encrypted(zip, (lgfx_mz_uint)file_index) &&
            stat.m_uncomp_size <= READER_MAX_FILE_BYTES &&
            stat.m_uncomp_size <= SIZE_MAX) {
            size_t size = (size_t)stat.m_uncomp_size;
            uint8_t *buffer = (uint8_t *)reader_alloc(size ? size : 1);
            if (buffer && lgfx_mz_zip_reader_extract_to_mem(zip, (lgfx_mz_uint)file_index,
                                                              buffer, size, 0)) {
                *data = buffer;
                *length = size;
                ok = true;
                ESP_LOGI(TAG, "CBZ page extracted: %u bytes", (unsigned)size);
            } else {
                reader_free(buffer);
            }
        }
    }
    lgfx_mz_zip_reader_end(zip);
    reader_free(zip);
    return ok;
}

static void reader_fill_white(void) {
    if (!s_image_pixels) return;
    size_t count = (size_t)s_image_width * s_image_height;
    lv_color_t white = lv_color_make(255, 255, 255);
    for (size_t i = 0; i < count; ++i) s_image_pixels[i] = white;
}

static bool reader_ensure_image_buffer(void) {
    uint16_t width = LV_HOR_RES;
    uint16_t height = LV_VER_RES - GUI_STATUS_BAR_H - READER_FOOTER_H;
    if (s_page_area) {
        lv_coord_t area_width = lv_obj_get_width(s_page_area);
        lv_coord_t area_height = lv_obj_get_height(s_page_area);
        /* A view can be rendered before LVGL has completed layout. Keep the
           explicit e-paper dimensions instead of treating that transient zero
           as a decoder failure. */
        if (area_width > 0) width = (uint16_t)area_width;
        if (area_height > 0) height = (uint16_t)area_height;
    }
    if (!width || !height) {
        ESP_LOGW(TAG, "Invalid reader page area: %ux%u", (unsigned)width, (unsigned)height);
        return false;
    }
    if (!s_image_pixels || width != s_image_width || height != s_image_height) {
        reader_free_image();
        s_image_pixels = (lv_color_t *)reader_alloc((size_t)width * height * sizeof(lv_color_t));
        if (!s_image_pixels) {
            ESP_LOGW(TAG, "Reader image buffer allocation failed: %ux%u (%u bytes)",
                     (unsigned)width, (unsigned)height,
                     (unsigned)((size_t)width * height * sizeof(lv_color_t)));
            return false;
        }
        s_image_width = width;
        s_image_height = height;
        memset(&s_image_dsc, 0, sizeof(s_image_dsc));
        s_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_image_dsc.header.w = width;
        s_image_dsc.header.h = height;
        s_image_dsc.data_size = (uint32_t)((size_t)width * height * sizeof(lv_color_t));
        s_image_dsc.data = (const uint8_t *)s_image_pixels;
    }
    reader_fill_white();
    return true;
}

static uint32_t reader_jpeg_input(void *device, uint8_t *buffer, uint32_t length) {
    reader_jpeg_session_t *input = (reader_jpeg_session_t *)device;
    if (!input || input->position >= input->length) return 0;
    size_t available = input->length - input->position;
    if ((size_t)length > available) length = (uint32_t)available;
    /* TJpgDec passes a NULL buffer when it only needs to skip an APP/COM
       metadata segment. The stream still has to advance in that case. */
    if (buffer && length) memcpy(buffer, input->data + input->position, length);
    input->position += length;
    return length;
}

static bool reader_store_rgb_block(reader_jpeg_session_t *output,
                                   int left, int top, int width, int height,
                                   const uint8_t *source) {
    if (!output || !source || !s_image_pixels || width <= 0 || height <= 0 ||
        output->source_width <= 0 || output->source_height <= 0) return false;

    for (int y = 0; y < height; ++y) {
        int source_y = top + y;
        int rendered_y = (int)(((int64_t)source_y * output->render_height) / output->source_height);
        int destination_y = output->y + rendered_y;
        for (int x = 0; x < width; ++x) {
            int source_x = left + x;
            int rendered_x = (int)(((int64_t)source_x * output->render_width) / output->source_width);
            int destination_x = output->x + rendered_x;
            const uint8_t *pixel = source + ((size_t)y * width + x) * 3;
            uint8_t gray = (uint8_t)(((uint16_t)pixel[0] * 30u +
                                      (uint16_t)pixel[1] * 59u +
                                      (uint16_t)pixel[2] * 11u) / 100u);
            /* Keep anti-aliased lettering attached to the stroke when it is
               reduced to the SSD1683's 1-bit panel. */
            uint8_t ink = gray < READER_COMIC_INK_THRESHOLD;
            uint8_t value = READER_COMIC_INVERT ? (ink ? 255 : 0) : (ink ? 0 : 255);
            if (s_comic_render_cache && rendered_x >= 0 && rendered_y >= 0 &&
                rendered_x < s_comic_render_cache_width && rendered_y < s_comic_render_cache_height) {
                s_comic_render_cache[(size_t)rendered_y * s_comic_render_cache_width + rendered_x] = value;
            }
            if (destination_x >= 0 && destination_x < output->width &&
                destination_y >= 0 && destination_y < output->height) {
                s_image_pixels[(size_t)destination_y * output->width + destination_x] =
                    lv_color_make(value, value, value);
            }
        }
    }
    return true;
}

static bool reader_copy_cached_comic_tile(const reader_jpeg_session_t *session,
                                          uint32_t tile_x, uint32_t tile_y) {
    if (!session || !s_comic_render_cache ||
        session->render_width != s_comic_render_cache_width ||
        session->render_height != s_comic_render_cache_height) return false;
    reader_fill_white();
    for (int y = 0; y < session->height; ++y) {
        int source_y = (int)tile_y + y;
        if (source_y < 0 || source_y >= session->render_height) continue;
        for (int x = 0; x < session->width; ++x) {
            int source_x = (int)tile_x + x;
            if (source_x < 0 || source_x >= session->render_width) continue;
            uint8_t value = s_comic_render_cache[(size_t)source_y * session->render_width + source_x];
            s_image_pixels[(size_t)y * session->width + x] = lv_color_make(value, value, value);
        }
    }
    return true;
}

#if !CONFIG_ESP_ROM_HAS_JPEG_DECODE
static uint32_t reader_jpeg_output(void *device, void *bitmap, JRECT *rect) {
    reader_jpeg_session_t *output = (reader_jpeg_session_t *)device;
    if (!output || !bitmap || !rect) return 0;
    return reader_store_rgb_block(output,
                                  (int)rect->left, (int)rect->top,
                                  (int)(rect->right - rect->left + 1),
                                  (int)(rect->bottom - rect->top + 1),
                                  (const uint8_t *)bitmap) ? 1u : 0u;
}
#endif

#if CONFIG_ESP_ROM_HAS_JPEG_DECODE
static size_t reader_rom_jpeg_input(void *arg, size_t index, uint8_t *buffer, size_t length) {
    reader_jpeg_session_t *input = (reader_jpeg_session_t *)arg;
    if (!input || index >= input->length) return 0;
    size_t available = input->length - index;
    if (length > available) length = available;
    if (buffer && length) memcpy(buffer, input->data + index, length);
    return length;
}

static bool reader_rom_jpeg_output(void *arg, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height, uint8_t *data) {
    /* esp_jpg_decode uses NULL data for its begin/end notifications. */
    if (!data) return true;
    return reader_store_rgb_block((reader_jpeg_session_t *)arg,
                                  x, y, width, height, data);
}
#endif

static bool reader_decode_jpeg(const uint8_t *data, size_t length, bool comic_mode) {
    s_comic_split_active = false;
    reader_jpeg_session_t session = {.data = data, .length = length, .position = 0};
    lgfxJdec decoder;
    memset(&decoder, 0, sizeof(decoder));
    uint8_t *pool = (uint8_t *)reader_alloc(READER_JPEG_POOL_BYTES);
    if (!pool) {
        ESP_LOGW(TAG, "JPEG decoder pool allocation failed: %u bytes", READER_JPEG_POOL_BYTES);
        return false;
    }

    JRESULT result = lgfx_jd_prepare(&decoder, reader_jpeg_input, pool,
                                     READER_JPEG_POOL_BYTES, &session);
    if (result != JDR_OK) {
        ESP_LOGW(TAG, "JPEG header rejected: result=%d, bytes=%u, signature=%02x %02x",
                 (int)result, (unsigned)length,
                 length > 0 ? data[0] : 0, length > 1 ? data[1] : 0);
        reader_free(pool);
        return false;
    }

    uint8_t scale = 0;
    uint32_t output_width;
    uint32_t output_height;
    bool portrait = comic_mode && decoder.height > decoder.width;

    if (portrait) {
        /* Preserve detail for portrait comic pages. Overview uses the full
           panel width; detail backs off one decoder scale step and presents
           the page as row-major tiles. */
        while (scale < 3) {
            uint32_t next_width = ((uint32_t)decoder.width + ((1u << (scale + 1)) - 1u)) >> (scale + 1);
            if (next_width < s_image_width) break;
            ++scale;
        }
        /* One scale step less gives approximately 2x the overview size.
         * The panel then shows a sequence of tiles instead of destroying
         * detail by shrinking the complete page into one frame. */
        if (s_comic_detail_active && scale > 0) {
            --scale;
        }
        output_width = ((uint32_t)decoder.width + ((1u << scale) - 1u)) >> scale;
        output_height = ((uint32_t)decoder.height + ((1u << scale) - 1u)) >> scale;
        s_comic_split_active = output_height > s_image_height + 16u;
    } else {
        while (scale < 3 &&
               ((((uint32_t)decoder.width + ((1u << scale) - 1u)) >> scale) > s_image_width ||
                (((uint32_t)decoder.height + ((1u << scale) - 1u)) >> scale) > s_image_height)) {
            ++scale;
        }
        output_width = ((uint32_t)decoder.width + ((1u << scale) - 1u)) >> scale;
        output_height = ((uint32_t)decoder.height + ((1u << scale) - 1u)) >> scale;
    }

    int display_width = (int)output_width;
    int display_height = (int)output_height;
    int render_width = display_width;
    int render_height = display_height;

    if (portrait) {
        /* Overview is always no wider than the panel. Detail deliberately
           keeps the larger render so the tile window can move left-to-right
           without throwing away the page edges. */
        if (!s_comic_detail_active) {
            if (render_width > (int)s_image_width) render_width = s_image_width;
            render_height = (int)(((int64_t)display_height * render_width + display_width / 2) / display_width);
        }
    } else if (render_width > (int)s_image_width || render_height > (int)s_image_height) {
        int64_t width_limited = ((int64_t)render_width * s_image_height) / render_height;
        if (width_limited < s_image_width) {
            render_width = (int)width_limited;
            render_height = s_image_height;
        } else {
            render_width = s_image_width;
            render_height = (int)(((int64_t)display_height * render_width + display_width / 2) / display_width);
        }
    }

    s_comic_tile_columns = 1;
    s_comic_section_count = 1;
    if (portrait) {
        if (s_comic_detail_active) {
            s_comic_tile_columns = (uint8_t)((render_width + s_image_width - 1) / s_image_width);
            uint32_t rows = ((uint32_t)render_height + s_image_height - 1u) / s_image_height;
            uint32_t tile_count = (uint32_t)s_comic_tile_columns * rows;
            if (tile_count > READER_COMIC_DETAIL_MAX_TILES) {
                ESP_LOGW(TAG, "Comic detail tile count %u exceeds limit; clamping", (unsigned)tile_count);
                tile_count = READER_COMIC_DETAIL_MAX_TILES;
            }
            s_comic_section_count = (uint8_t)(tile_count ? tile_count : 1u);
        } else {
            uint32_t section_count = ((uint32_t)render_height + s_image_height - 1u) / s_image_height;
            s_comic_section_count = (uint8_t)(section_count > 255u ? 255u : (section_count ? section_count : 1u));
        }
        if (s_comic_section >= s_comic_section_count) s_comic_section = 0;
        s_comic_split_active = s_comic_section_count > 1;
    }

    uint32_t tile_x = 0;
    uint32_t tile_y = 0;
    if (s_comic_split_active) {
        if (s_comic_detail_active) {
            tile_x = (uint32_t)(s_comic_section % s_comic_tile_columns) * s_image_width;
            tile_y = (uint32_t)(s_comic_section / s_comic_tile_columns) * s_image_height;
        } else {
            tile_y = (uint32_t)s_comic_section * s_image_height;
        }
    }
    ESP_LOGI(TAG, "Decoding JPEG: %ux%u -> %dx%d, render %dx%d (scale=%u, %s%s)",
             (unsigned)decoder.width, (unsigned)decoder.height,
             display_width, display_height, render_width, render_height, (unsigned)scale,
             s_comic_split_active ? (s_comic_detail_active ? "detail tile=" : "section=") : "full",
             s_comic_split_active ? "active" : "");
    session.x = s_comic_split_active ? -(int)tile_x : ((int)s_image_width - render_width) / 2;
    session.y = s_comic_split_active ? -(int)tile_y : ((int)s_image_height - render_height) / 2;
    session.width = s_image_width;
    session.height = s_image_height;
    session.source_width = display_width;
    session.source_height = display_height;
    session.render_width = render_width;
    session.render_height = render_height;

    /* Cache every split portrait render, not only zoom/detail renders. The
     * normal PART view used to decode the same JPEG again for each vertical
     * section, while ZOOM already benefited from this cache. */
    if (portrait && s_comic_split_active && render_width <= UINT16_MAX && render_height <= UINT16_MAX) {
        if (s_comic_render_cache_page == s_page_index &&
            s_comic_render_cache_width == render_width &&
            s_comic_render_cache_height == render_height && s_comic_render_cache) {
            bool copied = reader_copy_cached_comic_tile(&session, tile_x, tile_y);
            reader_free(pool);
            if (copied) {
                ESP_LOGI(TAG, "Using cached comic tile %u/%u", (unsigned)(s_comic_section + 1), (unsigned)s_comic_section_count);
                return true;
            }
        } else {
            reader_free_comic_render_cache();
            size_t cache_size = (size_t)render_width * (size_t)render_height;
            if (cache_size <= READER_PAGE_CACHE_MAX_BYTES) {
                s_comic_render_cache = reader_alloc(cache_size);
                if (s_comic_render_cache) {
                    s_comic_render_cache_width = (uint16_t)render_width;
                    s_comic_render_cache_height = (uint16_t)render_height;
                    s_comic_render_cache_page = s_page_index;
                    memset(s_comic_render_cache, 255, cache_size);
                    ESP_LOGI(TAG, "Allocated comic render cache: %ux%u (%u bytes)",
                             (unsigned)render_width, (unsigned)render_height, (unsigned)cache_size);
                }
            }
        }
    } else if (!s_comic_split_active) {
        reader_free_comic_render_cache();
    }

#if CONFIG_ESP_ROM_HAS_JPEG_DECODE
    /* The S3 ROM decoder is substantially faster than the software TJpgDec
       loop. Keep the software prepare pass for dimensions/format validation,
       then let the ROM perform the actual MCU decode. */
    reader_free(pool);
    ESP_LOGI(TAG, "JPEG decode: ESP32-S3 ROM path");
    esp_err_t rom_result = esp_jpg_decode(length, (jpg_scale_t)scale,
                                          reader_rom_jpeg_input,
                                          reader_rom_jpeg_output, &session);
    if (rom_result != ESP_OK) {
        ESP_LOGW(TAG, "JPEG ROM decode failed: %s", esp_err_to_name(rom_result));
    }
    return rom_result == ESP_OK;
#else
    result = lgfx_jd_decomp(&decoder, reader_jpeg_output, scale);
    if (result != JDR_OK) {
        ESP_LOGW(TAG, "JPEG decode failed: result=%d", (int)result);
    }
    reader_free(pool);
    return result == JDR_OK;
#endif
}

static uint16_t reader_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t reader_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint8_t reader_gray_from_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint8_t)(((uint16_t)red * 30u + (uint16_t)green * 59u + (uint16_t)blue * 11u) / 100u);
}

static void reader_plot_gray(int x, int y, uint8_t gray) {
    if (!s_image_pixels || x < 0 || y < 0 || x >= s_image_width || y >= s_image_height) return;
    uint8_t value = gray < 160 ? 0 : 255;
    s_image_pixels[(size_t)y * s_image_width + x] = lv_color_make(value, value, value);
}

static bool reader_decode_bmp(const uint8_t *data, size_t length) {
    if (!data || length < 54 || data[0] != 'B' || data[1] != 'M') return false;
    uint32_t pixel_offset = reader_u32(data + 10);
    uint32_t dib_size = reader_u32(data + 14);
    if (dib_size < 40 || 14u + dib_size > length || pixel_offset >= length) return false;

    int32_t source_width = (int32_t)reader_u32(data + 18);
    int32_t source_height = (int32_t)reader_u32(data + 22);
    uint16_t planes = reader_u16(data + 26);
    uint16_t bits = reader_u16(data + 28);
    uint32_t compression = reader_u32(data + 30);
    uint32_t colors_used = reader_u32(data + 46);
    if (source_width <= 0 || source_height == 0 || planes != 1 || compression != 0 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32)) return false;

    bool top_down = source_height < 0;
    uint32_t source_h = (uint32_t)(top_down ? -source_height : source_height);
    uint32_t source_w = (uint32_t)source_width;
    uint64_t row_bytes = (((uint64_t)source_w * bits + 31u) / 32u) * 4u;
    if (row_bytes == 0 || row_bytes * source_h > SIZE_MAX || pixel_offset + row_bytes * source_h > length) return false;

    uint32_t palette_count = colors_used;
    if (!palette_count && bits <= 8) palette_count = 1u << bits;
    const uint8_t *palette = data + 14 + dib_size;
    if (bits <= 8 && (size_t)(palette - data) + (size_t)palette_count * 4u > length) return false;

    for (int y = 0; y < s_image_height; ++y) {
        uint32_t source_y = ((uint32_t)y * source_h) / s_image_height;
        if (!top_down) source_y = source_h - 1u - source_y;
        const uint8_t *row = data + pixel_offset + (size_t)source_y * (size_t)row_bytes;
        for (int x = 0; x < s_image_width; ++x) {
            uint32_t source_x = ((uint32_t)x * source_w) / s_image_width;
            uint8_t red = 255, green = 255, blue = 255;
            if (bits == 24 || bits == 32) {
                const uint8_t *pixel = row + (size_t)source_x * (bits / 8u);
                blue = pixel[0];
                green = pixel[1];
                red = pixel[2];
            } else {
                uint32_t palette_index;
                if (bits == 8) palette_index = row[source_x];
                else if (bits == 4) palette_index = (row[source_x / 2u] >> ((source_x & 1u) ? 0 : 4)) & 0x0Fu;
                else palette_index = (row[source_x / 8u] >> (7u - (source_x & 7u))) & 1u;
                if (palette_index >= palette_count) return false;
                const uint8_t *color = palette + palette_index * 4u;
                blue = color[0];
                green = color[1];
                red = color[2];
            }
            reader_plot_gray(x, y, reader_gray_from_rgb(red, green, blue));
        }
    }
    return true;
}

static const uint8_t *reader_pbm_token(const uint8_t *cursor, const uint8_t *end,
                                       char *token, size_t token_size) {
    while (cursor < end) {
        if (isspace((unsigned char)*cursor)) {
            ++cursor;
        } else if (*cursor == '#') {
            while (cursor < end && *cursor != '\n') ++cursor;
        } else {
            break;
        }
    }
    if (cursor >= end || !token || token_size == 0) return NULL;
    size_t used = 0;
    while (cursor < end && !isspace((unsigned char)*cursor) && *cursor != '#') {
        if (used + 1 < token_size) token[used++] = (char)*cursor;
        ++cursor;
    }
    token[used] = '\0';
    return cursor;
}

static bool reader_decode_pbm(const uint8_t *data, size_t length) {
    if (!data || length < 3) return false;
    char magic[3], width_text[16], height_text[16];
    const uint8_t *cursor = data;
    const uint8_t *end = data + length;
    cursor = reader_pbm_token(cursor, end, magic, sizeof(magic));
    if (!cursor || strcmp(magic, "P4") != 0) return false;
    cursor = reader_pbm_token(cursor, end, width_text, sizeof(width_text));
    cursor = cursor ? reader_pbm_token(cursor, end, height_text, sizeof(height_text)) : NULL;
    if (!cursor) return false;

    char *parse_end = NULL;
    long source_width = strtol(width_text, &parse_end, 10);
    if (!parse_end || *parse_end || source_width <= 0) return false;
    long source_height = strtol(height_text, &parse_end, 10);
    if (!parse_end || *parse_end || source_height <= 0) return false;

    /* The token parser leaves the header separator in place. Consume the one
     * required separator and do not skip arbitrary payload bytes. */
    if (cursor < end && isspace((unsigned char)*cursor)) {
        if (*cursor == '\r' && cursor + 1 < end && cursor[1] == '\n') cursor += 2;
        else ++cursor;
    }
    size_t row_bytes = ((size_t)source_width + 7u) / 8u;
    if (row_bytes == 0 || (size_t)source_height > (length - (size_t)(cursor - data)) / row_bytes) return false;

    for (int y = 0; y < s_image_height; ++y) {
        uint32_t source_y = ((uint32_t)y * (uint32_t)source_height) / s_image_height;
        const uint8_t *row = cursor + (size_t)source_y * row_bytes;
        for (int x = 0; x < s_image_width; ++x) {
            uint32_t source_x = ((uint32_t)x * (uint32_t)source_width) / s_image_width;
            bool black = (row[source_x / 8u] & (uint8_t)(0x80u >> (source_x & 7u))) != 0;
            reader_plot_gray(x, y, black ? 0 : 255);
        }
    }
    return true;
}

static bool reader_decode_image(const uint8_t *data, size_t length, const char *name, bool comic_mode) {
    s_comic_split_active = false;
    if (!reader_ensure_image_buffer()) return false;
    const char *extension = reader_extension(name);
    if (reader_strcasecmp(extension, "jpg") == 0 || reader_strcasecmp(extension, "jpeg") == 0) {
        return reader_decode_jpeg(data, length, comic_mode);
    }
    if (reader_strcasecmp(extension, "bmp") == 0) return reader_decode_bmp(data, length);
    if (reader_strcasecmp(extension, "pbm") == 0) return reader_decode_pbm(data, length);
    return false;
}

static bool reader_load_text_page(const char *path, int page_index) {
    if (!path || !s_text_offsets || page_index < 0 || page_index >= s_page_count) return false;
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, s_text_offsets[page_index], SEEK_SET) != 0) {
        if (file) fclose(file);
        return false;
    }

    size_t used = 0;
    int lines = 0;
    int columns = 0;
    int value;
    s_text_page[0] = '\0';
    while (lines < READER_TEXT_LINES && (value = fgetc(file)) != EOF) {
        if (value == '\r') continue;
        if (value == '\n') {
            if (used + 1 < sizeof(s_text_page)) s_text_page[used++] = '\n';
            ++lines;
            columns = 0;
            continue;
        }
        if (columns >= READER_TEXT_COLUMNS) {
            if (used + 1 < sizeof(s_text_page)) s_text_page[used++] = '\n';
            ++lines;
            columns = 0;
            if (lines >= READER_TEXT_LINES) break;
        }
        if (used + 1 < sizeof(s_text_page)) s_text_page[used++] = (char)value;
        ++columns;
    }
    fclose(file);
    s_text_page[used] = '\0';
    return true;
}

static bool reader_load_epub_text_page(int page_index) {
    if (!s_epub_text || !s_text_offsets || page_index < 0 || page_index >= s_page_count) return false;
    size_t cursor = (size_t)s_text_offsets[page_index];
    if (cursor > s_epub_text_length) return false;
    size_t used = 0; int lines = 0, columns = 0;
    s_text_page[0] = 0;
    while (lines < READER_TEXT_LINES && cursor < s_epub_text_length) {
        size_t char_len = reader_utf8_length(s_epub_text + cursor, s_epub_text_length - cursor);
        char value = s_epub_text[cursor];
        cursor += char_len;
        if (value == '\r') continue;
        if (value == '\n') {
            if (used + 1 < sizeof(s_text_page)) s_text_page[used++] = '\n';
            ++lines; columns = 0; continue;
        }
        if (columns >= READER_TEXT_COLUMNS) {
            if (used + 1 < sizeof(s_text_page)) s_text_page[used++] = '\n';
            ++lines; columns = 0;
            if (lines >= READER_TEXT_LINES) break;
        }
        if (used + char_len < sizeof(s_text_page)) {
            memcpy(s_text_page + used, s_epub_text + cursor - char_len, char_len);
            used += char_len;
        }
        ++columns;
    }
    s_text_page[used] = 0;
    return true;
}

static void reader_set_message(const char *message) {
    if (!s_page_area) return;
    lv_obj_clean(s_page_area);
    s_page_image = NULL;
    s_page_text = NULL;
    s_page_message = lv_label_create(s_page_area);
    if (!s_page_message) return;
    lv_label_set_text(s_page_message, message ? message : "");
    lv_label_set_long_mode(s_page_message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_page_message, LV_PCT(90));
    lv_obj_set_style_text_font(s_page_message, accessibility_get_font_body(), 0);
    lv_obj_set_style_text_color(s_page_message, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_align(s_page_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_page_message);
}

static const char *reader_current_page_name(void) {
    if (s_page_count <= 0 || s_page_index < 0 || s_page_index >= s_page_count) return "";
    if (s_pages) {
        return s_pages[s_page_index].from_zip ? s_pages[s_page_index].member : s_pages[s_page_index].path;
    }
    return "text";
}

static void reader_update_footer(void) {
    if (!s_page_counter) return;
    if (s_comic_split_active) {
        lv_label_set_text_fmt(s_page_counter, "%s %u/%u  %d/%d",
                              s_comic_detail_active ? "ZOOM" : "PART",
                              (unsigned)(s_comic_section + 1),
                              (unsigned)s_comic_section_count,
                              s_page_count ? s_page_index + 1 : 0, s_page_count);
    } else {
        lv_label_set_text_fmt(s_page_counter, "%d / %d", s_page_count ? s_page_index + 1 : 0, s_page_count);
    }
}

static void reader_render_page(void) {
    if (!s_page_area || s_rendering) return;
    s_rendering = true;
    if (s_page_count <= 0) {
        reader_set_message("This book has no readable pages");
        reader_update_footer();
        s_rendering = false;
        return;
    }

    const reader_book_t *book = (s_book_index >= 0 && s_book_index < s_book_count) ? &s_books[s_book_index] : NULL;
    if (!book) {
        reader_set_message("Book is no longer available");
        s_rendering = false;
        return;
    }

    if (book->kind == READER_BOOK_TEXT || book->kind == READER_BOOK_EPUB) {
        s_comic_split_active = false;
        lv_obj_clean(s_page_area);
        s_page_image = NULL;
        s_page_message = NULL;
        s_page_text = NULL;
        bool loaded_text = book->kind == READER_BOOK_EPUB ?
                           reader_load_epub_text_page(s_page_index) :
                           reader_load_text_page(book->path, s_page_index);
        if (!loaded_text) {
            reader_set_message("Cannot read this book");
        } else {
            s_page_text = lv_label_create(s_page_area);
            lv_label_set_text(s_page_text, s_text_page);
            lv_label_set_long_mode(s_page_text, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(s_page_text, LV_PCT(92));
            lv_obj_set_height(s_page_text, LV_PCT(94));
            lv_obj_align(s_page_text, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_style_text_font(s_page_text, accessibility_get_font_body(), 0);
            lv_obj_set_style_text_color(s_page_text, lv_color_hex(0x000000), 0);
            lv_obj_set_style_text_align(s_page_text, LV_TEXT_ALIGN_LEFT, 0);
        }
    } else {
        uint8_t *data = NULL;
        size_t length = 0;
        bool mounted_here = false;
        bool display_was_suspended = false;
        bool data_is_cached = false;
        if (s_page_data_cache && s_page_data_cache_index != s_page_index) {
            reader_free_page_cache();
        }

        /* Promote a background-loaded CBZ page without touching the card or
         * extracting the archive again. */
        if (!s_page_data_cache && s_prefetch_lock &&
            xSemaphoreTake(s_prefetch_lock, portMAX_DELAY) == pdTRUE) {
            if (s_prefetch_data_cache && s_prefetch_data_cache_index == s_page_index) {
                s_page_data_cache = s_prefetch_data_cache;
                s_page_data_cache_length = s_prefetch_data_cache_length;
                s_page_data_cache_index = s_prefetch_data_cache_index;
                s_prefetch_data_cache = NULL;
                s_prefetch_data_cache_length = 0;
                s_prefetch_data_cache_index = -1;
            }
            xSemaphoreGive(s_prefetch_lock);
        }

        bool loaded = false;
        if (s_page_data_cache && s_page_data_cache_index == s_page_index) {
            data = s_page_data_cache;
            length = s_page_data_cache_length;
            loaded = true;
            data_is_cached = true;
            ESP_LOGI(TAG, "Using cached page: %u bytes", (unsigned)length);
        } else {
            bool io_locked = s_reader_io_lock &&
                             xSemaphoreTake(s_reader_io_lock, portMAX_DELAY) == pdTRUE;
            loaded = io_locked && reader_sd_begin(&mounted_here, &display_was_suspended);
            if (loaded) loaded = reader_load_page_data(&s_pages[s_page_index], &data, &length);
            if (loaded && length <= READER_PAGE_CACHE_MAX_BYTES) {
                s_page_data_cache = data;
                s_page_data_cache_length = length;
                s_page_data_cache_index = s_page_index;
                data_is_cached = true;
                ESP_LOGI(TAG, "Cached page for fast half-page turn: %u bytes", (unsigned)length);
            }
            if (io_locked) {
                reader_sd_end(mounted_here, display_was_suspended);
                xSemaphoreGive(s_reader_io_lock);
            }
        }
        if (loaded) {
            const char *name = reader_current_page_name();
            bool comic_mode = book->kind == READER_BOOK_CBZ || book->kind == READER_BOOK_FOLDER;
            loaded = reader_decode_image(data, length, name, comic_mode);
            if (!loaded) {
                ESP_LOGW(TAG, "Page decode failed: %s (%u bytes)",
                         name, (unsigned)length);
            }
        }
        if (!data_is_cached) reader_free(data);

        if (loaded && book->kind == READER_BOOK_CBZ && s_page_index + 1 < s_page_count) {
            reader_schedule_prefetch(s_page_index + 1);
        }

        lv_obj_clean(s_page_area);
        s_page_image = NULL;
        s_page_text = NULL;
        s_page_message = NULL;
        if (!loaded) {
            reader_set_message("Cannot open page\nUse JPG, BMP, or PBM images");
        } else {
            s_page_image = lv_img_create(s_page_area);
            if (!s_page_image) {
                reader_set_message("Not enough memory for page");
            } else {
                lv_img_set_src(s_page_image, &s_image_dsc);
                lv_obj_center(s_page_image);
            }
        }
    }
    reader_update_footer();
    s_rendering = false;
}

static void reader_change_page(int delta) {
    if (s_mode != READER_MODE_READING || s_page_count <= 0 || s_rendering) return;
    int64_t now = esp_timer_get_time();
    if (s_last_page_action_us && now - s_last_page_action_us < 220000) return;

    /* Comic sections and detail tiles are one linear reading sequence. In
     * detail mode the tile order is row-major (left-to-right, then down),
     * which matches how a reader naturally scans a page. */
    if (s_comic_split_active) {
        if (delta > 0 && s_comic_section + 1 < s_comic_section_count) {
            ++s_comic_section;
            s_last_page_action_us = now;
            reader_render_page();
            return;
        }
        if (delta < 0 && s_comic_section > 0) {
            --s_comic_section;
            s_last_page_action_us = now;
            reader_render_page();
            return;
        }
    }

    int next = s_page_index + delta;
    if (next < 0) next = 0;
    if (next >= s_page_count) next = s_page_count - 1;
    if (next == s_page_index) return;
    s_page_index = next;
    s_comic_section = 0;
    s_last_page_action_us = now;
    reader_render_page();
}

static bool reader_current_book_is_comic(void) {
    return s_book_index >= 0 && s_book_index < s_book_count &&
           (s_books[s_book_index].kind == READER_BOOK_CBZ ||
            s_books[s_book_index].kind == READER_BOOK_FOLDER);
}

static void reader_toggle_comic_detail(void) {
    if (s_mode != READER_MODE_READING || !reader_current_book_is_comic() || s_rendering) return;
    int64_t now = esp_timer_get_time();
    if (s_last_page_action_us && now - s_last_page_action_us < 220000) return;
    s_comic_detail_active = !s_comic_detail_active;
    s_comic_section = 0;
    s_last_page_action_us = now;
    reader_render_page();
}

static void reader_library_item_cb(lv_event_t *event) {
    if (!event || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index >= 0 && index < s_book_count) {
        s_book_index = index;
        reader_show_reading();
    }
}

static void reader_library_back_cb(lv_event_t *event) {
    (void)event;
    display_manager_go_back();
}

static void reader_delete_reading_ui(void) {
    if (s_page_area && lv_obj_is_valid(s_page_area)) lv_obj_del(s_page_area);
    if (s_page_footer && lv_obj_is_valid(s_page_footer)) lv_obj_del(s_page_footer);
    s_page_area = NULL;
    s_page_footer = NULL;
    s_page_image = NULL;
    s_page_text = NULL;
    s_page_message = NULL;
    s_page_counter = NULL;
    reader_free_pages();
}

static void reader_show_library(void) {
    s_mode = READER_MODE_LIBRARY;
    reader_delete_reading_ui();
    if (s_options) {
        options_view_destroy(s_options);
        s_options = NULL;
    }

    bool sd_ready = reader_scan_library();
    s_options = options_view_create_no_bg(s_root, "Reader");
    if (!s_options) return;

    if (!sd_ready) {
        options_view_add_item(s_options, "SD card unavailable", NULL, NULL);
        options_view_add_item(s_options, "Insert SD and reopen Reader", NULL, NULL);
    } else if (s_book_count == 0) {
        options_view_add_item(s_options, "No books found", NULL, NULL);
        options_view_add_item(s_options, "Copy content to /mnt/ghostesp/comics", NULL, NULL);
        options_view_add_item(s_options, "CBZ, JPG folders, or TXT books", NULL, NULL);
    } else {
        for (int i = 0; i < s_book_count; ++i) {
            char label[READER_MAX_BOOK_NAME + 20];
            const char *kind = s_books[i].kind == READER_BOOK_CBZ ? "CBZ" :
                               s_books[i].kind == READER_BOOK_FOLDER ? "BOOK" :
                               s_books[i].kind == READER_BOOK_TEXT ? "TEXT" :
                               s_books[i].kind == READER_BOOK_EPUB ? "EPUB" : "IMAGE";
            snprintf(label, sizeof(label), "%s  [%s]", s_books[i].name, kind);
            options_view_add_item(s_options, label, reader_library_item_cb, (void *)(intptr_t)i);
        }
    }
    options_view_add_back_row(s_options, reader_library_back_cb, NULL);
    options_view_set_selected(s_options, s_book_count > 0 ? s_book_index : 0);
}

static void reader_create_reading_ui(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t footer_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t text_color = lv_color_hex(theme_palette_get_text(theme));

    s_page_area = lv_obj_create(s_root);
    if (!s_page_area) return;
    lv_obj_remove_style_all(s_page_area);
    lv_obj_set_size(s_page_area, LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_H - READER_FOOTER_H);
    lv_obj_set_pos(s_page_area, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_page_area, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_page_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page_area, 0, 0);
    lv_obj_set_style_pad_all(s_page_area, 0, 0);
    lv_obj_clear_flag(s_page_area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_page_footer = lv_obj_create(s_root);
    if (!s_page_footer) return;
    lv_obj_remove_style_all(s_page_footer);
    lv_obj_set_size(s_page_footer, LV_HOR_RES, READER_FOOTER_H);
    lv_obj_set_pos(s_page_footer, 0, LV_VER_RES - READER_FOOTER_H);
    lv_obj_set_style_bg_color(s_page_footer, footer_color, 0);
    lv_obj_set_style_bg_opa(s_page_footer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_page_footer, 0, 0);

    lv_obj_t *previous = lv_label_create(s_page_footer);
    lv_label_set_text(previous, "< PREV");
    lv_obj_set_style_text_font(previous, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(previous, text_color, 0);
    lv_obj_align(previous, LV_ALIGN_LEFT_MID, 6, 0);

    s_page_counter = lv_label_create(s_page_footer);
    lv_obj_set_style_text_font(s_page_counter, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_page_counter, text_color, 0);
    lv_obj_align(s_page_counter, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *next = lv_label_create(s_page_footer);
    lv_label_set_text(next, "NEXT >");
    lv_obj_set_style_text_font(next, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(next, text_color, 0);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -6, 0);

}

static void reader_show_reading(void) {
    if (s_book_count <= 0) return;
    if (s_options) {
        options_view_destroy(s_options);
        s_options = NULL;
    }
    s_mode = READER_MODE_READING;
    s_page_index = 0;
    s_comic_section = 0;
    s_comic_section_count = 1;
    s_comic_tile_columns = 1;
    s_comic_split_active = false;
    s_comic_detail_active = false;
    s_last_page_action_us = 0;
    if (s_book_index < 0 || s_book_index >= s_book_count || !reader_build_pages(&s_books[s_book_index])) {
        reader_free_pages();
    }
    display_manager_add_status_bar("Reader");
    reader_create_reading_ui();
    reader_render_page();
}

static void reader_activate_selected(void) {
    int selected = s_options ? options_view_get_selected(s_options) : s_book_index;
    if (selected < 0 || selected >= s_book_count) {
        display_manager_go_back();
        return;
    }
    s_book_index = selected;
    reader_show_reading();
}

static void reader_input_callback(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (s_mode == READER_MODE_READING) reader_show_library();
        else display_manager_go_back();
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        if (s_mode == READER_MODE_READING) {
            reader_change_page(event->data.touch_data.point.x < LV_HOR_RES / 2 ? -1 : 1);
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            if (s_mode == READER_MODE_READING && reader_current_book_is_comic()) reader_toggle_comic_detail();
            else if (s_mode == READER_MODE_READING) reader_change_page(1);
            else reader_activate_selected();
        } else if (event->data.encoder.direction) {
            if (s_mode == READER_MODE_READING) reader_change_page(event->data.encoder.direction > 0 ? 1 : -1);
            else if (s_options) options_view_move_selection(s_options, event->data.encoder.direction > 0 ? 1 : -1);
        }
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == 29 || key == '`') {
            if (s_mode == READER_MODE_READING) reader_show_library();
            else display_manager_go_back();
        } else if (s_mode == READER_MODE_READING) {
            if (key == LV_KEY_LEFT || key == 'h') reader_change_page(-1);
            else if (key == LV_KEY_RIGHT || key == 'l' || key == LV_KEY_ENTER || key == 13) reader_change_page(1);
            else if (key == 'z' || key == 'Z') reader_toggle_comic_detail();
        } else if (s_options) {
            if (key == LV_KEY_UP || key == 'k') options_view_move_selection(s_options, -1);
            else if (key == LV_KEY_DOWN || key == 'j') options_view_move_selection(s_options, 1);
            else if (key == LV_KEY_ENTER || key == 13) reader_activate_selected();
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        int index = event->data.joystick_index;
        if (s_mode == READER_MODE_READING) {
            /* CrowPanel e-paper maps PREV/NEXT to logical 2/4.  0/3 are
             * accepted too so the view remains usable with a remapped profile. */
            if (index == 0 || index == 2) reader_change_page(-1);
            else if (index == 3 || index == 4) reader_change_page(1);
            else if (index == 1) {
                if (reader_current_book_is_comic()) reader_toggle_comic_detail();
                else reader_change_page(1);
            }
        } else if (s_options) {
            if (index == 0 || index == 2) options_view_move_selection(s_options, -1);
            else if (index == 3 || index == 4) options_view_move_selection(s_options, 1);
            else if (index == 1) reader_activate_selected();
        }
    }
}

void book_reader_create(void) {
    if (!s_reader_io_lock) s_reader_io_lock = xSemaphoreCreateMutex();
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t background = lv_color_hex(theme_palette_get_background(theme));
    display_manager_fill_screen(background);
    s_root = gui_screen_create_root_no_bg(NULL, "Reader", background, LV_OPA_COVER);
    book_reader_view.root = s_root;
    s_mode = READER_MODE_LIBRARY;
    s_book_index = 0;
    reader_show_library();
}

void book_reader_destroy(void) {
    if (s_options) {
        options_view_destroy(s_options);
        s_options = NULL;
    }
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = NULL;
    s_page_area = NULL;
    s_page_footer = NULL;
    reader_free_pages();
    reader_free_library();
    if (s_reader_io_lock) {
        vSemaphoreDelete(s_reader_io_lock);
        s_reader_io_lock = NULL;
    }
    book_reader_view.root = NULL;
}

void book_reader_get_callback(void **callback) {
    if (callback) *callback = (void *)reader_input_callback;
}

View book_reader_view = {
    .root = NULL,
    .create = book_reader_create,
    .destroy = book_reader_destroy,
    .name = "Reader",
    .get_hardwareinput_callback = book_reader_get_callback,
    .input_callback = reader_input_callback,
};

#endif /* CONFIG_CROWPANEL_EPAPER_42 */
