| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 | ESP32-C Series | ESP32-S Series |
| ----------------------- | ----- | -------------- | -------------- |

# BT Controller MAC Address and Initialisation Example

This example shows how to change the MAC Address. When run, a BLE
Scanner should show a `Bluedroid_Beacon` device with the assigned MAC
address.

For more information on the Bluedroid Beacon, see the ESP-IDF
[Bluedroid Beacon example](https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/ble_get_started/bluedroid/Bluedroid_Beacon).

This example can be modified to use NimBLE instead of Bluedroid.

To run the example without modifying the BT Mac address, run `idf.py
menuconfig` --> `Example Configuration` and turn off `Update MAC
Address of BT Controller` option.

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the
project.

(To exit the serial monitor, type ``Ctrl-]``.)

## Example Output

```
I (25) boot: ESP-IDF v6.0-dev-2343-g92b90afa77 2nd stage bootloader
I (26) boot: compile time Sep 19 2025 13:47:15
I (26) boot: Multicore bootloader
I (29) boot: chip revision: v1.0
I (30) boot: efuse block revision: v0.1
I (34) boot.esp32p4: SPI Speed      : 80MHz
I (38) boot.esp32p4: SPI Mode       : DIO
I (41) boot.esp32p4: SPI Flash Size : 2MB
I (45) boot: Enabling RNG early entropy source...
I (50) boot: Partition Table:
I (52) boot: ## Label            Usage          Type ST Offset   Length
I (59) boot:  0 nvs              WiFi data        01 02 00009000 00006000
I (65) boot:  1 phy_init         RF data          01 01 0000f000 00001000
I (72) boot:  2 factory          factory app      00 00 00010000 00100000
I (79) boot: End of partition table
I (82) esp_image: segment 0: paddr=00010020 vaddr=40070020 size=3166ch (202348) map
I (125) esp_image: segment 1: paddr=00041694 vaddr=30100000 size=00044h (    68) load
I (127) esp_image: segment 2: paddr=000416e0 vaddr=4ff00000 size=0e6ech ( 59116) load
I (141) esp_image: segment 3: paddr=0004fdd4 vaddr=4ff0e700 size=00244h (   580) load
I (143) esp_image: segment 4: paddr=00050020 vaddr=40000020 size=6b1f4h (438772) map
I (221) esp_image: segment 5: paddr=000bb21c vaddr=4ff0e944 size=02398h (  9112) load
I (228) boot: Loaded app from partition at offset 0x10000
I (228) boot: Disabling RNG early entropy source...
W (239) pmu_pvt: blk_version is less than 2, pvt auto dbias init not supported in efuse.
I (241) cpu_start: Multicore app
I (251) cpu_start: GPIO 38 and 37 are used as console UART I/O pins
I (252) cpu_start: Pro cpu start user code
I (252) cpu_start: cpu freq: 360000000 Hz
I (254) app_init: Application information:
I (257) app_init: Project name:     bt_mac_addr_init
I (262) app_init: App version:      bb05bb9c
I (266) app_init: Compile time:     Sep 19 2025 14:40:33
I (271) app_init: ELF file SHA256:  ab0fe993f...
I (275) app_init: ESP-IDF:          v6.0-dev-2343-g92b90afa77
I (281) efuse_init: Min chip rev:     v0.1
I (285) efuse_init: Max chip rev:     v1.99
I (289) efuse_init: Chip rev:         v1.0
I (293) heap_init: Initializing. RAM available for dynamic allocation:
I (299) heap_init: At 4FF130B0 len 00027F10 (159 KiB): RAM
I (304) heap_init: At 4FF3AFC0 len 00004BF0 (18 KiB): RAM
I (309) heap_init: At 4FF40000 len 00060000 (384 KiB): RAM
I (315) heap_init: At 50108080 len 00007F80 (31 KiB): RTCRAM
I (320) heap_init: At 30100044 len 00001FBC (7 KiB): TCM
I (326) spi_flash: detected chip: gd
I (328) spi_flash: flash io: dio
W (331) spi_flash: Detected size(16384k) larger than the size in the binary image header(2048k). Using the size in the binary image header.
I (343) host_init: ESP Hosted : Host chip_ip[18]
I (375) H_API: ESP-Hosted starting. Hosted_Tasks: prio:23, stack: 5120 RPC_task_stack: 5120
I (375) H_API: ** add_esp_wifi_remote_channels **
I (376) transport: Add ESP-Hosted channel IF[1]: S[0] Tx[0x4000c480] Rx[0x40020c3c]
--- 0x4000c480: transport_drv_sta_tx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_bt_controller_mac_addr/components/esp_hosted/host/drivers/transport/transport_drv.c:245
--- 0x40020c3c: esp_wifi_remote_channel_rx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_bt_controller_mac_addr/managed_components/espressif__esp_wifi_remote/esp_wifi_remote_net.c:19
I (384) transport: Add ESP-Hosted channel IF[2]: S[0] Tx[0x4000c3c0] Rx[0x40020c3c]
--- 0x4000c3c0: transport_drv_ap_tx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_bt_controller_mac_addr/components/esp_hosted/host/drivers/transport/transport_drv.c:275
--- 0x40020c3c: esp_wifi_remote_channel_rx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_bt_controller_mac_addr/managed_components/espressif__esp_wifi_remote/esp_wifi_remote_net.c:19
sdio_mempool_create free:196416 min-free:196416 lfb-def:155648 lfb-8bit:155648

I (400) H_SDIO_DRV: sdio_data_to_rx_buf_task started
I (403) main_task: Started on CPU0
I (406) main_task: Calling app_main()
I (416) H_API: ESP-Hosted Try to communicate with ESP-Hosted slave

I (416) transport: Attempt connection with slave: retry[0]
W (421) H_SDIO_DRV: Reset slave using GPIO[54]
I (425) os_wrapper_esp: GPIO [54] configured
I (1631) sdio_wrapper: SDIO master: Slot 1, Data-Lines: 4-bit Freq(KHz)[40000 KHz]
I (1631) sdio_wrapper: GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]
I (1635) sdio_wrapper: Queues: Tx[20] Rx[20] SDIO-Rx-Mode[1]
Name:
Type: SDIO
Speed: 40.00 MHz (limit: 40.00 MHz)
Size: 0MB
CSD: ver=1, sector_size=0, capacity=0 read_bl_len=0
SCR: sd_spec=0, bus_width=0
TUPLE: DEVICE, size: 3: D9 01 FF
TUPLE: MANFID, size: 4
  MANF: 0092, CARD: 6666
TUPLE: FUNCID, size: 2: 0C 00
TUPLE: FUNCE, size: 4: 00 00 02 32
TUPLE: CONFIG, size: 5: 01 01 00 02 07
TUPLE: CFTABLE_ENTRY, size: 8
  INDX: C1, Intface: 1, Default: 1, Conf-Entry-Num: 1
  IF: 41
  FS: 30, misc: 0, mem_space: 1, irq: 1, io_space: 0, timing: 0, power: 0
  IR: 30, mask: 1,   IRQ: FF FF
  LEN: FFFF
TUPLE: END
I (1714) sdio_wrapper: Function 0 Blocksize: 512
I (1718) sdio_wrapper: Function 1 Blocksize: 512
I (1723) H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
I (1728) transport: set_transport_state: 1
I (1732) transport: Waiting for esp_hosted slave to be ready
I (1800) H_SDIO_DRV: SDIO Host operating in STREAMING MODE
I (1800) H_SDIO_DRV: Open data path at slave
I (1800) H_SDIO_DRV: Starting SDIO process rx task
I (1828) H_SDIO_DRV: Received ESP_PRIV_IF type message
I (1828) transport: Received INIT event from ESP32 peripheral
I (1828) transport: EVENT: 12
I (1830) transport: Identified slave [esp32c6]
I (1834) transport: EVENT: 11
I (1836) transport: capabilities: 0xd
I (1840) transport: Features supported are:
I (1844) transport:      * WLAN
I (1846) transport:        - HCI over SDIO
I (1850) transport:        - BLE only
I (1853) transport: EVENT: 13
I (1856) transport: ESP board type is : 13

I (1860) transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
I (1866) H_API: Transport active
I (1869) transport: Slave chip Id[12]
I (1872) transport: raw_tp_dir[-], flow_ctrl: low[60] high[80]
I (1878) transport: transport_delayed_init
I (1881) esp_cli: Remove any existing deep_sleep cmd in cli
I (1887) esp_cli: Registering command: crash
I (1891) esp_cli: Registering command: reboot
I (1895) esp_cli: Registering command: mem-dump
I (1899) esp_cli: Registering command: task-dump
I (1903) esp_cli: Registering command: cpu-dump
I (1908) esp_cli: Registering command: heap-trace
I (1912) esp_cli: Registering command: sock-dump
I (1916) esp_cli: Registering command: host-power-save
I (1921) vhci_drv: Host BT Support: Enabled
I (1925) vhci_drv:      BT Transport Type: VHCI
I (1929) H_SDIO_DRV: Received INIT event
I (1933) H_SDIO_DRV: Event type: 0x22
I (1936) H_SDIO_DRV: Write thread started
I (1940) BT Example: getting fw version
I (2403) BT Example: FW Version: 2.4.4
I (2410) BT Example: Current BT controller mac address is 9c:9e:6e:5a:b1:66
I (2410) BT Example: Updating MAC address
I (2472) BT Example: New BT controller mac address is 08:3a:8d:01:01:01
I (2582) transport: Attempt connection with slave: retry[0]
I (2582) transport: Transport is already up
I (2607) main_task: Returned from app_main()
I (2608) BT Example: Advertising data raw set, status 0
I (2609) BT Example: Scan response data raw set, status 0
I (2616) BT Example: Advertising start successfully
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-hosted-mcu/issues) on ESP-Hosted on GitHub. We will get back to you soon.

## References

* Bluetooth Implementation in ESP-Hosted: https://github.com/espressif/esp-hosted-mcu/blob/main/docs/bluetooth_design.md
