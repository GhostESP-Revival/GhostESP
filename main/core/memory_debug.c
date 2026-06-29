#include "core/memory_debug.h"

#include <stdint.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

#define MEMORY_DEBUG_TRACE_RECORDS 256
#define MEMORY_DEBUG_TRACE_TOP_COUNT 12

static const char *TAG = "memory_debug";

static void print_heap_caps(const char *name, uint32_t caps) {
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, caps);

    size_t total = info.total_allocated_bytes + info.total_free_bytes;
    size_t free = info.total_free_bytes;
    size_t used = info.total_allocated_bytes;
    size_t largest = info.largest_free_block;
    size_t min_free = info.minimum_free_bytes;
    size_t fragmented = free > largest ? free - largest : 0;
    float free_pct = total > 0 ? (100.0f * (float)free / (float)total) : 0.0f;
    float frag_pct = free > 0 ? (100.0f * (float)fragmented / (float)free) : 0.0f;

    if (total == 0) {
        return;
    }

    ESP_LOGI(TAG,
             "%-14s total=%u used=%u free=%u largest=%u min_free=%u free_pct=%.1f%% frag=%u/%.1f%% alloc_blks=%u free_blks=%u total_blks=%u",
             name,
             (unsigned)total,
             (unsigned)used,
             (unsigned)free,
             (unsigned)largest,
             (unsigned)min_free,
             free_pct,
             (unsigned)fragmented,
             frag_pct,
             (unsigned)info.allocated_blocks,
             (unsigned)info.free_blocks,
             (unsigned)info.total_blocks);
}

