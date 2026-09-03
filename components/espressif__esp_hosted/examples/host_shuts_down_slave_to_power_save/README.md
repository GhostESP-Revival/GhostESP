# Host-Controlled Slave Shutdown When Not in Use

This example demonstrates **host-side control of an ESP slave** to reduce power consumption by shutting the slave down when idle and waking it up on demand to resume normal operation.

## Supported Platforms

### Host Platforms

| ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Other MCU Hosts |
| ----: | -------------: | -------------: | -------------: | -------------: | --------------: |

### Co-Processor (Slave) Platforms

| ESP32 | ESP32-C Series | ESP32-S Series |
| ----: | -------------: | -------------: |

## Scope

The example implements **slave-side power saving** by keeping the device in reset when not in use.
No host-side power saving is included.

For host-side power saving while the slave maintains connectivity, see the `host_network_split_power_save` example.

## Implementation Overview

The main logic is in `main/main.c` and performs the following sequence:

1. Clean up Wi-Fi (disconnect, stop, deinit)
2. Deinitialize the `esp-hosted` transport (`esp_hosted_deinit()`)
3. Power-cycle the slave by toggling a GPIO
4. Reinitialize the transport (`esp_hosted_init()`) and reconnect to the slave
5. Reinitialize Wi-Fi and reconnect to the access point

**Notes:**

* GPIO control functions (`platform_gpio_*`) are stubs; implement them for your host platform.
* Update Wi-Fi credentials and GPIO pin definitions before running the example.

## Build and Run

Build using the **ESP-IDF build system** following the top-level project instructions.

The application initializes the host and slave, then repeatedly:

* Shuts down the slave to save power
* Waits for a fixed interval
* Wakes the slave and restores connectivity

The cycle repeats approximately every **30 seconds**.

---

## Memory Leak Verification (Optional)

Memory tracing is included to **verify heap stability** across repeated slave reset and transport deinit/init cycles.
This is for validation only and can be disabled:

```c
#define MEMORY_DEBUG_ENABLE 0
```

in `main/memory_debug.h`.

### Behavior When Enabled

* Captures heap usage at key points in the cycle
* Traces allocations that remain at cycle boundaries
* Records free heap and minimum-ever free heap for trend analysis

> Transient LWIP timer allocations may appear in traces if still active at the end of a cycle. These are expected and typically released in subsequent cycles.

### Sample Heap Trace Output

```
====== Heap Trace Summary ======
Mode: Heap Trace Leaks
48 bytes 'leaked' in trace (3 allocations)
total allocations: 345
total frees: 345
================================
```

### Heap Stability Across Cycles (Example)

| Cycle | Free Heap (Example) | Min-Ever Free Heap (Example) |
| ----: | ------------------: | ---------------------------: |
|     2 |              530 KB |                       481 KB |
|    29 |              530 KB |                       481 KB |
|    73 |              530 KB |                       481 KB |

**Values are for reference only and may vary depending on platform, configuration, and build options.**

### Interpretation

Heap usage remains stable across cycles with no progressive reduction in free or minimum-ever heap, indicating **no cumulative memory leaks** and confirming reliable behavior of the slave shutdown and reinitialization flow.
