#ifndef GARBAGE_COLLECTOR_H
#define GARBAGE_COLLECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// GC Configuration
#define GC_MAX_OBJECTS 1024
#define GC_MARK_STACK_SIZE 256
#define GC_POOL_BLOCK_SIZE 64
#define GC_POOL_MAX_BLOCKS 512
#define GC_DEFRAG_THRESHOLD 0.3f  // Trigger defrag when fragmentation > 30%

// Memory capabilities for different allocation types
typedef enum {
    GC_CAP_DEFAULT = MALLOC_CAP_8BIT,
    GC_CAP_DMA = MALLOC_CAP_8BIT | MALLOC_CAP_DMA,
    GC_CAP_INTERNAL = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL,
    GC_CAP_SPIRAM = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
    GC_CAP_DMA_INTERNAL = MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
} gc_capability_t;

// GC Object types for different cleanup strategies
typedef enum {
    GC_OBJ_GENERIC,
    GC_OBJ_STRING,
    GC_OBJ_BUFFER,
    GC_OBJ_TASK_STACK,
    GC_OBJ_DMA_BUFFER,
    GC_OBJ_NETWORK_BUFFER,
    GC_OBJ_DISPLAY_BUFFER,
    GC_OBJ_NFC_BUFFER
} gc_object_type_t;

// GC Object structure
typedef struct gc_object {
    void* ptr;
    size_t size;
    uint32_t ref_count;
    gc_object_type_t type;
    gc_capability_t capability;
    bool marked;
    struct gc_object* next;
    struct gc_object* prev;
    uint32_t alloc_time;
    uint32_t last_access_time;
} gc_object_t;

// Memory pool for fast allocation
typedef struct gc_memory_pool {
    void* base_ptr;
    size_t block_size;
    size_t total_blocks;
    size_t free_blocks;
    uint8_t* free_bitmap;
    gc_capability_t capability;
    SemaphoreHandle_t mutex;
} gc_memory_pool_t;

// GC Statistics
typedef struct gc_stats {
    size_t total_allocated;
    size_t total_freed;
    size_t current_objects;
    size_t peak_objects;
    size_t fragmentation_bytes;
    float fragmentation_ratio;
    uint32_t gc_cycles;
    uint32_t defrag_cycles;
    uint32_t last_gc_time;
    uint32_t last_defrag_time;
} gc_stats_t;

// Main GC Manager
typedef struct gc_manager {
    gc_object_t* object_list;
    gc_memory_pool_t pools[5]; // Different pools for different capabilities
    gc_stats_t stats;
    SemaphoreHandle_t mutex;
    TaskHandle_t gc_task_handle;
    bool auto_gc_enabled;
    bool auto_defrag_enabled;
    uint32_t gc_interval_ms;
    uint32_t defrag_interval_ms;
    size_t low_memory_threshold;
    bool is_initialized;
} gc_manager_t;

// Global GC manager instance
extern gc_manager_t* g_gc_manager;

// Core GC Functions
esp_err_t gc_init(void);
void gc_deinit(void);
void* gc_malloc(size_t size, gc_capability_t capability, gc_object_type_t type);
void* gc_calloc(size_t num, size_t size, gc_capability_t capability, gc_object_type_t type);
void* gc_realloc(void* ptr, size_t new_size, gc_capability_t capability, gc_object_type_t type);
void gc_free(void* ptr);
void gc_add_ref(void* ptr);
void gc_remove_ref(void* ptr);

// Memory Pool Functions
esp_err_t gc_pool_init(gc_memory_pool_t* pool, size_t block_size, size_t num_blocks, gc_capability_t capability);
void gc_pool_deinit(gc_memory_pool_t* pool);
void* gc_pool_alloc(gc_memory_pool_t* pool);
void gc_pool_free(gc_memory_pool_t* pool, void* ptr);

// Garbage Collection Functions
void gc_collect(void);
void gc_collect_aggressive(void);
void gc_mark_and_sweep(void);
void gc_defragment(void);
void gc_cleanup_unused(void);

// Memory Management Functions
size_t gc_get_free_memory(gc_capability_t capability);
size_t gc_get_total_memory(gc_capability_t capability);
float gc_get_fragmentation_ratio(gc_capability_t capability);
bool gc_is_memory_low(gc_capability_t capability);

// Statistics and Monitoring
const gc_stats_t* gc_get_stats(void);
void gc_print_stats(void);
void gc_dump_objects(void);
void gc_dump_pools(void);

// Configuration Functions
void gc_set_auto_gc(bool enabled, uint32_t interval_ms);
void gc_set_auto_defrag(bool enabled, uint32_t interval_ms);
void gc_set_low_memory_threshold(size_t threshold);

// Utility Functions
bool gc_is_valid_ptr(void* ptr);
gc_object_t* gc_find_object(void* ptr);
void gc_force_cleanup_type(gc_object_type_t type);
void gc_optimize_for_capability(gc_capability_t capability);

// Missing function declarations
void gc_dump_pools(void);

// Integration with existing systems
void gc_register_ble_cleanup(void);
void gc_register_wifi_cleanup(void);
void gc_register_display_cleanup(void);
void gc_register_nfc_cleanup(void);

// Emergency cleanup functions
void gc_emergency_cleanup(void);
void gc_force_defrag(void);

// Task functions
void gc_task(void* pvParameters);
void gc_defrag_task(void* pvParameters);

// Macros for easy usage
#define GC_MALLOC(size, cap, type) gc_malloc(size, cap, type)
#define GC_CALLOC(num, size, cap, type) gc_calloc(num, size, cap, type)
#define GC_REALLOC(ptr, size, cap, type) gc_realloc(ptr, size, cap, type)
#define GC_FREE(ptr) gc_free(ptr)
#define GC_ADD_REF(ptr) gc_add_ref(ptr)
#define GC_REMOVE_REF(ptr) gc_remove_ref(ptr)

// Convenience macros for common allocations
#define GC_MALLOC_DMA(size) gc_malloc(size, GC_CAP_DMA_INTERNAL, GC_OBJ_DMA_BUFFER)
#define GC_MALLOC_STRING(size) gc_malloc(size, GC_CAP_DEFAULT, GC_OBJ_STRING)
#define GC_MALLOC_BUFFER(size) gc_malloc(size, GC_CAP_DEFAULT, GC_OBJ_BUFFER)
#define GC_MALLOC_NETWORK(size) gc_malloc(size, GC_CAP_DMA_INTERNAL, GC_OBJ_NETWORK_BUFFER)

#ifdef __cplusplus
}
#endif

#endif // GARBAGE_COLLECTOR_H