void memory_debug_print_heap_summary(void) {
    ESP_LOGI(TAG, "heap columns: total used free largest min_free frag alloc_blks free_blks total_blks");
    print_heap_caps("internal-8bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap_caps("internal-dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    print_heap_caps("internal-32", MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    print_heap_caps("psram-8bit", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    print_heap_caps("all-8bit", MALLOC_CAP_8BIT);
    print_heap_caps("all-dma", MALLOC_CAP_DMA);
    print_heap_caps("all-32bit", MALLOC_CAP_32BIT);
}

void memory_debug_print_heap_regions(void) {
    ESP_LOGI(TAG, "heap regions: internal-8bit");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap regions: internal-dma");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) > 0) {
        ESP_LOGI(TAG, "heap regions: psram-8bit");
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_LOGI(TAG, "heap regions: all-8bit");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);
}

bool memory_debug_check_heap_integrity(void) {
    return heap_caps_check_integrity_all(true);
}

#if defined(CONFIG_HEAP_TRACING) || defined(CONFIG_HEAP_TRACING_STANDALONE)
#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
EXT_RAM_BSS_ATTR static heap_trace_record_t s_heap_trace_records[MEMORY_DEBUG_TRACE_RECORDS];
static const char *s_heap_trace_location = "PSRAM";
#else
static heap_trace_record_t s_heap_trace_records[MEMORY_DEBUG_TRACE_RECORDS];
static const char *s_heap_trace_location = "internal RAM";
#endif
static bool s_heap_trace_initialized;
static bool s_heap_trace_running;

typedef struct {
    void *address;
    void *caller;
    size_t size;
} memory_debug_trace_top_t;

static void trace_top_insert(memory_debug_trace_top_t *top, const heap_trace_record_t *rec) {
    for (int i = 0; i < MEMORY_DEBUG_TRACE_TOP_COUNT; i++) {
        if (rec->size <= top[i].size) {
            continue;
        }

        for (int j = MEMORY_DEBUG_TRACE_TOP_COUNT - 1; j > i; j--) {
            top[j] = top[j - 1];
        }
        top[i].address = rec->address;
#if CONFIG_HEAP_TRACING_STACK_DEPTH > 0
        top[i].caller = rec->alloced_by[0];
#else
        top[i].caller = NULL;
#endif
        top[i].size = rec->size;
        return;
    }
}

static esp_err_t memory_debug_trace_init_once(void) {
    if (s_heap_trace_initialized) {
        return ESP_OK;
    }

    esp_err_t err = heap_trace_init_standalone(s_heap_trace_records, MEMORY_DEBUG_TRACE_RECORDS);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_heap_trace_initialized = true;
        return ESP_OK;
    }

    return err;
}

esp_err_t memory_debug_trace_start(bool leaks_only) {
    esp_err_t err = memory_debug_trace_init_once();
    if (err != ESP_OK) {
        return err;
    }

    if (s_heap_trace_running) {
        heap_trace_stop();
        s_heap_trace_running = false;
    }

    err = heap_trace_start(leaks_only ? HEAP_TRACE_LEAKS : HEAP_TRACE_ALL);
    if (err == ESP_OK) {
        s_heap_trace_running = true;
    }
    return err;
}

esp_err_t memory_debug_trace_stop(void) {
    if (!s_heap_trace_running) {
        return ESP_OK;
    }

    heap_trace_stop();
    s_heap_trace_running = false;
    return ESP_OK;
}

void memory_debug_trace_dump(void) {
    esp_err_t err = memory_debug_trace_init_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heap trace dump failed: %s", esp_err_to_name(err));
        return;
    }

    if (s_heap_trace_running) {
        heap_trace_stop();
        s_heap_trace_running = false;
    }

    size_t count = heap_trace_get_count();
    size_t internal_count = 0;
    size_t internal_bytes = 0;
    size_t psram_count = 0;
    size_t psram_bytes = 0;
    size_t other_count = 0;
    size_t other_bytes = 0;
    memory_debug_trace_top_t top_internal[MEMORY_DEBUG_TRACE_TOP_COUNT] = {0};
    memory_debug_trace_top_t top_psram[MEMORY_DEBUG_TRACE_TOP_COUNT] = {0};

    for (size_t i = 0; i < count; i++) {
        heap_trace_record_t rec = {0};
        if (heap_trace_get(i, &rec) != ESP_OK || !rec.address || rec.freed) {
            continue;
        }

        if (esp_ptr_internal(rec.address)) {
            internal_count++;
            internal_bytes += rec.size;
            trace_top_insert(top_internal, &rec);
        } else if (esp_ptr_external_ram(rec.address)) {
            psram_count++;
            psram_bytes += rec.size;
            trace_top_insert(top_psram, &rec);
        } else {
            other_count++;
            other_bytes += rec.size;
        }
    }

    ESP_LOGI(TAG, "heap trace live records=%u/%u internal=%u/%u bytes psram=%u/%u bytes other=%u/%u bytes",
             (unsigned)count,
             (unsigned)MEMORY_DEBUG_TRACE_RECORDS,
             (unsigned)internal_count,
             (unsigned)internal_bytes,
             (unsigned)psram_count,
             (unsigned)psram_bytes,
             (unsigned)other_count,
             (unsigned)other_bytes);

    ESP_LOGI(TAG, "largest live internal allocations:");
    for (int i = 0; i < MEMORY_DEBUG_TRACE_TOP_COUNT && top_internal[i].size > 0; i++) {
        ESP_LOGI(TAG, "  %u bytes at %p caller=%p", (unsigned)top_internal[i].size,
                 top_internal[i].address, top_internal[i].caller);
    }

    ESP_LOGI(TAG, "largest live PSRAM allocations:");
    for (int i = 0; i < MEMORY_DEBUG_TRACE_TOP_COUNT && top_psram[i].size > 0; i++) {
        ESP_LOGI(TAG, "  %u bytes at %p caller=%p", (unsigned)top_psram[i].size,
                 top_psram[i].address, top_psram[i].caller);
    }
}

void memory_debug_start_boot_trace(void) {
    esp_err_t err = memory_debug_trace_start(true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "boot heap leak trace started (%u records, %s)",
                 (unsigned)MEMORY_DEBUG_TRACE_RECORDS,
                 s_heap_trace_location);
    } else {
        ESP_LOGW(TAG, "boot heap leak trace failed: %s", esp_err_to_name(err));
    }
}
#else
esp_err_t memory_debug_trace_start(bool leaks_only) {
    (void)leaks_only;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t memory_debug_trace_stop(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

void memory_debug_trace_dump(void) {
    ESP_LOGW(TAG, "heap tracing not enabled");
}

void memory_debug_start_boot_trace(void) {
    ESP_LOGI(TAG, "boot heap leak trace disabled");
}
#endif
