| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 | ESP32-C Series | ESP32-S Series |
| ----------------------- | ----- | -------------- | -------------- |

# BLE Peripheral Example

This example uses the UART transport written by application itself. Refer the file [main/uart_driver.c](main/uart_driver.c).

To write the custom transport in the application, the controller should be disabled and the default uart-transport should be disabled (when the controller is disabled, by default the uart-transport is selected). In order to compile the custom transport in the application, the default uart-transport should be disabled. Refer to the sdkconfig.defaults.

As an example, the [BLE Peripheral Example Walkthrough](https://github.com/espressif/esp-hosted-mcu/blob/main/examples/host_nimble_bleprph_host_only_uart_hci/tutorial/bleprph_host_only_walkthrough.md) shows an ESP32-P4 connected to a ESP32 over UART. See the Walkthrough for information on hardware setup.

For more information about the application, refer to the bleprph [README file](https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/bleprph/README.md).
To test this demo, any BLE scanner app can be used.

## How to Use Example

### Project Configuration for Host
Before project configuration and build, be sure to set the correct chip target on both the host and co-processor using:

```bash
idf.py set-target <chip_name>
```

### Project Configuration for Host

Open the project configuration menu:

```bash
idf.py menuconfig
```

In the `Component config-> Bluetooth` menu:

* Select `controller` to Disabled.
* Disable `Nimble Options-> Host-controller Transport -> Enable Uart Transport`.

>[!Important]
> Co-processor selection is done by wifi-remote. Ensure the correct co-processor chip is selected in `Component config` -> `Wi-Fi Remote` -> `choose slave target`. The target selected will affect the ESP-Hosted transport options and default GPIOs used.

### Setup and Configuration for Co-processor

On the co-processor, UART HCI setup is done through the Bluetooth Component kconfig settings. In menuconfig, select `Component config` -> `Bluetooth` -> `Controller Options` -> `HCI mode` or `HCI Config` and set it to `UART(H4)`.

Depending on the selected co-processor, you can configure various UART parameters (Tx, Rx pins, hardware flow control, RTS, CTS pins, baudrate) through the Bluetooth Component. Other UART parameters not handled by the Bluetooth Component are configured by ESP-Hosted through `Example Configuration` -> `HCI UART Settings`.

> [!NOTE]
> Make sure the UART GPIO pins selected do not conflict with the GPIO
> pins used for the selected ESP-Hosted transport.

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project on both the host and co-processor.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://idf.espressif.com/) for full steps to configure and use ESP-IDF to build projects.

## Example Output

Console output when `host_nimble_bleprph_host_only_uart_hci` is running on ESP32-P4 and using ESP32 as the BT controller:

```
I (25) boot: ESP-IDF v5.5-dev-1868-g39f34a65a9-dirty 2nd stage bootloader
I (26) boot: compile time Feb 13 2025 17:15:22
I (26) boot: Multicore bootloader
I (29) boot: chip revision: v1.0
I (31) boot: efuse block revision: v0.1
I (34) boot.esp32p4: SPI Speed      : 80MHz
I (38) boot.esp32p4: SPI Mode       : DIO
I (42) boot.esp32p4: SPI Flash Size : 2MB
I (46) boot: Enabling RNG early entropy source...
I (50) boot: Partition Table:
I (53) boot: ## Label            Usage          Type ST Offset   Length
I (59) boot:  0 nvs              WiFi data        01 02 00009000 00006000
I (66) boot:  1 phy_init         RF data          01 01 0000f000 00001000
I (72) boot:  2 factory          factory app      00 00 00010000 00100000
I (80) boot: End of partition table
I (82) esp_image: segment 0: paddr=00010020 vaddr=40070020 size=252a0h (152224) map
I (116) esp_image: segment 1: paddr=000352c8 vaddr=30100000 size=00044h (    68) load
I (118) esp_image: segment 2: paddr=00035314 vaddr=4ff00000 size=0ad04h ( 44292) load
I (130) esp_image: segment 3: paddr=00040020 vaddr=40000020 size=68930h (428336) map
I (202) esp_image: segment 4: paddr=000a8958 vaddr=4ff0ad04 size=069e0h ( 27104) load
I (209) esp_image: segment 5: paddr=000af340 vaddr=4ff11700 size=026d8h (  9944) load
I (217) boot: Loaded app from partition at offset 0x10000
I (217) boot: Disabling RNG early entropy source...
I (228) cpu_start: Multicore app
I (238) cpu_start: Pro cpu start user code
I (239) cpu_start: cpu freq: 360000000 Hz
I (239) app_init: Application information:
I (239) app_init: Project name:     bleprph_host_only
I (243) app_init: App version:      6eaa9b1
I (247) app_init: Compile time:     Feb 13 2025 17:15:16
I (252) app_init: ELF file SHA256:  8861453bb...
I (257) app_init: ESP-IDF:          v5.5-dev-1868-g39f34a65a9-dirty
I (263) efuse_init: Min chip rev:     v0.1
I (266) efuse_init: Max chip rev:     v1.99
I (270) efuse_init: Chip rev:         v1.0
I (274) heap_init: Initializing. RAM available for dynamic allocation:
I (281) heap_init: At 4FF17130 len 00023E90 (143 KiB): RAM
I (286) heap_init: At 4FF3AFC0 len 00004BF0 (18 KiB): RAM
I (291) heap_init: At 4FF40000 len 00060000 (384 KiB): RAM
I (296) heap_init: At 50108080 len 00007F80 (31 KiB): RTCRAM
I (301) heap_init: At 30100044 len 00001FBC (7 KiB): TCM
I (307) spi_flash: detected chip: generic
I (310) spi_flash: flash io: dio
W (313) spi_flash: Detected size(16384k) larger than the size in the binary image header(2048k). Using the size in the binary image header.
I (326) host_init: ESP Hosted : Host chip_ip[18]
I (357) H_API: ESP-Hosted starting. Hosted_Tasks: prio:23, stack: 5120 RPC_task_stack: 5120
sdio_mempool_create free:177280 min-free:177280 lfb-def:131072 lfb-8bit:131072

I (361) gpio: GPIO[18]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (369) gpio: GPIO[19]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (378) gpio: GPIO[14]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (386) gpio: GPIO[15]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (394) gpio: GPIO[16]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (403) gpio: GPIO[17]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (411) H_API: ** add_esp_wifi_remote_channels **
I (415) transport: Add ESP-Hosted channel IF[1]: S[0] Tx[0x4000f988] Rx[0x4001ddee]
--- 0x4000f988: transport_drv_sta_tx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_nimble_bleprph_host_only_uart_hci/components/esp_hosted/host/drivers/transport/transport_drv.c:208
0x4001ddee: esp_wifi_remote_channel_rx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_nimble_bleprph_host_only_uart_hci/managed_components/espressif__esp_wifi_remote/esp_wifi_remote_net.c:19

I (423) transport: Add ESP-Hosted channel IF[2]: S[0] Tx[0x4000f8d0] Rx[0x4001ddee]
--- 0x4000f8d0: transport_drv_ap_tx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_nimble_bleprph_host_only_uart_hci/components/esp_hosted/host/drivers/transport/transport_drv.c:238
0x4001ddee: esp_wifi_remote_channel_rx at /home/kysoh/projects/gitlab_esp_hosted_mcu/examples/host_nimble_bleprph_host_only_uart_hci/managed_components/espressif__esp_wifi_remote/esp_wifi_remote_net.c:19

I (431) main_task: Started on CPU0
I (441) main_task: Calling app_main()
I (451) NimBLE_BLE_PRPH: BLE Host Task Started
I (451) uart: queue free spaces: 8
I (471) NimBLE: GAP procedure initiated: stop advertising.

�I (501) main_task: Returned from app_main()
I (501) NimBLE: ogf=0x08, ocf=0x004e, hci_err=0x201 : BLE_ERR_UNKNOWN_HCI_CMD (Unknown HCI Command)

I (501) NimBLE: Device Address:
I (511) NimBLE: 10:97:bd:d5:8a:62
I (511) NimBLE:

I (521) NimBLE: GAP procedure initiated: advertise;
I (521) NimBLE: disc_mode=2
I (521) NimBLE:  adv_channel_map=0 own_addr_type=0 adv_filter_policy=0 adv_itvl_min=0 adv_itvl_max=0
I (531) NimBLE:
```

Console output on ESP32 as the BT controller:

```
I (29) boot: ESP-IDF v5.5-dev-1868-g39f34a65a9-dirty 2nd stage bootloader
I (29) boot: compile time Feb 13 2025 17:15:30
I (29) boot: Multicore bootloader
I (33) boot: chip revision: v3.0
I (36) boot.esp32: SPI Speed      : 40MHz
I (39) boot.esp32: SPI Mode       : DIO
I (43) boot.esp32: SPI Flash Size : 4MB
I (46) boot: Enabling RNG early entropy source...
I (51) boot: Partition Table:
I (53) boot: ## Label            Usage          Type ST Offset   Length
I (60) boot:  0 nvs              WiFi data        01 02 00009000 00004000
I (66) boot:  1 otadata          OTA data         01 00 0000d000 00002000
I (73) boot:  2 phy_init         RF data          01 01 0000f000 00001000
I (79) boot:  3 ota_0            OTA app          00 10 00010000 001c0000
I (86) boot:  4 ota_1            OTA app          00 11 001d0000 001c0000
I (92) boot: End of partition table
I (96) boot: No factory image, trying OTA 0
I (100) esp_image: segment 0: paddr=00010020 vaddr=3f400020 size=285e0h (165344) map
I (164) esp_image: segment 1: paddr=00038608 vaddr=3ff80000 size=0001ch (    28) load
I (164) esp_image: segment 2: paddr=0003862c vaddr=3ffbdb60 size=05da4h ( 23972) load
I (176) esp_image: segment 3: paddr=0003e3d8 vaddr=40080000 size=01c40h (  7232) load
I (180) esp_image: segment 4: paddr=00040020 vaddr=400d0020 size=9c69ch (640668) map
I (401) esp_image: segment 5: paddr=000dc6c4 vaddr=40081c40 size=1dc1ch (121884) load
I (465) boot: Loaded app from partition at offset 0x10000
I (503) boot: Set actual ota_seq=1 in otadata[0]
I (503) boot: Disabling RNG early entropy source...
I (513) cpu_start: Multicore app
I (521) cpu_start: Pro cpu start user code
I (521) cpu_start: cpu freq: 240000000 Hz
I (521) app_init: Application information:
I (521) app_init: Project name:     network_adapter
I (526) app_init: App version:      6eaa9b1
I (530) app_init: Compile time:     Feb 13 2025 17:15:24
I (535) app_init: ELF file SHA256:  b3b6ed47b...
I (539) app_init: ESP-IDF:          v5.5-dev-1868-g39f34a65a9-dirty
I (545) efuse_init: Min chip rev:     v0.0
I (549) efuse_init: Max chip rev:     v3.99
I (553) efuse_init: Chip rev:         v3.0
I (557) heap_init: Initializing. RAM available for dynamic allocation:
I (563) heap_init: At 3FFAFF10 len 000000F0 (0 KiB): DRAM
I (568) heap_init: At 3FFB6388 len 00001C78 (7 KiB): DRAM
I (573) heap_init: At 3FFB9A20 len 00004108 (16 KiB): DRAM
I (578) heap_init: At 3FFC8F18 len 000170E8 (92 KiB): DRAM
I (584) heap_init: At 3FFE0440 len 00003AE0 (14 KiB): D/IRAM
I (589) heap_init: At 3FFE4350 len 0001BCB0 (111 KiB): D/IRAM
I (594) heap_init: At 4009F85C len 000007A4 (1 KiB): IRAM
I (601) spi_flash: detected chip: generic
I (603) spi_flash: flash io: dio
W (606) spi_flash: Detected size(8192k) larger than the size in the binary image header(4096k). Using the size in the binary image header.
I (620) coexist: coex firmware version: e727207
I (624) main_task: Started on CPU0
I (626) main_task: Calling app_main()
I (628) fg_mcu_slave: *********************************************************************
I (634) fg_mcu_slave:                 ESP-Hosted-MCU Slave FW version :: 1.1.3

I (642) fg_mcu_slave:                 Transport used :: SPI + UART
I (648) fg_mcu_slave: *********************************************************************
I (654) fg_mcu_slave: Supported features are:
I (656) fg_mcu_slave: - WLAN over SPI
I (660) h_bt: - BT/BLE
I (662) h_bt:    - HCI Over UART
I (664) h_bt:    - BT/BLE dual mode
I (666) fg_mcu_slave: capabilities: 0xba
I (670) fg_mcu_slave: Supported extended features are:
I (674) h_bt: - BT/BLE (extended)
I (676) fg_mcu_slave: extended capabilities: 0x0
I (686) h_bt: ESP Bluetooth MAC addr: 10:97:bd:d5:8a:62
I (686) bt_uart: UART1 Pins: TX 5, RX 18, RTS 19, CTS 23 Baudrate:921600
I (688) BTDM_INIT: BT controller compile version [194dd63]
I (694) BTDM_INIT: Bluetooth MAC: 10:97:bd:d5:8a:62
I (698) phy_init: phy_version 4840,02e0d70,Sep  2 2024,19:39:07
I (1006) SPI_DRIVER: Using SPI interface
I (1008) gpio: GPIO[26]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (1008) gpio: GPIO[4]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (1014) SPI_DRIVER: SPI Ctrl:1 mode: 2, Freq:ConfigAtHost
GPIOs: MOSI: 13, MISO: 12, CS: 15, CLK: 14 HS: 26 DR: 4

I (1022) SPI_DRIVER: Hosted SPI queue size: Tx:10 Rx:10
I (1026) gpio: GPIO[15]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (1032) gpio: GPIO[15]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (1042) SPI_DRIVER: Slave chip Id[12]
I (1042) fg_mcu_slave: Initial set up done
I (1046) slave_ctrl: event ESPInit
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-hosted-mcu/issues) on GitHub. We will get back to you soon.
