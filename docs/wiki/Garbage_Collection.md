# Garbage Collection System

## Overview

The Ghost ESP project now includes a comprehensive garbage collection system designed specifically for ESP32 microcontrollers. This system provides automatic memory management, reduces memory fragmentation, and helps prevent memory leaks in embedded applications.

## Features

### Core Functionality
- **Reference Counting**: Automatic cleanup of objects when reference count reaches zero
- **Mark-and-Sweep**: Handles cyclic references and comprehensive memory cleanup
- **Memory Pool Management**: Fast allocation/deallocation for common object sizes
- **Heap Defragmentation**: Reduces memory fragmentation over time
- **Memory Monitoring**: Real-time tracking of memory usage and statistics

### Memory Capabilities
- **Default Memory**: Standard 8-bit accessible memory
- **DMA Memory**: Memory suitable for DMA operations
- **Internal Memory**: Internal RAM only
- **SPIRAM**: External PSRAM when available
- **DMA Internal**: Internal RAM with DMA capability

### Object Types
- **Generic**: General purpose objects
- **String**: String buffers
- **Buffer**: Data buffers
- **Task Stack**: FreeRTOS task stacks
- **DMA Buffer**: DMA-capable buffers
- **Network Buffer**: Network-related buffers
- **Display Buffer**: Display-related buffers
- **NFC Buffer**: NFC-related buffers

## Usage

### Basic Allocation

```c
// Allocate memory with garbage collection
void* ptr = GC_MALLOC(size, GC_CAP_DEFAULT, GC_OBJ_BUFFER);

// Allocate and zero memory
void* ptr = GC_CALLOC(num, size, GC_CAP_DEFAULT, GC_OBJ_BUFFER);

// Reallocate memory
void* new_ptr = GC_REALLOC(ptr, new_size, GC_CAP_DEFAULT, GC_OBJ_BUFFER);

// Free memory
GC_FREE(ptr);
```

### Reference Counting

```c
// Add reference
GC_ADD_REF(ptr);

// Remove reference (same as GC_FREE)
GC_REMOVE_REF(ptr);
```

### Convenience Macros

```c
// DMA memory allocation
void* dma_ptr = GC_MALLOC_DMA(size);

// String allocation
char* str = GC_MALLOC_STRING(size);

// Buffer allocation
void* buf = GC_MALLOC_BUFFER(size);

// Network buffer allocation
void* net_buf = GC_MALLOC_NETWORK(size);
```

## Command Line Interface

The garbage collector provides a comprehensive command-line interface:

### Basic Commands

```bash
# Show GC statistics
gc stats

# Run garbage collection
gc collect

# Defragment memory
gc defrag

# Cleanup unused objects
gc cleanup

# Emergency cleanup
gc emergency

# Show all tracked objects
gc objects

# Show memory pool status
gc pools
```

### System-Specific Commands

```bash
# Optimize memory for BLE
gc ble

# Optimize memory for WiFi
gc wifi

# Run comprehensive tests
gc test
```

## Integration with Existing Systems

### BLE Manager Integration

The garbage collector is integrated with the BLE manager to provide automatic memory optimization:

```c
// Automatic memory optimization before BLE initialization
gc_register_ble_cleanup();
gc_optimize_for_capability(GC_CAP_DMA_INTERNAL);
```

### WiFi Manager Integration

Similar integration is available for WiFi operations:

```c
// Optimize memory for WiFi operations
gc_register_wifi_cleanup();
gc_optimize_for_capability(GC_CAP_DMA_INTERNAL);
```

## Configuration

### Auto-GC Settings

```c
// Enable/disable automatic garbage collection
gc_set_auto_gc(true, 30000);  // Enable with 30-second interval

// Enable/disable automatic defragmentation
gc_set_auto_defrag(true, 300000);  // Enable with 5-minute interval

// Set low memory threshold
gc_set_low_memory_threshold(50 * 1024);  // 50KB threshold
```

### Memory Pool Configuration

The system includes pre-configured memory pools:

- **Default Pool**: 8KB blocks (32 blocks)
- **DMA Pool**: 4KB blocks (16 blocks)
- **String Pool**: 256B blocks (128 blocks)
- **Buffer Pool**: 1KB blocks (64 blocks)
- **Network Pool**: 2KB blocks (32 blocks)

## Statistics and Monitoring

### Available Statistics

- Total allocated memory
- Total freed memory
- Current number of objects
- Peak number of objects
- Memory fragmentation ratio
- Number of GC cycles
- Number of defragmentation cycles

### Memory Information

```c
// Get free memory for specific capability
size_t free_mem = gc_get_free_memory(GC_CAP_DMA_INTERNAL);

// Get total memory for specific capability
size_t total_mem = gc_get_total_memory(GC_CAP_DMA_INTERNAL);

// Check if memory is low
bool is_low = gc_is_memory_low(GC_CAP_DEFAULT);

// Get fragmentation ratio
float frag_ratio = gc_get_fragmentation_ratio(GC_CAP_DEFAULT);
```

## Best Practices

### Memory Allocation

1. **Use appropriate capabilities**: Choose the right memory capability for your use case
2. **Use appropriate object types**: This helps with cleanup and optimization
3. **Avoid mixing GC and standard malloc**: Stick to one allocation method per object

### Reference Management

1. **Add references when sharing objects**: Use `GC_ADD_REF()` when passing objects between functions
2. **Remove references when done**: Use `GC_REMOVE_REF()` or `GC_FREE()` when no longer needed
3. **Be careful with cyclic references**: The mark-and-sweep algorithm handles these, but it's better to avoid them

### Performance Optimization

1. **Use memory pools for frequent allocations**: Pools provide faster allocation/deallocation
2. **Monitor fragmentation**: Use `gc defrag` when fragmentation is high
3. **Use system-specific optimization**: Use `gc ble` or `gc wifi` before critical operations

## Troubleshooting

### Common Issues

1. **Memory not being freed**: Check reference counts and ensure proper cleanup
2. **High fragmentation**: Run `gc defrag` or enable automatic defragmentation
3. **Low memory warnings**: Use `gc emergency` or optimize for specific capabilities

### Debug Commands

```bash
# Show detailed object information
gc objects

# Show memory pool status
gc pools

# Run comprehensive tests
gc test

# Show current statistics
gc stats
```

### Memory Leak Detection

The system tracks all allocations and provides detailed statistics. Use the following approach:

1. Check statistics before and after operations
2. Use `gc objects` to see all tracked objects
3. Look for objects with high reference counts that should be freed
4. Use `gc cleanup` to force cleanup of unused objects

## Performance Impact

### Overhead

- **Memory overhead**: ~32 bytes per tracked object
- **CPU overhead**: Minimal during normal operation
- **GC cycles**: Run automatically in background or on-demand

### Benefits

- **Reduced memory leaks**: Automatic cleanup prevents memory leaks
- **Reduced fragmentation**: Defragmentation keeps memory organized
- **Better memory utilization**: Pools provide efficient allocation
- **System stability**: Prevents out-of-memory conditions

## Future Enhancements

Planned improvements include:

1. **Compacting garbage collector**: Move objects to reduce fragmentation
2. **Generational GC**: Separate young and old objects for better performance
3. **Memory pressure detection**: Automatic GC triggering based on memory pressure
4. **Integration with more managers**: Extend integration to all system managers

## API Reference

For complete API documentation, see the header file:
`include/managers/garbage_collector.h`

The system provides a comprehensive set of functions for memory management, statistics, and system integration.
