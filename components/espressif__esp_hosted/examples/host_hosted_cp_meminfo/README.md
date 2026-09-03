| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 | ESP32-C Series | ESP32-S Series |
| ----------------------- | ----- | -------------- | -------------- |

# ESP-Hosted Co-processor Memory Info Example

This host example enables a memory monitor on the co-processor. This can be used to monitor the heap usage on the co-processor. Once enabled, the host can:

- query the heap usage on the co-processor at any time, or
- get a periodic memory report from the co-processor, or
- get a memory report only when the heap size on the co-processor falls below memory thresholds set by the host

This has been tested with the ESP32-C5 co-processor, flashed with the standard [ESP-Hosted slave example](https://components.espressif.com/components/espressif/esp_hosted/examples/slave) from the ESP Component Registry, or built from the `slave` directory from ESP-Hosted Github.

To show the heap size changing on the co-processor, the example enables Wi-Fi, then connects and disconnects from an AP.

## Prerequisite

### Setting up the Co-Processor

Fetch the co-processor code using the ESP Registry [ESP-Hosted-MCU Slave example](https://components.espressif.com/components/espressif/esp_hosted/examples/slave?language=en).

Quick steps:

```sh
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave
idf.py set-target <cp_chip_type>
```

Start `idf.py menuconfig` and ensure that `Example Configuration` ---> `Enable Memory Monitor Interface` is enabled.

> [!NOTE]
> PSRAM must be enabled on the co-processor to get memory reports on external memory and to set the external memory thresholds. Ensure that `Component config` ---> `ESP PSRAM` ---> `Support for external, SPI-connected RAM` is enabled.

Now build and flash the code on the co-processor.

```sh
idf.py -p <cp_port> flash monitor
```

Where `cp_chip_type` is one of ESP32, ESP32-C Series or ESP32-S Series as the co-processor.

Alternatively, you built from the [slave directory](https://github.com/espressif/esp-hosted-mcu/tree/main/slave) of the ESP-Hosted-MCU GitHub repository.

### Building the Host

Start `idf.py menuconfig` and ensure that `Component Config` ---> `ESP-Hosted config` ---> `Enable Memory Monitor Interface` is enabled. There are three ways to configure the example:

1. **Do a one time query**: enable `Query Memory Info only once`
2. **Get a periodic report**: enter a `Memory Monitor Interval` (default is `10` seconds) and enable `Always send a memory report`
3. **Get a report only when memory falls below a threshold**: disable `Always send a memory report` and enter one or more threshold values for DMA / 8-bit for internal / external memory

**See [the section below](#how-to-use-this-example) for more information on how to configure the example.**

Now build and flash the code on the host.

```sh
idf.py -p <host_port> flash monitor
```

## Setting up the Memory Info Request


The example uses this API call to set the memory monitor:

`esp_err_t esp_hosted_set_mem_monitor(esp_hosted_config_mem_monitor_t *config, esp_hosted_curr_mem_info_t *curr_mem_info)`

where:

- `esp_hosted_config_mem_monitor_t`: pointer to the structure containing the mem monitor configuration
- `esp_hosted_curr_mem_info_t`: pointer to a structure that will hold the returned memory report when the call returns. This is valid only if the return value is `ESP_OK`

See [`esp_hosted_misc_types.h`](https://github.com/espressif/esp-hosted-mcu/blob/main/host/esp_hosted_misc_types.h) for details on the structures.

For example, to enable the memory monitor to periodically return a report every 10 seconds:

```
esp_hosted_curr_mem_info_t curr_mem_info = { 0 };
esp_hosted_config_mem_monitor_t config = {
	.config = ESP_HOSTED_MEMMONITOR_ENABLE,
	.report_always = true,
	.interval_sec = 10,
	// since we always get a report, no need to set thresholds
	.internal_mem.threshold_mem_dma = 0,
	.internal_mem.threshold_mem_8bit = 0,
	.external_mem.threshold_mem_dma = 0,
	.external_mem.threshold_mem_8bit = 0,
};

esp_err_t res = esp_hosted_set_mem_monitor(&config, &curr_mem_info);

```

`curr_mem_info` contains the current heap info from the co-processor if res is `ESP_OK`.

## Memory Info Reported

The returned memory report contains this information from the co-processor:

- the current memory report configuration on the co-processor
- the current total heap size
- the current internal and external free and largest free block for DMA or for 8/16/...-bit access

## Memory Monitor Event

If periodic reporting is enabled, or when heap memory falls below a set threshold, a memory report event is sent to the host by the co-processor, with an event base of `ESP_HOSTED_EVENT`:

- `ESP_HOSTED_EVENT_MEM_MONITOR`: sent by the co-processor to the Host with a memory usage report of type `esp_hosted_event_mem_info_t` as the event data.

The event data contains this information from the co-processor:

- the current total free heap size
- the minimum free heap size since boot
- the current internal and external free and largest free block for DMA or for 8/16/...-bit access

## How to Use This Example

The example shows how heap usage on the co-processor changes when Wi-Fi operations are performed:

1. Starts the example with Wi-Fi disabled
2. Enable Wi-Fi
3. Connect to an AP
4. Disconnected from an AP

Delays are deliberately introduced after each step to give time for memory reports to be returned (if enabled).

The example also shows how to use the memory monitor in the following ways:

1. Get a one time report: the memory report is read once on app start up.
2. Periodically generate a memory report: a memory report is generated periodically.
3. Generate a memory report when memory usage drops below a threshold: no memory report is received until after the Wi-Fi connection has cause heap space to drop below a set threshold.

The hardware used in the example is a ESP32-P4 connected to a ESP32-C5 as the co-processor with PSRAM on the C5 enabled.

### Configuring the memory report options on the ESP32-P4 host

Set the correct chip target:

```bash
idf.py set-target esp32p4
```

Open the project configuration menu:

```bash
idf.py menuconfig
```

Set the memory report configuration as required. The sections below show the configuration setup. Then, configure the Wi-Fi station parameters at needed:

- Set WiFi SSID.
- Set WiFi Password.

Optional: If you need, change the other Wi-Fi options according to your requirements.

### Build and Flash

Run `idf.py -p HOST_PORT flash monitor` to build, flash and monitor the
project.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://idf.espressif.com/) for full
steps to configure and use ESP-IDF to build projects.

## Example Output

### Example 1: Generate a one time report

A one-time memory report is queried by the host.

In the Kconfig `Example Configuration`:

- enable `Query Memory Info only once` (other memory report settings are now hidden)
- set the required Wi-Fi settings

This is the example output. A report is generated only once.

<details>

<summary>Select to see output</summary>

```
[...]
I (1867) hosted_meminfo: ESP-Hosted Transport is UP
I (1997) hosted_meminfo: ESP-Hosted is ready
I (2137) RPC_WRAP: Coprocessor Boot-up
I (2137) hosted_meminfo: *** got INIT event from co-processor ***
I (2137) hosted_meminfo: *** Co-processor Reset Reason 1 ***
I (2137) hosted_meminfo: ======= Current Co-processor Mem Info =====
I (2137) hosted_meminfo: mem monitoring enabled : 0
I (2137) hosted_meminfo: report always          : 0
I (2137) hosted_meminfo: reporting interval     : 0
I (2137) hosted_meminfo: current heap size      : 4299348
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: |                   |           | Largest    |
I (2137) hosted_meminfo: | Current Heap Size | Free Size | Free Block |
I (2137) hosted_meminfo: |-------------------|-----------|------------|
I (2137) hosted_meminfo: | Internal DMA      |    123359 |      86016 |
I (2137) hosted_meminfo: | Internal 8Bit     |    139303 |      86016 |
I (2137) hosted_meminfo: | External DMA      |         0 |          0 |
I (2137) hosted_meminfo: | External 8Bit     |   4191920 |    4128768 |
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: delaying for 10 secs before connecting
I (12137) transport: Attempt connection with slave: retry[0]
I (12137) transport: Transport is already up
I (13187) wifi station: wifi_init_sta finished.
I (13187) RPC_WRAP: ESP Event: wifi station started
I (13187) H_API: esp_wifi_remote_connect
I (13207) RPC_WRAP: ESP Event: wifi station started
I (13807) RPC_WRAP: ESP Event: Station mode: Connected
I (14217) RPC_WRAP: ESP Event: Station mode: Connected
I (14217) RPC_WRAP: ESP Event: Station mode: Connected
I (14237) esp_wifi_remote: esp_wifi_internal_reg_rxcb: sta: 0x40046e20
--- 0x40046e20: wifi_sta_receive at /home/kysoh/esp/github_esp-idf/components/esp_wifi/src/wifi_netif.c:38
I (15257) wifi station: got ip:10.10.2.152
I (15257) esp_netif_handlers: sta ip: 10.10.2.152, mask: 255.255.255.0, gw: 10.10.2.1
I (15257) wifi station: connected to ap SSID:xxx password:yyy
I (15257) hosted_meminfo: delaying for 10 secs before disconnecting
I (25277) main_task: Returned from app_main()
I (25287) RPC_WRAP: ESP Event: Station mode: Disconnected
I (25287) wifi station: do not reconnect
[...]
```

</details>

### Example 2: Periodically generate a memory report

The co-processor sends a memory report periodically.

In the `Example Configuration`:

- disable `Query Memory Info only once`
- set `Memory Monitor Interval (in seconds)`. Default is 10 seconds
- enable `Always send a memory report`
- set the required Wi-Fi settings

This is the example output. A report is generated at every reporting interval.

<details>

<summary>Select to see output</summary>

```
[...]
I (1867) hosted_meminfo: ESP-Hosted Transport is UP
I (1997) hosted_meminfo: ESP-Hosted is ready
I (2137) RPC_WRAP: Coprocessor Boot-up
I (2137) hosted_meminfo: *** got INIT event from co-processor ***
I (2137) hosted_meminfo: *** Co-processor Reset Reason 1 ***
I (2137) hosted_meminfo: ======= Current Co-processor Mem Info =====
I (2137) hosted_meminfo: mem monitoring enabled : 1
I (2137) hosted_meminfo: report always          : 1
I (2137) hosted_meminfo: reporting interval     : 10
I (2137) hosted_meminfo: current heap size      : 4299280
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: |                   |           | Largest    |
I (2137) hosted_meminfo: | Current Heap Size | Free Size | Free Block |
I (2137) hosted_meminfo: |-------------------|-----------|------------|
I (2137) hosted_meminfo: | Internal DMA      |    123291 |      86016 |
I (2137) hosted_meminfo: | Internal 8Bit     |    139235 |      86016 |
I (2137) hosted_meminfo: | External DMA      |         0 |          0 |
I (2137) hosted_meminfo: | External 8Bit     |   4191920 |    4128768 |
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: delaying for 20 secs before connecting
I (12137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (12137) hosted_meminfo: Total free heap size : 4299532
I (12137) hosted_meminfo: Min free heap size   : 4295348
I (12137) hosted_meminfo: ----------------------------------------------
I (12137) hosted_meminfo: |                   |           | Largest    |
I (12137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (12137) hosted_meminfo: |-------------------|-----------|------------|
I (12137) hosted_meminfo: | Internal DMA      |    123699 |      86016 |
I (12137) hosted_meminfo: | Internal 8Bit     |    139643 |      86016 |
I (12137) hosted_meminfo: | External DMA      |         0 |          0 |
I (12137) hosted_meminfo: | External 8Bit     |   4191920 |    4128768 |
I (12137) hosted_meminfo: ----------------------------------------------
I (22137) transport: Attempt connection with slave: retry[0]
I (22137) transport: Transport is already up
I (22287) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (22287) hosted_meminfo: Total free heap size : 4288288
I (22287) hosted_meminfo: Min free heap size   : 4288288
I (22287) hosted_meminfo: ----------------------------------------------
I (22287) hosted_meminfo: |                   |           | Largest    |
I (22287) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (22287) hosted_meminfo: |-------------------|-----------|------------|
I (22287) hosted_meminfo: | Internal DMA      |    112455 |      77824 |
I (22287) hosted_meminfo: | Internal 8Bit     |    128399 |      77824 |
I (22287) hosted_meminfo: | External DMA      |         0 |          0 |
I (22287) hosted_meminfo: | External 8Bit     |   4191920 |    4128768 |
I (22287) hosted_meminfo: ----------------------------------------------
I (23187) wifi station: wifi_init_sta finished.
I (23197) RPC_WRAP: ESP Event: wifi station started
I (23197) H_API: esp_wifi_remote_connect
I (23207) RPC_WRAP: ESP Event: wifi station started
I (23717) RPC_WRAP: ESP Event: Station mode: Connected
I (24227) RPC_WRAP: ESP Event: Station mode: Connected
I (24227) RPC_WRAP: ESP Event: Station mode: Connected
I (24237) esp_wifi_remote: esp_wifi_internal_reg_rxcb: sta: 0x40046e38
--- 0x40046e38: wifi_sta_receive at /home/kysoh/esp/github_esp-idf/components/esp_wifi/src/wifi_netif.c:38
I (25277) wifi station: got ip:10.10.2.152
I (25277) esp_netif_handlers: sta ip: 10.10.2.152, mask: 255.255.255.0, gw: 10.10.2.1
I (25277) wifi station: connected to ap SSID:xxx password:yyy
I (25277) hosted_meminfo: delaying for 20 secs before disconnecting
I (32137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (32137) hosted_meminfo: Total free heap size : 4256044
I (32137) hosted_meminfo: Min free heap size   : 4253944
I (32137) hosted_meminfo: ----------------------------------------------
I (32137) hosted_meminfo: |                   |           | Largest    |
I (32137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (32137) hosted_meminfo: |-------------------|-----------|------------|
I (32137) hosted_meminfo: | Internal DMA      |     80279 |      45056 |
I (32137) hosted_meminfo: | Internal 8Bit     |     96223 |      45056 |
I (32137) hosted_meminfo: | External DMA      |         0 |          0 |
I (32137) hosted_meminfo: | External 8Bit     |   4191852 |    4128768 |
I (32137) hosted_meminfo: ----------------------------------------------
I (42137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (42137) hosted_meminfo: Total free heap size : 4256044
I (42137) hosted_meminfo: Min free heap size   : 4253944
I (42137) hosted_meminfo: ----------------------------------------------
I (42137) hosted_meminfo: |                   |           | Largest    |
I (42137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (42137) hosted_meminfo: |-------------------|-----------|------------|
I (42137) hosted_meminfo: | Internal DMA      |     80279 |      45056 |
I (42137) hosted_meminfo: | Internal 8Bit     |     96223 |      45056 |
I (42137) hosted_meminfo: | External DMA      |         0 |          0 |
I (42137) hosted_meminfo: | External 8Bit     |   4191852 |    4128768 |
I (42137) hosted_meminfo: ----------------------------------------------
I (45297) main_task: Returned from app_main()
I (45307) RPC_WRAP: ESP Event: Station mode: Disconnected
I (45307) wifi station: do not reconnect
I (52137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (52137) hosted_meminfo: Total free heap size : 4256900
I (52137) hosted_meminfo: Min free heap size   : 4253944
I (52137) hosted_meminfo: ----------------------------------------------
I (52137) hosted_meminfo: |                   |           | Largest    |
I (52137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (52137) hosted_meminfo: |-------------------|-----------|------------|
I (52137) hosted_meminfo: | Internal DMA      |     81135 |      45056 |
I (52137) hosted_meminfo: | Internal 8Bit     |     97079 |      45056 |
I (52137) hosted_meminfo: | External DMA      |         0 |          0 |
I (52137) hosted_meminfo: | External 8Bit     |   4191852 |    4128768 |
I (52137) hosted_meminfo: ----------------------------------------------
[...]
```

</details>

### Example 3: Generate a memory report when heap memory drops below threshold

The co-processor only sends a memory report when heap memory drops below a threshold value. Here, a report is only sent while the station is connected to the AP.

In the previous example, the `Internal DMA` free size after connecting is `80279`, and after disconnecting is `81135`. We choose a value that is in between for the threshold: `81000`. Adjust this value as required based on your system.

In the `Example Configuration`:

- disable `Query Memory Info only once`
- set `Memory Monitor Interval (in seconds)`. Default is 10 seconds
- disable `Always send a memory report`
- set `Threshold for internal DMA Heap Memory (in bytes)` to `81000`
- set the threshold for other parameters to `0` to disable them
- set the required Wi-Fi settings

This is the example output. A report is generated only when the internal DMA heap memory is lower than the threshold of 81,000 bytes (only when connected to the AP).

<details>

<summary>Select to see output</summary>


```
[...]
I (1867) hosted_meminfo: ESP-Hosted Transport is UP
I (1997) hosted_meminfo: ESP-Hosted is ready
I (2137) RPC_WRAP: Coprocessor Boot-up
I (2137) hosted_meminfo: *** got INIT event from co-processor ***
I (2137) hosted_meminfo: *** Co-processor Reset Reason 1 ***
I (2137) hosted_meminfo: ======= Current Co-processor Mem Info =====
I (2137) hosted_meminfo: mem monitoring enabled : 1
I (2137) hosted_meminfo: report always          : 0
I (2137) hosted_meminfo: reporting interval     : 10
I (2137) hosted_meminfo: current heap size      : 4299292
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: |                   |           | Largest    |
I (2137) hosted_meminfo: | Current Heap Size | Free Size | Free Block |
I (2137) hosted_meminfo: |-------------------|-----------|------------|
I (2137) hosted_meminfo: | Internal DMA      |    123303 |      86016 |
I (2137) hosted_meminfo: | Internal 8Bit     |    139247 |      86016 |
I (2137) hosted_meminfo: | External DMA      |         0 |          0 |
I (2137) hosted_meminfo: | External 8Bit     |   4191920 |    4128768 |
I (2137) hosted_meminfo: ----------------------------------------------
I (2137) hosted_meminfo: delaying for 20 secs before connecting
I (22147) transport: Attempt connection with slave: retry[0]
I (22147) transport: Transport is already up
I (23197) wifi station: wifi_init_sta finished.
I (23207) RPC_WRAP: ESP Event: wifi station started
I (23207) H_API: esp_wifi_remote_connect
I (23217) RPC_WRAP: ESP Event: wifi station started
I (23817) RPC_WRAP: ESP Event: Station mode: Connected
I (24237) RPC_WRAP: ESP Event: Station mode: Connected
I (24237) RPC_WRAP: ESP Event: Station mode: Connected
I (24247) esp_wifi_remote: esp_wifi_internal_reg_rxcb: sta: 0x40046e32
--- 0x40046e32: wifi_sta_receive at /home/kysoh/esp/github_esp-idf/components/esp_wifi/src/wifi_netif.c:38
I (25267) wifi station: got ip:10.10.2.152
I (25267) esp_netif_handlers: sta ip: 10.10.2.152, mask: 255.255.255.0, gw: 10.10.2.1
I (25267) wifi station: connected to ap SSID:xxx password:yyy
I (25267) hosted_meminfo: delaying for 20 secs before disconnecting
I (32137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (32137) hosted_meminfo: Total free heap size : 4256020
I (32137) hosted_meminfo: Min free heap size   : 4253572
I (32137) hosted_meminfo: ----------------------------------------------
I (32137) hosted_meminfo: |                   |           | Largest    |
I (32137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (32137) hosted_meminfo: |-------------------|-----------|------------|
I (32137) hosted_meminfo: | Internal DMA      |     80255 |      47104 |
I (32137) hosted_meminfo: | Internal 8Bit     |     96199 |      47104 |
I (32137) hosted_meminfo: | External DMA      |         0 |          0 |
I (32137) hosted_meminfo: | External 8Bit     |   4191852 |    4128768 |
I (32137) hosted_meminfo: ----------------------------------------------
I (42137) hosted_meminfo: ======== Event: Co-processor Mem Info ========
I (42137) hosted_meminfo: Total free heap size : 4256020
I (42137) hosted_meminfo: Min free heap size   : 4253572
I (42137) hosted_meminfo: ----------------------------------------------
I (42137) hosted_meminfo: |                   |           | Largest    |
I (42137) hosted_meminfo: | Heap Size         | Free Size | Free Block |
I (42137) hosted_meminfo: |-------------------|-----------|------------|
I (42137) hosted_meminfo: | Internal DMA      |     80255 |      47104 |
I (42137) hosted_meminfo: | Internal 8Bit     |     96199 |      47104 |
I (42137) hosted_meminfo: | External DMA      |         0 |          0 |
I (42137) hosted_meminfo: | External 8Bit     |   4191852 |    4128768 |
I (42137) hosted_meminfo: ----------------------------------------------
[...]
```

</details>

# References

- ESP-IDF documentation on [Memory Capabilities](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/mem_alloc.html#memory-capabilities)
- ESP-IDF documentation on [Heap information](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/heap_debug.html#heap-information).
- ESP-IDF [capabilities of memory systems](https://github.com/espressif/esp-idf/blob/master/components/heap/include/esp_heap_caps.h)
