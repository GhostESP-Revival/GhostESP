# OpenThread and Zigbee Support

**Table of Contents**

<details>

- [1. Introduction](#1-introduction)
  - [1.1 Prerequisites](#11-prerequisites)
	- [1.1.1 Basic OpenThread / Zigbee Operation](#111-basic-openthread--zigbee-operation)
	- [1.1.2 Operating as an OpenThread Border Router / Zigbee Gateway](#112-operating-as-an-openthread-border-router--zigbee-gateway)
- [2. Configuration](#2-configuration)
  - [2.1 RCP Configuration on the Co-processor](#21-rcp-configuration-on-the-co-processor)
  - [2.2 OpenThread Configuration on the Host](#22-openthread-configuration-on-the-host)
	- [2.2.1 Initialising the OpenThread Connection from the Host Application](#221-initialising-the-openthread-connection-from-the-host-application)
  - [2.3 Zigbee Configuration on the Host](#23-zigbee-configuration-on-the-host)
    - [2.3.1 Initialising the Zigbee Connection from the Host Application](#231-initialising-the-zigbee-connection-from-the-host-application)
- [3. More Information](#3-more-information)

</details>

## 1. Introduction

OpenThread is an IP stack which features mesh network and low power consumption. Zigbee is a low-power, wireless mesh networking protocol designed for IoT devices. Both operate on the 802.15.4 standard.

ESP-Hosted runs the OpenThread or Zigbee Host stack on the Host Processor and the RCP (Radio Co-Processor) on the co-processor. The RCP is common and can operate with either OpenThread or Zigbee Host stacks.

The diagram below shows how the OpenThread Host and RCP communicate.


```
    +------------+                                      +------------+
    |            |         ESP-Hosted Transport         |            |
    | OpenThread |<------------------------------------>|    RCP     | (Example: ESP32-C6)
    | / Zigbee   |                                      |            |
    | Host       |                                      |            |
    |            |                 UART                 |            |
    |            |<------------------------------------>|            |
    |            |                                      |            |
    +------------+                                      +------------+
```

> [!NOTE]
> Currently a dedicated UART channel is required to pass OpenThread data between the Host and RCP. OpenThread over the current ESP-Hosted Transport to be supported in the future.
>
> Zigbee only supports using a dedicated UART channel for communication.

### 1.1 Prerequisites

#### 1.1.1 Basic OpenThread / Zigbee Operation

The co-processor must support OpenThread / Zigbee RCP operation.

Example co-processors that support RCP include the ESP32-H2, ESP32-C5 and ESP32-C6.

#### 1.1.2 Operating as an OpenThread Border Router / Zigbee Gateway

The co-processor must support RCP operation and Wi-Fi to operate as part of an OpenThread Border Router or Zigbee Gateway, which passes packets between the OpenThread / Zigbee and Wi-Fi networks.

Example co-processors as RCPs that support OpenThread Border Router / Zigbee Gateway operation include the ESP32-C5 and ESP32-C6.

**Coexistence between OpenThread / Zigbee and Wi-Fi**

The co-processor has only one hardware radio, so Wi-Fi and OpenThread / Zigbee have to share the radio (coexistence). This may lead to performance issues or dropped packets if there is a lot of traffic on Wi-Fi and/or OpenThread. See this guide on [RF Coexistence](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html) for more information.

The recommended option is to use two co-processors: one for Wi-Fi and one for OpenThread / Zigbee:

```
    +------------+
    |            |         ESP-Hosted Transport         +-----------+
    | OpenThread |<------------------------------------>|   Wi-Fi   | (Example: ESP32-C6)
    | Border     |                                      +-----------+
    | Router     |
    | / Zigbee   |                 UART                 +-----------+
    | Gateway    |<------------------------------------>|    RCP    | (Example: ESP32-H2)
    |            |                                      +-----------+
    +------------+
```

Here, OpenThread / Zigbee is operating independent of ESP-Hosted. Since there are now two hardware radios, coexistence is not required.

More information on this mode of operation for the ESP32-P4 can be found in the [ESP-IDF OpenThread Border Router Example for ESP32-P4](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md) or the [Zigbee SDK Gateway Example](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/zigbee_gateway).

## 2. Configuration

### 2.1 RCP Configuration on the Co-processor

> [!NOTE]
> This section targets the ESP32-C6 as the RCP. The same configuration is required when using other supported co-processors as the RCP.

In the ESP-Hosted project `slave` directory:

Edit `sdkconfig.defaults.esp32c6` to add or enable the OpenThread section. These settings are required to enable OpenThread, configure it to be an RCP, and to disable OpenThread features not required for a RCP.

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

Enter the OpenThread section to configure the dedicated OpenThread UART:


```
Example Configuration
└── [*] Enable OpenThread RCP (Radio Co-Processor)         <─── ENABLE
    └── OpenThread RCP Configuration
        ├── OpenThread Transport ──> UART                  <─── SET TO THIS VALUE
        └── (configure UART parameters)
```

If you did not edit `sdkconfig.defaults.esp32c6` to enable OpenThread (above), modify these settings instead:

```
Component config
├── Wireless Coexistence
│   └── [*] Software controls WiFi/Bluetooth coexistence   <─── ENABLE
└── OpenThread
    ├── [*] OpenThread                                     <─── ENABLE
    ├── Thread Task Parameters
    │   └── (3072) Size of OpenThread task                 <─── SET TO THIS VALUE
    ├── Thread Console
    │   ├── [ ] Enable OpenThread console                  <─── DISABLE
    │   └── [ ] Enable Openthread Command-Line Interface   <─── DISABLE
    ├── Thread Core Features
    │   ├── Thread device type ──> Radio Only Device       <─── SET TO THIS VALUE
    │   ├── [ ] Enable Commissioner                        <─── DISABLE
    │   ├── [ ] Enable Joiner                              <─── DISABLE
    │   ├── [ ] Enable SRP Client                          <─── DISABLE
    │   ├── [ ] Enable DNS Client                          <─── DISABLE
    │   ├── [ ] Enable diag Client                         <─── DISABLE
    └── Thread Log
        └── [ ] Enable dynamic log level control           <─── DISABLE
```

You can now build the co-processor as an RCP.

### 2.2 OpenThread Configuration on the Host

> [!NOTE]
> This section targets the ESP32-P4 as the OpenThread Host.

OpenThread configuration on the host depends on the OpenThread features to be enabled. ESP-Hosted includes two examples:

- [OpenThread as a Border Router](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_openthread_border_router)
- [OpenThread with CLI interface](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_openthread_cli)

Further OpenThread Host examples can be found in the [ESP-IDF Examples directory](https://github.com/espressif/esp-idf/tree/master/examples/openthread).

**Further configuration**

PSRAM should be enabled to run OpenThread with ESP-Hosted due to memory requirements.

The OpenThread host must also be configured to recognise that the OpenThread RCP has coexistence enabled: otherwise OpenThread initialisation will fail due to a mismatch of OpenThread capabilities between the Host and RCP.

```text
### enable SPIRAM, else application will not start (out of memory)
CONFIG_SPIRAM=y
CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM=y

### SW coexistence on the co-processor is on, so this option must be
### disabled on host to keep OpenThread capabilities consistent
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n
```

For the ESP32-P4, these have been set-up in the `sdkconfig.default.esp32p4` configuration files in the ESP-Hosted OpenThread examples.

### 2.2.1 Initialising the OpenThread Connection from the Host Application

Initialise the ESP-Hosted interface first:

```c
esp_hosted_init();
esp_hosted_connect_to_slave();
```

Then initialise the OpenThread RCP using ESP-Hosted:

```c
esp_hosted_openthread_rcp_init();
esp_hosted_openthread_rcp_start();
```

In ESP-IDF, the OpenThread Host interface needs to know the OpenThread radio configuration. This is provided via ESP-Hosted:

```c
esp_hosted_openthread_radio_config_t hosted_radio_config = { 0 };
esp_hosted_openthread_get_radio_config(&hosted_radio_config);
```

`hosted_radio_config` is then used to configure the OpenThread's radio configuration.

See the ESP-Hosted OpenThread examples for more information.

### 2.3 Zigbee Configuration on the Host

> [!NOTE]
> This section targets the ESP32-P4 as the Zigbee Host.

> [!IMPORTANT]
> Zigbee support has been tested using ESP-IDF 5.5.4.

ESP-Hosted includes the following example:

- [Home Automation thermostat on a Zigbee Coordinator](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/host_zigbee_thermostat)

See the ESP Zigbee SDK for a [Zigbee Gateway Example](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/zigbee_gateway). Further Zigbee examples can be found in the [ESP Zigbee SDK](https://github.com/espressif/esp-zigbee-sdk/).

### 2.3.1 Initialising the Zigbee Connection from the Host Application

Initialising the ESP-Hosted interface and setting the Zigbee radio configuration is similar to setting up under OpenThread. See [Section 2.2.1 above](#221-initialising-the-openthread-connection-from-the-host-application)

## 3. More Information

- [ESP-IDF OpenThread documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/openthread.html)
- [ESP-IDF Coexistence Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)
- [ESP-IDF OpenThread Border Router Example for ESP32-P4 with two co-processors](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md)
- [ESP Zigbee SDK Programming Guide](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/index.html)
- [ESP Zigbee SDK](https://github.com/espressif/esp-zigbee-sdk/)
