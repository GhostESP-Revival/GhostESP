#ifndef MEMORY_DEBUG_H
#define MEMORY_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

void memory_debug_print_heap_summary(void);
void memory_debug_print_heap_regions(void);
bool memory_debug_check_heap_integrity(void);
void memory_debug_start_boot_trace(void);
esp_err_t memory_debug_trace_start(bool leaks_only);
esp_err_t memory_debug_trace_stop(void);
void memory_debug_trace_dump(void);

static inline void *spiram_malloc(size_t size) {
    void *ptr = NULL;
#if CONFIG_SPIRAM
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr;
}

static inline void *spiram_calloc(size_t count, size_t size) {
    void *ptr = NULL;
#if CONFIG_SPIRAM
    ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!ptr) ptr = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr;
}

#endif // MEMORY_DEBUG_H
