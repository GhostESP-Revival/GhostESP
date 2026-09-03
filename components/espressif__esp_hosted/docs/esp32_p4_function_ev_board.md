# ESP-Hosted on the ESP32-P4-Function-EV-Board DevKit

<details>
<summary>Table of Contents</summary>

- [1. Introduction](#1-introduction)
- [2. Set-Up ESP-IDF](#2-set-up-esp-idf)
- [3. Building Host for the P4](#3-building-host-for-the-p4)
  - [Adding Components](#31-adding-components)
  - [Selecting the Co-processor](#32-selecting-the-co-processor)
  - [Configuring Wi-Fi Defaults](#33-configuring-wi-fi-defaults)
  - [Building Firmware](#34-building-firmware)
- [4. Checking ESP-Hosted](#4-checking-esp-hosted)
- [5. Flashing ESP32-C6](#5-flashing-esp32-c6)
  - [Using ESP-Prog](#51-serial-flashing-using-esp-prog-initial-setup)
  - [OTA Updates](#52-esp-hosted-slave-ota-updates-recommended)
- [6. Troubleshooting](#6-troubleshooting)
- [7. Flashing the On-board ESP32-P4 through the ESP-Prog](#7-flashing-esp32-p4)
- [8. Testing ESP-Hosted with SPI-FD with other MCUs](#8-testing-esp-hosted-with-spi-fd-with-other-mcus)
- [9. References](#10-references)
</details>

## 1. Introduction

This guide covers using ESP-Hosted-MCU on the ESP32-P4-Function-EV-Board. The board includes an on-board ESP32-C6 module that comes pre-flashed with ESP-Hosted-MCU slave firmware (v0.0.6). This provides Wi-Fi/Bluetooth connectivity to the on-board ESP32-P4, which acts as the host.

The image below shows the board.

<img src="images/esp32-p4-function-ev-board.jpg" alt="ESP32-P4-Function-EV-Board" width="800" />

*ESP32-P4-Function-EV-Board*

The ESP32-P4 communicates with the ESP32-C6 module using SDIO.

## 2. Set-Up ESP-IDF

If you haven't already set up ESP-IDF, choose one of the following options:

#### Option 1: Installer Way (Recommended)

- **Windows**
  - Follow the [Standard Setup of Toolchain for Windows](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html)
  - Use the ESP-IDF [Powershell Command Prompt](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html#using-the-command-prompt) for all commands

- **Linux or MacOS**
  - For bash:
    ```bash
    bash docs/setup_esp_idf__latest_stable__linux_macos.sh
    ```
  - For fish:
    ```fish
    fish docs/setup_esp_idf__latest_stable__linux_macos.fish
    ```

#### Option 2: Manual Way

Follow the [ESP-IDF Get Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) for manual installation.

## 3. Building Host for the P4

### 3.1. Adding Components

Add the required components to your project:

```bash
idf.py add-dependency "espressif/esp_wifi_remote"
idf.py add-dependency "espressif/esp_hosted"
```

**Important:** Remove `esp-extconn` if present, as it conflicts with `esp-hosted`.

Open `main/idf_component.yml` and remove or comment out this block if it exists:

```yaml
# ------- Delete or comment this block ---------
espressif/esp-extconn:
  version: "~0.1.0"
  rules:
    - if: "target in [esp32p4]"
# -----------------------------------
```

### 3.2. Selecting the Co-processor

Run:

```bash
idf.py menuconfig
```

Select co-processors with native Wi-Fi (like ESP32-C6) using `Option (1)` below. For other co-processors (like ESP32-H2), use `Option (2)`.

```
Component config
├── Wi-Fi Remote
│   └── Choose slave target                                      ⎫
│       ├── ( ) esp32                                            │
│       ├── ( ) esp32s2                                          │     Choose co-processor from:
│       ├── ( ) esp32c3                                          │
│       ├── ( ) esp32s3                                          ⎬       Option (1):
│       ├── ( ) esp32c2                                          │       "Wi-Fi Remote" config
│       ├── (X) esp32c6                                          │
│       ├── ( ) esp32c5                                          │
│       └── ( ) esp32c61                                         ⎭
│
└── ESP-Hosted config                                            ⎫
    └── Choose the Co-processor to use                           │
        ├── ( ) ESP32-C6 (fetched from Wi-Fi Remote component)   ⎬       Option (2):
        ├── (X) ESP32-H2                                         │       "ESP-Hosted" config
        └── ( ) ESP32-H4                                         ⎭
```

> [!NOTE]
> ESP32-H4 currently does not support Bluetooth or SPI transport. This will be enabled in the future.

### 3.3. Configuring Wi-Fi Defaults

For the ESP32-C6 as the co-processor, edit `sdkconfig.defaults.esp32p4` to include the following configuration:

```
### sdkconfig for ESP32-P4 + C6 Dev board
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y

CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=16
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=64
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=64
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=32

CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
CONFIG_LWIP_TCP_WND_DEFAULT=65534
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64

CONFIG_LWIP_TCP_SACK_OUT=y
```

For optimized parameters when using other co-processors, see the [Performance Optimization Guide](performance_optimization.md).

### 3.4. Building Firmware

Build and flash the firmware:

```sh
idf.py set-target esp32p4
idf.py build
idf.py -p <P4 Serial Port> flash monitor
```

Replace `<P4 Serial Port>` with your actual serial port (e.g., `COM3` on Windows or `/dev/ttyUSB0` on Linux).

## 4. Checking ESP-Hosted

After flashing, you should see output similar to this on the console:

```
I (498) H_API: esp_wifi_remote_init
I (498) transport: Attempt connection with slave: retry[0]
I (498) transport: Reset slave using GPIO[54]
I (498) os_wrapper_esp: GPIO [54] configured
I (508) gpio: GPIO[54]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (1678) sdio_wrapper: SDIO master: Data-Lines: 4-bit Freq(KHz)[40000 KHz]
I (1678) sdio_wrapper: GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]
I (1678) H_SDIO_DRV: Starting SDIO process rx task
I (1678) sdio_wrapper: Queues: Tx[20] Rx[20] SDIO-Rx-Mode[3]
I (1718) gpio: GPIO[15]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (1718) gpio: GPIO[17]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
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
I (1768) sdio_wrapper: Function 0 Blocksize: 512
I (1778) sdio_wrapper: Function 1 Blocksize: 512
I (1778) H_SDIO_DRV: SDIO Host operating in PACKET MODE
I (1788) H_SDIO_DRV: generate slave intr
I (1798) transport: Received INIT event from ESP32 peripheral
I (1798) transport: EVENT: 12
I (1798) transport: EVENT: 11
I (1808) transport: capabilities: 0xd
I (1808) transport: Features supported are:
I (1818) transport:      * WLAN
I (1818) transport:        - HCI over SDIO
I (1818) transport:        - BLE only
I (1828) transport: EVENT: 13
I (1828) transport: ESP board type is : 13

I (1838) transport: Base transport is set-up

I (1838) transport: Slave chip Id[12]
I (1848) hci_stub_drv: Host BT Support: Disabled
I (1848) H_SDIO_DRV: Received INIT event
I (1868) rpc_wrap: Received Slave ESP Init
```

This confirms that ESP-Hosted is running correctly.

## 5. Flashing ESP32-C6

Note: The ESP32-C6 comes pre-flashed with ESP-Hosted slave firmware v0.0.6, so this step is optional unless you need to update the firmware. However, it is **recommended** to upgrade to the latest slave firmware to get updated features and performance optimizations.

### 5.1 Serial Flashing Using ESP-Prog (Initial Setup)

> [!TIP]
> For firmware updates after initial setup, use the OTA method described in section 5.2

You'll need an ESP-Prog or similar UART adapter for serial flashing.

<img src="images/esp32-p4-function-ev-board-esp-prog.jpg" alt="ESP32-P4-Function-EV-Board with ESP-Prog Connected to ESP32-C6" width="800" />

*ESP32-P4-Function-EV-Board with ESP-Prog Connected to ESP32-C6*

**Steps:**

1. Get the ESP-Hosted slave example:

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
```

2. Configure the project:

```sh
idf.py set-target esp32c6
idf.py menuconfig
```

3. Verify SDIO is enabled (it should be by default):
   ```
   Example Configuration
   └── Bus Config in between Host and Co-processor
       └── Transport layer
           └── Select "SDIO"
   ```

4. Build the firmware:

```sh
idf.py build
```

5. Connect ESP-Prog to the `PROG_C6` header:

| ESP-Prog | PROG_C6 | Notes                   |
| ---      | ---     | ---                     |
| ESP\_EN  | EN      |                         |
| ESP\_TXD | TXD     |                         |
| ESP\_RXD | RXD     |                         |
| VDD      | -       | **Do not connect**      |
| GND      | GND     |                         |
| ESP\_IO0 | IO0     |                         |

6. Put the ESP32-P4 into bootloader mode to prevent interference:

   **Manual method:**
   - Hold down the `BOOT` button
   - Press and release the `RST` button
   - Release the `BOOT` button

   **Script method:**
   ```sh
   esptool.py -p <host_serial_port> --before default_reset --after no_reset run
   ```

7. Flash the C6:

```sh
idf.py -p <Serial Port> flash monitor
```

### 5.2 ESP-Hosted Slave OTA Updates (Recommended)

The ESP-Hosted link comes pre-configured and ready to use on first boot. You can update the slave firmware remotely from the host MCU using OTA (Over-The-Air) updates: **No** ESP-Prog, serial cable, or extra GPIO connections are required.

For step-by-step instructions, see the [Host Performs Slave OTA Example](../examples/host_performs_slave_ota/README.md).

## 6. Troubleshooting

If you encounter any issues, refer to the [Troubleshooting Guide](troubleshooting.md).

<details>
<summary>7. Flashing the On-board ESP32-P4 through the ESP-Prog</summary>

The standard way to flash the P4 is through the **USB connector with Type-C cable** inserted into USB-UART port on ESP32-P4. However, you can also use an ESP-Prog connected to the serial interface.

<img src="images/esp32-p4-esp-prog.jpg" alt="ESP32-P4 Serial Connection with ESP-Prog" width="600" />

*ESP32-P4 Serial Connection with ESP-Prog*

**Connections:**

| ESP-Prog | P4 Header       |
| ---      | ---             |
| ESP\_TXD | U0TXD (GPIO 37) |
| ESP\_RXD | U0RXD (GPIO 38) |
| GND      | GND             |

Leave other ESP-Prog pins disconnected.

**Flashing steps:**

1. Hold down the `BOOT` button
2. Press and release the `RST` button
3. Release the `BOOT` button

4. Flash the firmware:

```sh
idf.py -p <Serial Port> flash monitor
```

5. Press the `RST` button to restart the P4 after flashing.

</details>

## 8. Testing ESP-Hosted with SPI-FD with other MCUs

You can test ESP-Hosted using SPI Full Duplex (SPI-FD) by connecting another ESP-Dev-Kit or ESP Chipset to the P4 through the J1 GPIO header.

**Important:** Use GPIO 36 or lower to avoid LDO power issues with higher-numbered GPIOs.

**Recommended GPIO configuration:**

| Function   | GPIO |
|------------|------|
| MOSI       | 4    |
| MISO       | 5    |
| CLK        | 26   |
| CS         | 6    |
| Handshake  | 20   |
| Data Ready | 32   |
| Reset      | 2    |

> [!NOTE]
> Avoid using GPIO 35 and 36, as they affect bootloader mode. See [ESP32-P4 Boot Mode Selection](https://docs.espressif.com/projects/esptool/en/latest/esp32p4/advanced-topics/boot-mode-selection.html#select-bootloader-mode) for details.

> [!TIP]
> For optimal performance testing, check out the [Shield Box Test Setup](shield-box-test-setup.md).

## 9. References

- [ESP32-P4-Function-EV-Board Documentation](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/)
- [ESP-Prog Guide](https://docs.espressif.com/projects/esp-iot-solution/en/latest/hw-reference/ESP-Prog_guide.html)
- [`esp_wifi_remote` component](https://components.espressif.com/components/espressif/esp_wifi_remote/)
- [`esp_hosted` component](https://components.espressif.com/components/espressif/esp_hosted/)
