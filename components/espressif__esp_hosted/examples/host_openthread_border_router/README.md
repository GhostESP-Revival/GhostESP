| Supported Hosts | ESP32-P4 |
| --------------- | -------- |

| Supported Co-processors | ESP32-C5 | ESP32-C6 |
|-------------------------|----------|----------|

# OpenThread Border Router Example

## Overview

This example demonstrates an [OpenThread border router](https://openthread.io/guides/border-router) running on a Host Processor and communicating with a OpenThread RCP (Radio Co-processor) and Wi-Fi running on a ESP-Hosted co-processor.

> [!NOTE]
> Running OpenThread and Wi-Fi together on the same ESP32-C6 co-processor may lead to Wi-Fi and OpenThread performance issues as they have to share the same hardware radio. See the [ESP32-C6 RF Coexistence matrix](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html) for more information.
>
> For a multi co-processor configuration (ESP32-P4 + C6 Co-processor (Wi-Fi) + H2 (OpenThread RCP) see this [ESP32-P4 Thread Border Router Example](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md).

## Example 1: OpenThread Host communicating with Openthread RCP over UART

The example runs on an ESP32-P4, connected to a ESP32-C6 DevKit as the co-processor via the GPIO Header. ESP-Hosted transport is SPI-FD (Full duplex) while UART is used for OpenThread communications.

### Setting up the hardware

This table shows the Hardware Connections between the ESP32-P4 and C6.

**SPI-FD Connection Between ESP32-P4 and ESP32-C6 co-processor**

|            | ESP32-P4 GPIO | ESP32-C6 GPIO |
|------------|--------------:|--------------:|
| MOSI       |             4 |             7 |
| MISO       |             5 |             2 |
| CLK        |            26 |             6 |
| CS         |             6 |            10 |
| Handshake  |            20 |             3 |
| Data Ready |            32 |             4 |
| C6 Reset   |             2 |           RST |

**UART Connection Between ESP32-P4 and ESP32-C6 co-processor**

|  ESP32-P4 GPIO | ESP32-C6 GPIO |
|---------------:|--------------:|
| (To RCP Tx) 24 |       (Tx) 21 |
| (To RCP Rx) 25 |       (Rx) 20 |

### Configure the project for ESP32-P4

On the command-line:

```bash
idf.py set-target esp32p4
idf.py menuconfig
```

Configure the project:

```
Component config
└── ESP-Hosted config
    ├── Configure GPIOs for Development Board ──> No development board
    ├── Transport layer ──> SPI Full-duplex
    ├── SPI Configuration ──> (Configure GPIOs)
    │   ⋮
    └── [*] Enable OpenThread Host Support
        └── OpenThread RCP Configuration
            ├── OpenThread Transport ──> UART
            └── (configure UART parameters)
```

### Configure the project for ESP32-C6

In the ESP-Hosted project `slave` directory:

Edit `sdkconfig.defaults.esp32c6` to enable the OpenThread section. These settings are required to enable OpenThread, configure it to be an RCP, and to disable OpenThread features not required for a RCP.

```text
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_RADIO=y
CONFIG_OPENTHREAD_DIAG=n
CONFIG_OPENTHREAD_COMMISSIONER=n
CONFIG_OPENTHREAD_JOINER=n
CONFIG_OPENTHREAD_BORDER_ROUTER=n
CONFIG_OPENTHREAD_CLI=n
CONFIG_OPENTHREAD_SRP_CLIENT=n
CONFIG_OPENTHREAD_DNS_CLIENT=n
CONFIG_OPENTHREAD_TASK_SIZE=3072
CONFIG_OPENTHREAD_CONSOLE_ENABLE=n
CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC=n

CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

On the command-line:

```bash
idf.py set-target esp32c6
idf.py menuconfig
```

Configure the project:

```
Example Configuration
├── Bus Config in between Host and Co-processor
│   ├── Transport layer ──> SPI Full-duplex
│   └── SPI Full-duplex Configuration ──> (Configure GPIOs)
│   ⋮
└── [*] Enable OpenThread RCP (Radio Co-Processor)
    └── OpenThread RCP Configuration
        ├── OpenThread Transport ──> UART
        └── (configure UART parameters)
```

If you did not edit `sdkconfig.defaults.esp32c6` to enable OpenThread (above), modify these settings:

```
Component config
├── Wireless Coexistence
│   └── [*] Software controls WiFi/Bluetooth coexistence
└── OpenThread
    ├── [*] OpenThread
    ├── Thread Task Parameters
    │   └── (3072) Size of OpenThread task
    ├── Thread Console
    │   ├── [ ] Enable OpenThread console
    │   └── [ ] Enable Openthread Command-Line Interface
    ├── Thread Core Features
    │   ├── Thread device type ──> Radio Only Device
    │   ├── [ ] Enable Commissioner
    │   ├── [ ] Enable Joiner
    │   ├── [ ] Enable SRP Client
    │   ├── [ ] Enable DNS Client
    │   ├── [ ] Enable diag Client
    └── Thread Log
        └── [ ] Enable dynamic log level control
```

### Build, Flash, and Run

Build the projects and flash them to the appropriate boards, then run monitor tool to view serial output:

```bash
idf.py -p PORT build flash monitor
```

On the ESP32-P4, manually configure the networks with CLI commands.

`wifi` command can be used to configure the Wi-Fi network.

```text
esp32p4> ot wifi
--wifi parameter---
connect
-s                   :     wifi ssid
-p                   :     wifi psk
---example---
join a wifi:
ssid: threadcertAP
psk: threadcertAP    :     wifi connect -s threadcertAP -p threadcertAP
state                :     get wifi state, disconnect or connect
---example---
get wifi state       :     wifi state
Done
```

To join a Wi-Fi network, use the `ot wifi connect` command:

```text
esp32p4> ot wifi connect -s threadcertAP -p threadcertAP
ssid: threadcertAP
psk: threadcertAP
I (11331) wifi:wifi driver task: 3ffd06e4, prio:23, stack:6656, core=0
I (11331) system_api: Base MAC address is not set
I (11331) system_api: read default base MAC address from EFUSE
I (11341) wifi:wifi firmware version: 45c46a4
I (11341) wifi:wifi certification version: v7.0


..........

I (13741) esp_netif_handlers: sta ip: 192.168.3.10, mask: 255.255.255.0, gw: 192.168.3.1
W (13771) wifi:<ba-add>idx:0 (ifx:0, 02:0f:c1:32:3b:2b), tid:0, ssn:2, winSize:64
wifi sta is connected successfully
Done
```

To get the state of the Wi-Fi network:

```text
esp32p4> ot wifi state
connected
Done
```

For forming the Thread network, please refer to the [ot\_cli README](../host_openthread_cli/README.md).

## Example Output

```text
I (2729) esp_netif_handlers: example_connect: sta ip: 192.168.1.100, mask: 255.255.255.0, gw: 192.168.1.1
I (2729) example_connect: Got IPv4 event: Interface "example_connect: sta" address: 192.168.1.100
I (3729) example_connect: Got IPv6 event: Interface "example_connect: sta" address: fe80:0000:0000:0000:266f:28ff:fe80:2920, type: ESP_IP6_ADDR_IS_LINK_LOCAL
I (3729) example_connect: Connected to example_connect: sta
I (3739) example_connect: - IPv4 address: 192.168.1.100
I (3739) example_connect: - IPv6 address: fe80:0000:0000:0000:266f:28ff:fe80:2920, type: ESP_IP6_ADDR_IS_LINK_LOCAL

......


I(8139) OPENTHREAD:[INFO]-MLE-----: AttachState ParentReqReeds -> Idle
I(8139) OPENTHREAD:[NOTE]-MLE-----: Allocate router id 50
I(8139) OPENTHREAD:[NOTE]-MLE-----: RLOC16 fffe -> c800
I(8159) OPENTHREAD:[NOTE]-MLE-----: Role Detached -> Leader
```

## More Information

- [ESP-IDF Thread Border Router example](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_br)
- [ESP Thread Border Router SDK](https://github.com/espressif/esp-thread-br)
- [ESP Thread Border Router SDK example for ESP32-P4](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md)
