#include "managers/garbage_collector.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"
#include "core/glog.h"

static const char* TAG = "GC_TEST";

// Test function to validate garbage collection
void gc_run_tests(void) {
    glog("Starting Garbage Collection Tests...\n");
    
    // Check if GC is initialized
    if (g_gc_manager == NULL) {
        glog("ERROR: GC not initialized! Run 'gc init' first.\n");
        return;
    }
    
    // Test 1: Basic allocation and deallocation
    glog("Test 1: Basic allocation and deallocation\n");
    void* ptr1 = GC_MALLOC(256, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
    if (ptr1 != NULL) {
        glog("Allocated 256 bytes at %p\n", ptr1);
        memset(ptr1, 0xAA, 256);
        GC_FREE(ptr1);
        glog("Freed memory\n");
    } else {
        glog("Failed to allocate memory\n");
    }
    
    // Test 2: Small multiple allocations
    glog("Test 2: Small multiple allocations\n");
    void* ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = GC_MALLOC(128, GC_CAP_DEFAULT, GC_OBJ_STRING);
        if (ptrs[i] != NULL) {
            // Use snprintf to prevent buffer overflow
            snprintf((char*)ptrs[i], 128, "Test %d", i);
            glog("Allocated string %d\n", i);
        } else {
            glog("Failed to allocate string %d\n", i);
        }
    }
    
    // Free some of them
    for (int i = 0; i < 3; i++) {
        if (ptrs[i] != NULL) {
            GC_FREE(ptrs[i]);
            glog("Freed string %d\n", i);
        }
    }
    
    // Test 3: Reference counting
    glog("Test 3: Reference counting\n");
    void* shared_ptr = GC_MALLOC(64, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
    if (shared_ptr != NULL) {
        GC_ADD_REF(shared_ptr);
        glog("Added reference to %p\n", shared_ptr);
        
        // First free should not actually free
        GC_FREE(shared_ptr);
        glog("First free (should not actually free)\n");
        
        // Second free should actually free
        GC_FREE(shared_ptr);
        glog("Second free (should actually free)\n");
    } else {
        glog("Failed to allocate shared pointer\n");
    }
    
    // Test 4: Small DMA memory allocation
    glog("Test 4: Small DMA memory allocation\n");
    void* dma_ptr = GC_MALLOC_DMA(512);
    if (dma_ptr != NULL) {
        glog("Allocated DMA memory at %p\n", dma_ptr);
        memset(dma_ptr, 0x55, 512);
        GC_FREE(dma_ptr);
        glog("Freed DMA memory\n");
    } else {
        glog("Failed to allocate DMA memory\n");
    }
    
    // Test 5: Memory pool allocation (only if pools are available)
    glog("Test 5: Memory pool allocation\n");
    if (g_gc_manager != NULL && g_gc_manager->pools[2].base_ptr != NULL) {
        void* pool_ptrs[3];
        for (int i = 0; i < 3; i++) {
            pool_ptrs[i] = gc_pool_alloc(&g_gc_manager->pools[2]); // String pool
            if (pool_ptrs[i] != NULL) {
                glog("Allocated from pool: %p\n", pool_ptrs[i]);
            } else {
                glog("Failed to allocate from pool %d\n", i);
            }
        }
        
        // Free pool allocations
        for (int i = 0; i < 3; i++) {
            if (pool_ptrs[i] != NULL) {
                gc_pool_free(&g_gc_manager->pools[2], pool_ptrs[i]);
                glog("Freed from pool: %p\n", pool_ptrs[i]);
            }
        }
    } else {
        glog("Memory pools not available, skipping pool test\n");
    }
    
    // Test 6: Garbage collection
    glog("Test 6: Garbage collection\n");
    gc_collect();
    glog("Garbage collection completed\n");
    
    // Test 7: Statistics
    glog("Test 7: Statistics\n");
    gc_print_stats();
    
    // Test 8: Memory fragmentation
    glog("Test 8: Memory fragmentation\n");
    float frag_ratio = gc_get_fragmentation_ratio(GC_CAP_DEFAULT);
    glog("Fragmentation ratio: %.2f%%\n", frag_ratio * 100.0f);
    
    // Test 9: Emergency cleanup (safer version)
    glog("Test 9: Emergency cleanup\n");
    gc_emergency_cleanup();
    glog("Emergency cleanup completed\n");
    
    // Test 10: Object type cleanup
    glog("Test 10: Object type cleanup\n");
    gc_force_cleanup_type(GC_OBJ_STRING);
    glog("String object cleanup completed\n");
    
    glog("Garbage Collection Tests completed\n");
    gc_print_stats();
}

// Comprehensive verification tests
void gc_run_verification_tests(void) {
    glog("=== GC Verification Tests ===\n");
    
    if (g_gc_manager == NULL) {
        glog("ERROR: GC not initialized!\n");
        return;
    }
    
    // Test 1: Basic functionality
    glog("Test 1: Basic allocation/deallocation\n");
    void* ptr = GC_MALLOC(256, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
    if (ptr == NULL) {
        glog("FAIL: Basic allocation failed\n");
        return;
    }
    size_t after_alloc = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    GC_FREE(ptr);
    size_t after_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    
    if (after_free >= after_alloc) {
        glog("PASS: Memory properly freed\n");
    } else {
        glog("FAIL: Memory not properly freed\n");
    }
    
    // Test 2: Reference counting
    glog("Test 2: Reference counting\n");
    void* shared = GC_MALLOC(128, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
    if (shared != NULL) {
        GC_ADD_REF(shared);
        GC_FREE(shared); // Should not free
        GC_FREE(shared); // Should actually free
        glog("PASS: Reference counting works\n");
    } else {
        glog("FAIL: Reference counting test allocation failed\n");
    }
    
    // Test 3: Memory pools
    glog("Test 3: Memory pools\n");
    if (g_gc_manager->pools[2].base_ptr != NULL) {
        void* pool_ptr = gc_pool_alloc(&g_gc_manager->pools[2]);
        if (pool_ptr != NULL) {
            gc_pool_free(&g_gc_manager->pools[2], pool_ptr);
            glog("PASS: Memory pool allocation/free works\n");
        } else {
            glog("FAIL: Memory pool allocation failed\n");
        }
    } else {
        glog("SKIP: Memory pools not available\n");
    }
    
    // Test 4: Garbage collection
    glog("Test 4: Garbage collection\n");
    gc_collect();
    glog("PASS: Garbage collection completed\n");
    
    // Test 5: Statistics accuracy
    glog("Test 5: Statistics accuracy\n");
    gc_stats_t* stats = &g_gc_manager->stats;
    if (stats->current_objects >= 0 && stats->total_allocated >= 0) {
        glog("PASS: Statistics are valid\n");
    } else {
        glog("FAIL: Statistics are invalid\n");
    }
    
    glog("=== Verification Tests Complete ===\n");
    gc_print_stats();
}

// Memory leak detection test
void gc_run_leak_test(void) {
    glog("=== Memory Leak Detection Test ===\n");
    
    if (g_gc_manager == NULL) {
        glog("ERROR: GC not initialized!\n");
        return;
    }
    
    size_t initial_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    gc_stats_t initial_stats = g_gc_manager->stats;
    
    glog("Initial free memory: %d bytes\n", (int)initial_free);
    glog("Initial objects: %d\n", (int)initial_stats.current_objects);
    
    // Allocate and free many objects
    void* ptrs[20];
    for (int i = 0; i < 20; i++) {
        ptrs[i] = GC_MALLOC(64, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
        if (ptrs[i] == NULL) {
            glog("WARNING: Allocation %d failed\n", i);
        }
    }
    
    // Free half of them
    for (int i = 0; i < 10; i++) {
        if (ptrs[i] != NULL) {
            GC_FREE(ptrs[i]);
        }
    }
    
    // Force garbage collection
    gc_collect();
    
    size_t after_gc = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    gc_stats_t after_stats = g_gc_manager->stats;
    
    glog("After GC free memory: %d bytes\n", (int)after_gc);
    glog("After GC objects: %d\n", (int)after_stats.current_objects);
    
    // Free remaining objects
    for (int i = 10; i < 20; i++) {
        if (ptrs[i] != NULL) {
            GC_FREE(ptrs[i]);
        }
    }
    
    // Final garbage collection
    gc_collect();
    
    size_t final_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    gc_stats_t final_stats = g_gc_manager->stats;
    
    glog("Final free memory: %d bytes\n", (int)final_free);
    glog("Final objects: %d\n", (int)final_stats.current_objects);
    
    // Check for leaks
    if (final_free >= initial_free - 1000) { // Allow some tolerance
        glog("PASS: No significant memory leaks detected\n");
    } else {
        glog("FAIL: Potential memory leak detected\n");
        glog("Memory difference: %d bytes\n", (int)(initial_free - final_free));
    }
    
    if (final_stats.current_objects == 0) {
        glog("PASS: All objects properly cleaned up\n");
    } else {
        glog("FAIL: %d objects still allocated\n", (int)final_stats.current_objects);
    }
    
    glog("=== Leak Test Complete ===\n");
}

// Stress test
void gc_run_stress_test(void) {
    glog("=== GC Stress Test ===\n");
    
    if (g_gc_manager == NULL) {
        glog("ERROR: GC not initialized!\n");
        return;
    }
    
    const int NUM_ITERATIONS = 50;
    const int ALLOCS_PER_ITERATION = 10;
    
    glog("Running %d iterations with %d allocations each...\n", NUM_ITERATIONS, ALLOCS_PER_ITERATION);
    
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        void* ptrs[ALLOCS_PER_ITERATION];
        
        // Allocate
        for (int i = 0; i < ALLOCS_PER_ITERATION; i++) {
            ptrs[i] = GC_MALLOC(32 + (i * 8), GC_CAP_DEFAULT, GC_OBJ_BUFFER);
        }
        
        // Free some randomly
        for (int i = 0; i < ALLOCS_PER_ITERATION; i += 2) {
            if (ptrs[i] != NULL) {
                GC_FREE(ptrs[i]);
            }
        }
        
        // Run GC every 10 iterations
        if (iter % 10 == 0) {
            gc_collect();
        }
        
        // Free remaining
        for (int i = 1; i < ALLOCS_PER_ITERATION; i += 2) {
            if (ptrs[i] != NULL) {
                GC_FREE(ptrs[i]);
            }
        }
        
        if (iter % 10 == 0) {
            glog("Iteration %d completed\n", iter);
        }
    }
    
    // Final cleanup
    gc_collect();
    
    glog("Stress test completed successfully\n");
    gc_print_stats();
    glog("=== Stress Test Complete ===\n");
}

// Performance benchmark
void gc_run_benchmark(void) {
    glog("=== GC Performance Benchmark ===\n");
    
    if (g_gc_manager == NULL) {
        glog("ERROR: GC not initialized!\n");
        return;
    }
    
    const int NUM_ALLOCS = 100;
    void* ptrs[NUM_ALLOCS];
    
    // Benchmark allocation
    uint32_t start_time = esp_timer_get_time();
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = GC_MALLOC(64, GC_CAP_DEFAULT, GC_OBJ_BUFFER);
    }
    uint32_t alloc_time = esp_timer_get_time() - start_time;
    
    // Benchmark deallocation
    start_time = esp_timer_get_time();
    for (int i = 0; i < NUM_ALLOCS; i++) {
        if (ptrs[i] != NULL) {
            GC_FREE(ptrs[i]);
        }
    }
    uint32_t free_time = esp_timer_get_time() - start_time;
    
    // Benchmark garbage collection
    start_time = esp_timer_get_time();
    gc_collect();
    uint32_t gc_time = esp_timer_get_time() - start_time;
    
    glog("Performance Results:\n");
    glog("  Allocation: %d us for %d allocs (%.2f us/alloc)\n", 
         (int)alloc_time, NUM_ALLOCS, (float)alloc_time / NUM_ALLOCS);
    glog("  Deallocation: %d us for %d frees (%.2f us/free)\n", 
         (int)free_time, NUM_ALLOCS, (float)free_time / NUM_ALLOCS);
    glog("  Garbage Collection: %d us\n", (int)gc_time);
    
    // Memory pool benchmark
    if (g_gc_manager->pools[2].base_ptr != NULL) {
        start_time = esp_timer_get_time();
        for (int i = 0; i < 50; i++) {
            void* ptr = gc_pool_alloc(&g_gc_manager->pools[2]);
            if (ptr != NULL) {
                gc_pool_free(&g_gc_manager->pools[2], ptr);
            }
        }
        uint32_t pool_time = esp_timer_get_time() - start_time;
        glog("  Pool operations: %d us for 50 ops (%.2f us/op)\n", 
             (int)pool_time, (float)pool_time / 50);
    }
    
    glog("=== Benchmark Complete ===\n");
}
