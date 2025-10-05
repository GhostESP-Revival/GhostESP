#include "managers/garbage_collector.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"
#include "stdlib.h"
#include "core/glog.h"

static const char* TAG = "GC";

// Global GC manager instance
gc_manager_t* g_gc_manager = NULL;

// Internal function declarations
static void gc_mark_object(gc_object_t* obj);
static void gc_sweep_objects(void);
static void gc_cleanup_object(gc_object_t* obj);
static bool gc_should_defrag(void);

// Initialize the garbage collector
esp_err_t gc_init(void) {
    if (g_gc_manager != NULL) {
        ESP_LOGW(TAG, "GC already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Garbage Collector...");
    ESP_LOGI(TAG, "Free memory before GC init: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Allocate GC manager
    g_gc_manager = heap_caps_calloc(1, sizeof(gc_manager_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_gc_manager == NULL) {
        ESP_LOGE(TAG, "Failed to allocate GC manager");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GC manager allocated successfully");

    // Initialize mutex
    g_gc_manager->mutex = xSemaphoreCreateMutex();
    if (g_gc_manager->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create GC mutex");
        heap_caps_free(g_gc_manager);
        g_gc_manager = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Initialize object list
    g_gc_manager->object_list = NULL;

    // Initialize memory pools (much smaller for ESP32)
    esp_err_t ret = ESP_OK;
    
    // Default pool (1KB blocks, 8 blocks = 8KB total)
    ret |= gc_pool_init(&g_gc_manager->pools[0], 1024, 8, GC_CAP_DEFAULT);
    
    // DMA pool (512B blocks, 8 blocks = 4KB total)
    ret |= gc_pool_init(&g_gc_manager->pools[1], 512, 8, GC_CAP_DMA_INTERNAL);
    
    // String pool (128B blocks, 16 blocks = 2KB total)
    ret |= gc_pool_init(&g_gc_manager->pools[2], 128, 16, GC_CAP_DEFAULT);
    
    // Buffer pool (256B blocks, 16 blocks = 4KB total)
    ret |= gc_pool_init(&g_gc_manager->pools[3], 256, 16, GC_CAP_DEFAULT);
    
    // Network pool (512B blocks, 8 blocks = 4KB total)
    ret |= gc_pool_init(&g_gc_manager->pools[4], 512, 8, GC_CAP_DMA_INTERNAL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize memory pools");
        ESP_LOGE(TAG, "Free memory after pool init failure: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        gc_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "Memory pools initialized successfully");

    // Initialize statistics
    memset(&g_gc_manager->stats, 0, sizeof(gc_stats_t));

    // Configure auto-GC
    g_gc_manager->auto_gc_enabled = true;
    g_gc_manager->gc_interval_ms = 30000; // 30 seconds
    g_gc_manager->auto_defrag_enabled = true;
    g_gc_manager->defrag_interval_ms = 300000; // 5 minutes
    g_gc_manager->low_memory_threshold = 50 * 1024; // 50KB

    g_gc_manager->is_initialized = true;

    // Create GC task
    BaseType_t task_ret = xTaskCreate(gc_task, "gc_task", 4096, NULL, 5, &g_gc_manager->gc_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create GC task");
    }

    ESP_LOGI(TAG, "Garbage Collector initialized successfully");
    ESP_LOGI(TAG, "Free memory: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    return ESP_OK;
}

// Deinitialize the garbage collector
void gc_deinit(void) {
    if (g_gc_manager == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing Garbage Collector...");

    // Stop auto-GC
    g_gc_manager->auto_gc_enabled = false;
    g_gc_manager->auto_defrag_enabled = false;

    // Delete GC task
    if (g_gc_manager->gc_task_handle != NULL) {
        vTaskDelete(g_gc_manager->gc_task_handle);
        g_gc_manager->gc_task_handle = NULL;
    }

    // Take mutex to ensure no concurrent access
    if (xSemaphoreTake(g_gc_manager->mutex, portMAX_DELAY) == pdTRUE) {
        // Clean up all objects
        gc_object_t* obj = g_gc_manager->object_list;
        while (obj != NULL) {
            gc_object_t* next = obj->next;
            gc_cleanup_object(obj);
            obj = next;
        }

        // Deinitialize pools
        for (int i = 0; i < 5; i++) {
            gc_pool_deinit(&g_gc_manager->pools[i]);
        }

        xSemaphoreGive(g_gc_manager->mutex);
    }

    // Clean up mutex
    if (g_gc_manager->mutex != NULL) {
        vSemaphoreDelete(g_gc_manager->mutex);
    }

    // Free GC manager
    heap_caps_free(g_gc_manager);
    g_gc_manager = NULL;

    ESP_LOGI(TAG, "Garbage Collector deinitialized");
}

// Allocate memory with garbage collection
void* gc_malloc(size_t size, gc_capability_t capability, gc_object_type_t type) {
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return heap_caps_malloc(size, capability);
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take GC mutex for malloc");
        return heap_caps_malloc(size, capability);
    }

    void* ptr = heap_caps_malloc(size, capability);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "Allocation failed, attempting GC cleanup");
        gc_collect_aggressive();
        ptr = heap_caps_malloc(size, capability);
    }

    if (ptr != NULL) {
        // Create GC object
        gc_object_t* obj = heap_caps_calloc(1, sizeof(gc_object_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (obj != NULL) {
            obj->ptr = ptr;
            obj->size = size;
            obj->ref_count = 1;
            obj->type = type;
            obj->capability = capability;
            obj->marked = false;
            obj->alloc_time = esp_timer_get_time() / 1000; // Convert to ms
            obj->last_access_time = obj->alloc_time;

            // Add to object list
            obj->next = g_gc_manager->object_list;
            obj->prev = NULL;
            if (g_gc_manager->object_list != NULL) {
                g_gc_manager->object_list->prev = obj;
            }
            g_gc_manager->object_list = obj;

            // Update statistics
            g_gc_manager->stats.total_allocated += size;
            g_gc_manager->stats.current_objects++;
            if (g_gc_manager->stats.current_objects > g_gc_manager->stats.peak_objects) {
                g_gc_manager->stats.peak_objects = g_gc_manager->stats.current_objects;
            }
        } else {
            ESP_LOGW(TAG, "Failed to create GC object, memory not tracked");
        }
    }

    xSemaphoreGive(g_gc_manager->mutex);
    return ptr;
}

// Allocate and zero memory with garbage collection
void* gc_calloc(size_t num, size_t size, gc_capability_t capability, gc_object_type_t type) {
    size_t total_size = num * size;
    void* ptr = gc_malloc(total_size, capability, type);
    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

// Reallocate memory with garbage collection
void* gc_realloc(void* ptr, size_t new_size, gc_capability_t capability, gc_object_type_t type) {
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return heap_caps_realloc(ptr, new_size, capability);
    }

    if (ptr == NULL) {
        return gc_malloc(new_size, capability, type);
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take GC mutex for realloc");
        return heap_caps_realloc(ptr, new_size, capability);
    }

    gc_object_t* obj = gc_find_object(ptr);
    if (obj == NULL) {
        ESP_LOGW(TAG, "Realloc on untracked pointer");
        xSemaphoreGive(g_gc_manager->mutex);
        return heap_caps_realloc(ptr, new_size, capability);
    }

    void* new_ptr = heap_caps_realloc(ptr, new_size, capability);
    if (new_ptr != NULL) {
        // Update object
        size_t old_size = obj->size;
        obj->ptr = new_ptr;
        obj->size = new_size;
        obj->last_access_time = esp_timer_get_time() / 1000;

        // Update statistics
        g_gc_manager->stats.total_allocated += (new_size - old_size);
    }

    xSemaphoreGive(g_gc_manager->mutex);
    return new_ptr;
}

// Free memory with garbage collection
void gc_free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        heap_caps_free(ptr);
        return;
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take GC mutex for free");
        heap_caps_free(ptr);
        return;
    }

    gc_object_t* obj = gc_find_object(ptr);
    if (obj != NULL) {
        obj->ref_count--;
        if (obj->ref_count <= 0) {
            gc_cleanup_object(obj);
        } else {
            obj->last_access_time = esp_timer_get_time() / 1000;
        }
    } else {
        ESP_LOGW(TAG, "Free on untracked pointer");
        heap_caps_free(ptr);
    }

    xSemaphoreGive(g_gc_manager->mutex);
}

// Add reference to object
void gc_add_ref(void* ptr) {
    if (ptr == NULL || g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return;
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        gc_object_t* obj = gc_find_object(ptr);
        if (obj != NULL) {
            obj->ref_count++;
            obj->last_access_time = esp_timer_get_time() / 1000;
        }
        xSemaphoreGive(g_gc_manager->mutex);
    }
}

// Remove reference from object
void gc_remove_ref(void* ptr) {
    gc_free(ptr); // Same as free for reference counting
}

// Perform garbage collection
void gc_collect(void) {
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return;
    }

    ESP_LOGD(TAG, "Starting garbage collection...");

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take GC mutex for collection");
        return;
    }

    // Mark phase - mark all reachable objects
    gc_object_t* obj = g_gc_manager->object_list;
    while (obj != NULL) {
        obj->marked = false;
        obj = obj->next;
    }

    // Mark objects with references > 0 as reachable
    obj = g_gc_manager->object_list;
    while (obj != NULL) {
        if (obj->ref_count > 0) {
            gc_mark_object(obj);
        }
        obj = obj->next;
    }

    // Sweep phase - remove unmarked objects
    gc_sweep_objects();

    // Update statistics
    g_gc_manager->stats.gc_cycles++;
    g_gc_manager->stats.last_gc_time = esp_timer_get_time() / 1000;

    xSemaphoreGive(g_gc_manager->mutex);

    ESP_LOGD(TAG, "Garbage collection completed");
}

// Perform aggressive garbage collection
void gc_collect_aggressive(void) {
    ESP_LOGI(TAG, "Starting aggressive garbage collection...");
    
    // Force cleanup of unused objects
    gc_cleanup_unused();
    
    // Regular GC
    gc_collect();
    
    // Force defragmentation if needed
    if (gc_should_defrag()) {
        gc_defragment();
    }
    
    ESP_LOGI(TAG, "Aggressive garbage collection completed");
}

// Mark object as reachable
static void gc_mark_object(gc_object_t* obj) {
    if (obj == NULL || obj->marked) {
        return;
    }

    obj->marked = true;
    obj->last_access_time = esp_timer_get_time() / 1000;
}

// Sweep unmarked objects
static void gc_sweep_objects(void) {
    gc_object_t* obj = g_gc_manager->object_list;
    while (obj != NULL) {
        gc_object_t* next = obj->next;
        if (!obj->marked && obj->ref_count <= 0) {
            gc_cleanup_object(obj);
        }
        obj = next;
    }
}

// Clean up unused objects (objects not accessed recently)
void gc_cleanup_unused(void) {
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return;
    }

    uint32_t current_time = esp_timer_get_time() / 1000;
    uint32_t unused_threshold = 60000; // 60 seconds

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        gc_object_t* obj = g_gc_manager->object_list;
        while (obj != NULL) {
            gc_object_t* next = obj->next;
            if (obj->ref_count <= 0 && 
                (current_time - obj->last_access_time) > unused_threshold) {
                ESP_LOGD(TAG, "Cleaning up unused object: type=%d, size=%d", obj->type, (int)obj->size);
                gc_cleanup_object(obj);
            }
            obj = next;
        }
        xSemaphoreGive(g_gc_manager->mutex);
    }
}

// Clean up a specific object
static void gc_cleanup_object(gc_object_t* obj) {
    if (obj == NULL) {
        return;
    }

    // Remove from object list
    if (obj->prev != NULL) {
        obj->prev->next = obj->next;
    } else {
        g_gc_manager->object_list = obj->next;
    }
    if (obj->next != NULL) {
        obj->next->prev = obj->prev;
    }

    // Free the actual memory
    if (obj->ptr != NULL) {
        heap_caps_free(obj->ptr);
    }

    // Update statistics
    g_gc_manager->stats.total_freed += obj->size;
    g_gc_manager->stats.current_objects--;

    // Free the object structure
    heap_caps_free(obj);
}

// Find object by pointer
gc_object_t* gc_find_object(void* ptr) {
    if (g_gc_manager == NULL || ptr == NULL) {
        return NULL;
    }

    gc_object_t* obj = g_gc_manager->object_list;
    while (obj != NULL) {
        if (obj->ptr == ptr) {
            return obj;
        }
        obj = obj->next;
    }
    return NULL;
}

// Check if pointer is valid
bool gc_is_valid_ptr(void* ptr) {
    return gc_find_object(ptr) != NULL;
}

// Get free memory for specific capability
size_t gc_get_free_memory(gc_capability_t capability) {
    return heap_caps_get_free_size(capability);
}

// Get total memory for specific capability
size_t gc_get_total_memory(gc_capability_t capability) {
    return heap_caps_get_total_size(capability);
}

// Check if memory is low
bool gc_is_memory_low(gc_capability_t capability) {
    size_t free_mem = gc_get_free_memory(capability);
    return free_mem < g_gc_manager->low_memory_threshold;
}

// Get GC statistics
const gc_stats_t* gc_get_stats(void) {
    if (g_gc_manager == NULL) {
        return NULL;
    }
    return &g_gc_manager->stats;
}

// Print GC statistics
void gc_print_stats(void) {
    if (g_gc_manager == NULL) {
        glog("GC not initialized\n");
        return;
    }

    const gc_stats_t* stats = &g_gc_manager->stats;
    glog("=== GC Statistics ===\n");
    glog("Total allocated: %d bytes\n", (int)stats->total_allocated);
    glog("Total freed: %d bytes\n", (int)stats->total_freed);
    glog("Current objects: %d\n", (int)stats->current_objects);
    glog("Peak objects: %d\n", (int)stats->peak_objects);
    glog("GC cycles: %d\n", (int)stats->gc_cycles);
    glog("Defrag cycles: %d\n", (int)stats->defrag_cycles);
    glog("Fragmentation: %.2f%%\n", stats->fragmentation_ratio * 100.0f);
    
    // Print memory info
    glog("=== Memory Info ===\n");
    glog("Free (8bit): %d bytes\n", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    glog("Free (DMA): %d bytes\n", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    glog("Free (SPIRAM): %d bytes\n", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Dump all objects
void gc_dump_objects(void) {
    if (g_gc_manager == NULL) {
        glog("GC not initialized\n");
        return;
    }

    glog("=== GC Objects ===\n");
    gc_object_t* obj = g_gc_manager->object_list;
    int count = 0;
    while (obj != NULL) {
        glog("Object %d: ptr=%p, size=%d, refs=%d, type=%d\n", 
             count, obj->ptr, (int)obj->size, (int)obj->ref_count, obj->type);
        obj = obj->next;
        count++;
    }
    glog("Total objects: %d\n", count);
}

// Dump memory pools
void gc_dump_pools(void) {
    if (g_gc_manager == NULL) {
        glog("GC not initialized\n");
        return;
    }

    glog("=== Memory Pools ===\n");
    for (int i = 0; i < 5; i++) {
        gc_memory_pool_t* pool = &g_gc_manager->pools[i];
        glog("Pool %d: block_size=%d, total_blocks=%d, free_blocks=%d, capability=%d\n",
             i, (int)pool->block_size, (int)pool->total_blocks, (int)pool->free_blocks, pool->capability);
    }
}

// Set auto-GC configuration
void gc_set_auto_gc(bool enabled, uint32_t interval_ms) {
    if (g_gc_manager == NULL) {
        return;
    }
    g_gc_manager->auto_gc_enabled = enabled;
    g_gc_manager->gc_interval_ms = interval_ms;
}

// Set auto-defrag configuration
void gc_set_auto_defrag(bool enabled, uint32_t interval_ms) {
    if (g_gc_manager == NULL) {
        return;
    }
    g_gc_manager->auto_defrag_enabled = enabled;
    g_gc_manager->defrag_interval_ms = interval_ms;
}

// Set low memory threshold
void gc_set_low_memory_threshold(size_t threshold) {
    if (g_gc_manager == NULL) {
        return;
    }
    g_gc_manager->low_memory_threshold = threshold;
}

// Force cleanup of specific object type
void gc_force_cleanup_type(gc_object_type_t type) {
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return;
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        gc_object_t* obj = g_gc_manager->object_list;
        while (obj != NULL) {
            gc_object_t* next = obj->next;
            if (obj->type == type && obj->ref_count <= 0) {
                ESP_LOGI(TAG, "Force cleaning up object type %d", type);
                gc_cleanup_object(obj);
            }
            obj = next;
        }
        xSemaphoreGive(g_gc_manager->mutex);
    }
}

// Emergency cleanup
void gc_emergency_cleanup(void) {
    ESP_LOGW(TAG, "Emergency cleanup triggered!");
    
    // Force cleanup of all unused objects
    gc_cleanup_unused();
    
    // Force cleanup of specific types that are typically safe to clean
    gc_force_cleanup_type(GC_OBJ_STRING);
    gc_force_cleanup_type(GC_OBJ_BUFFER);
    
    // Aggressive GC
    gc_collect_aggressive();
    
    ESP_LOGW(TAG, "Emergency cleanup completed");
}

// GC task
void gc_task(void* pvParameters) {
    TickType_t last_gc_time = 0;
    TickType_t last_defrag_time = 0;
    
    while (g_gc_manager != NULL && g_gc_manager->auto_gc_enabled) {
        TickType_t current_time = xTaskGetTickCount();
        
        // Check if it's time for GC
        if (g_gc_manager->auto_gc_enabled && 
            (current_time - last_gc_time) >= pdMS_TO_TICKS(g_gc_manager->gc_interval_ms)) {
            gc_collect();
            last_gc_time = current_time;
        }
        
        // Check if it's time for defragmentation
        if (g_gc_manager->auto_defrag_enabled && 
            (current_time - last_defrag_time) >= pdMS_TO_TICKS(g_gc_manager->defrag_interval_ms)) {
            if (gc_should_defrag()) {
                gc_defragment();
                last_defrag_time = current_time;
            }
        }
        
        // Check for low memory
        if (gc_is_memory_low(GC_CAP_DEFAULT)) {
            ESP_LOGW(TAG, "Low memory detected, triggering emergency cleanup");
            gc_emergency_cleanup();
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000)); // Check every 5 seconds
    }
    
    vTaskDelete(NULL);
}

// Check if defragmentation is needed
static bool gc_should_defrag(void) {
    if (g_gc_manager == NULL) {
        return false;
    }
    
    float frag_ratio = gc_get_fragmentation_ratio(GC_CAP_DEFAULT);
    return frag_ratio > GC_DEFRAG_THRESHOLD;
}

// Get fragmentation ratio
float gc_get_fragmentation_ratio(gc_capability_t capability) {
    size_t free_mem = heap_caps_get_free_size(capability);
    size_t largest_block = heap_caps_get_largest_free_block(capability);
    
    if (free_mem == 0) {
        return 1.0f;
    }
    
    return 1.0f - ((float)largest_block / (float)free_mem);
}

// Defragment memory
void gc_defragment(void) {
    ESP_LOGI(TAG, "Starting memory defragmentation...");
    
    // This is a simplified defragmentation
    // In a real implementation, you would need to move objects around
    // and update all references to them
    
    if (g_gc_manager == NULL) {
        return;
    }
    
    g_gc_manager->stats.defrag_cycles++;
    g_gc_manager->stats.last_defrag_time = esp_timer_get_time() / 1000;
    
    // Force garbage collection to clean up fragmented memory
    gc_collect_aggressive();
    
    ESP_LOGI(TAG, "Memory defragmentation completed");
}

// Force defragmentation
void gc_force_defrag(void) {
    ESP_LOGI(TAG, "Force defragmentation triggered");
    gc_defragment();
}

// Memory pool implementation
esp_err_t gc_pool_init(gc_memory_pool_t* pool, size_t block_size, size_t num_blocks, gc_capability_t capability) {
    if (pool == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    pool->block_size = block_size;
    pool->total_blocks = num_blocks;
    pool->free_blocks = num_blocks;
    pool->capability = capability;
    
    // Allocate base memory
    pool->base_ptr = heap_caps_malloc(block_size * num_blocks, capability);
    if (pool->base_ptr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Allocate free bitmap
    size_t bitmap_size = (num_blocks + 7) / 8; // Round up to bytes
    pool->free_bitmap = heap_caps_calloc(bitmap_size, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pool->free_bitmap == NULL) {
        heap_caps_free(pool->base_ptr);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize all blocks as free
    memset(pool->free_bitmap, 0xFF, bitmap_size);
    
    // Create mutex
    pool->mutex = xSemaphoreCreateMutex();
    if (pool->mutex == NULL) {
        heap_caps_free(pool->free_bitmap);
        heap_caps_free(pool->base_ptr);
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

void gc_pool_deinit(gc_memory_pool_t* pool) {
    if (pool == NULL) {
        return;
    }
    
    if (pool->mutex != NULL) {
        vSemaphoreDelete(pool->mutex);
    }
    
    if (pool->free_bitmap != NULL) {
        heap_caps_free(pool->free_bitmap);
    }
    
    if (pool->base_ptr != NULL) {
        heap_caps_free(pool->base_ptr);
    }
    
    memset(pool, 0, sizeof(gc_memory_pool_t));
}

void* gc_pool_alloc(gc_memory_pool_t* pool) {
    if (pool == NULL || pool->free_blocks == 0) {
        return NULL;
    }
    
    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return NULL;
    }
    
    // Find first free block
    for (size_t i = 0; i < pool->total_blocks; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        
        if (pool->free_bitmap[byte_idx] & (1 << bit_idx)) {
            // Mark as allocated
            pool->free_bitmap[byte_idx] &= ~(1 << bit_idx);
            pool->free_blocks--;
            
            void* ptr = (uint8_t*)pool->base_ptr + (i * pool->block_size);
            xSemaphoreGive(pool->mutex);
            return ptr;
        }
    }
    
    xSemaphoreGive(pool->mutex);
    return NULL;
}

void gc_pool_free(gc_memory_pool_t* pool, void* ptr) {
    if (pool == NULL || ptr == NULL) {
        return;
    }
    
    // Check if pointer is within pool range
    if (ptr < pool->base_ptr || 
        (uint8_t*)ptr >= (uint8_t*)pool->base_ptr + (pool->total_blocks * pool->block_size)) {
        return;
    }
    
    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    // Calculate block index
    size_t offset = (uint8_t*)ptr - (uint8_t*)pool->base_ptr;
    size_t block_idx = offset / pool->block_size;
    
    if (block_idx < pool->total_blocks) {
        size_t byte_idx = block_idx / 8;
        size_t bit_idx = block_idx % 8;
        
        // Mark as free
        pool->free_bitmap[byte_idx] |= (1 << bit_idx);
        pool->free_blocks++;
    }
    
    xSemaphoreGive(pool->mutex);
}

// Integration functions for existing systems
void gc_register_ble_cleanup(void) {
    ESP_LOGI(TAG, "Registering BLE cleanup");
    gc_force_cleanup_type(GC_OBJ_NETWORK_BUFFER);
}

void gc_register_wifi_cleanup(void) {
    ESP_LOGI(TAG, "Registering WiFi cleanup");
    gc_force_cleanup_type(GC_OBJ_NETWORK_BUFFER);
}

void gc_register_display_cleanup(void) {
    ESP_LOGI(TAG, "Registering Display cleanup");
    gc_force_cleanup_type(GC_OBJ_DISPLAY_BUFFER);
}

void gc_register_nfc_cleanup(void) {
    ESP_LOGI(TAG, "Registering NFC cleanup");
    gc_force_cleanup_type(GC_OBJ_NFC_BUFFER);
}

// Optimize memory for specific capability
void gc_optimize_for_capability(gc_capability_t capability) {
    ESP_LOGI(TAG, "Optimizing memory for capability %d", capability);
    
    // Force cleanup of objects with matching capability
    if (g_gc_manager == NULL || !g_gc_manager->is_initialized) {
        return;
    }

    if (xSemaphoreTake(g_gc_manager->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        gc_object_t* obj = g_gc_manager->object_list;
        while (obj != NULL) {
            gc_object_t* next = obj->next;
            if (obj->capability == capability && obj->ref_count <= 0) {
                ESP_LOGD(TAG, "Optimizing object with capability %d", capability);
                gc_cleanup_object(obj);
            }
            obj = next;
        }
        xSemaphoreGive(g_gc_manager->mutex);
    }
    
    // Force garbage collection
    gc_collect();
}
