# ESP-Hosted Performance Optimization Guide

Quick reference for optimizing ESP-Hosted performance across co-processors and different transport interfaces.

<details>
<summary>Table of Contents</summary>

1. [Quick Start - High Performance Config](#1-quick-start---high-performance-config)

	1.1 [ESP32-C6 as Co-Procesor](#11-esp32-c6-as-co-procesor)

	1.2 [ESP32-C5 as Co-Processor](#12-esp32-c5-as-co-processor)

	1.3 [ESP32-C61 as Co-Processor](#13-esp32-c61-as-co-processor)

	1.4 [ESP32-C2 as Co-Processor](#14-esp32-c2-as-co-processor)

	1.5 [ESP32-S2 as Co-Processor](#15-esp32-s2-as-co-processor)

	1.6 [ESP32-C3 as Co-Processor](#16-esp32-c3-as-co-processor)

2. [Transport Optimization](#2-transport-optimization)

	2.1 [SDIO (Highest Performance)](#21-sdio-highest-performance)

	2.2 [SPI Full-Duplex](#22-spi-full-duplex)

	2.3 [2.3 SPI Half-Duplex](#23-spi-half-duplex)

	2.4 [UART (Lowest Performance)](#24-uart-lowest-performance)

3. [Memory Optimization](#3-memory-optimization)

4. [Hardware Guidelines](#4-hardware-guidelines)

	4.1 [Critical Requirements](#41-critical-requirements)

	4.2 [PCB Design Checklist](#42-pcb-design-checklist)

	4.3 [Development Workflow](#43-development-workflow)

</details>

## 1 Quick Start - High Performance Config

For immediate performance gains, add these to your host's `sdkconfig.defaults.esp32XX` file based on the co-processor you are using.

> [!NOTE]
> Adjust the values based on your MCU host's and co-processor memory capacity. These values may change when more testing shows better performance figures.

Test conditions for throughput numbers using the performance settings:

- raw: data transferred from sender to receiver over transport
- iPerf used to measure TCP and UDP throughput

A diagram showing the setup used to get the throughput numbers.

```mermaid
flowchart TB
    %% floating IP labels — above their boxes, faint leader line
    ipAP["192.168.1.1"]:::ip
    ipP4["192.168.1.2"]:::ip
    ipHost["192.168.1.88 · test<br/>10.0.0.1 · control"]:::ip
    ipDev["10.0.0.2"]:::ip

    subgraph SB["Shield Box"]
        AP["📶 AP / Router"]

        subgraph EVB["ESP32-P4-Function-EV-Board 1.2+"]
            direction LR
            C6["ESP32-C6<br/>Wi-Fi slave"]
            P4["ESP32-P4<br/>iperf app"]
            C6 ---|"SDIO"| P4
        end

        AP -.-|"Wi-Fi"| C6
        HOST["AP-backend<br/>iperf host"]
        AP ===|"LAN cable"| HOST
        P4 ---|"USB / UART"| HOST
    end

    DEV["Dev machine"]
    HOST ---|"control"| DEV

    %% faint leaders from IP labels to devices
    ipAP -.- AP
    ipP4 -.- P4
    ipHost -.- HOST
    ipDev -.- DEV

    style SB fill:#fff7ec,stroke:#e8a33d,color:#333
    style EVB fill:#efeaf8,stroke:#8a7bbd,color:#333
    classDef node fill:#ffffff,stroke:#99aabb,color:#111
    classDef ip fill:none,stroke:none,color:#555
    class AP,C6,P4,HOST,DEV node

    linkStyle 0 stroke:#e8762d,stroke-width:2.5px
    linkStyle 1 stroke:#e8762d,stroke-width:2.5px
    linkStyle 2 stroke:#e8762d,stroke-width:2.5px
    linkStyle 3 stroke:#888888,stroke-width:1.5px,stroke-dasharray:5
    linkStyle 4 stroke:#0e9488,stroke-width:2px
    linkStyle 5 stroke:#cccccc,stroke-width:1px,stroke-dasharray:2 2
    linkStyle 6 stroke:#cccccc,stroke-width:1px,stroke-dasharray:2 2
    linkStyle 7 stroke:#cccccc,stroke-width:1px,stroke-dasharray:2 2
    linkStyle 8 stroke:#cccccc,stroke-width:1px,stroke-dasharray:2 2
```

> [!NOTE]
> The diagram shows the router and ESP board in a shield box. The performance numbers here were obtained from an 'Open Air' configuration, without using a shield box.

### 1.1 ESP32-C6 as Co-Procesor

```
### sdkconfig for ESP32-P4 + C6 as co-processor

# Let P4 know, C6 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y

# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=16
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=64
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=64
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=32

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
CONFIG_LWIP_TCP_WND_DEFAULT=65534
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCP_SACK_OUT=y
```

**Throughput using the settings.**

Using SDIO Transport, 4-bits, running at 40MHz, connected to a 2.4GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to C6      |      72 |
| Raw        | C6 to P4      |      80 |
| iPerf, TCP | P4 to Test PC |      32 |
| iPerf, UDP | P4 to Test PC |      50 |
| iPerf, TCP | Test PC to P4 |      30 |
| iPerf, UDP | Test PC to P4 |      49 |

### 1.2 ESP32-C5 as Co-Processor

```
### sdkconfig for ESP32-P4 + C5 as co-processor

# Let P4 know, C5 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32C5=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5=y

# Optional PCB selection: Uncomment if you are using Pre designed PCB P4_C5_CORE_BOARD (to use correct GPIOs on that PCB)
# CONFIG_ESP_HOSTED_P4_C5_CORE_BOARD=y

# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=10
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=32
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=16

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520
CONFIG_LWIP_TCP_WND_DEFAULT=32768
CONFIG_LWIP_TCP_RECVMBOX_SIZE=48
CONFIG_LWIP_UDP_RECVMBOX_SIZE=48
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=48

CONFIG_LWIP_TCP_SACK_OUT=y
```

**Throughput using the settings.**

Using SDIO Transport, 4-bits, running at 40MHz, connected to a 5GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to C5      |      72 |
| Raw        | C5 to P4      |      81 |
| iPerf, TCP | P4 to Test PC |      23 |
| iPerf, UDP | P4 to Test PC |      67 |
| iPerf, TCP | Test PC to P4 |      32 |
| iPerf, UDP | Test PC to P4 |      68 |

### 1.3 ESP32-C61 as Co-Processor

```
### sdkconfig for ESP32-P4 + C61 as co-processor

# Let P4 know, C61 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32C61=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61=y

# Optional PCB selection: Uncomment if you are using Pre designed PCB P4_C61_CORE_BOARD (to use correct GPIOs on that PCB)
# CONFIG_ESP_HOSTED_P4_C61_CORE_BOARD=y

# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=10
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=16
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=16
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=16

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=12930
CONFIG_LWIP_TCP_WND_DEFAULT=22488
CONFIG_LWIP_TCP_RECVMBOX_SIZE=48
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=48
CONFIG_LWIP_IP_REASS_MAX_PBUFS=15

CONFIG_LWIP_TCP_SACK_OUT=y

CONFIG_LWIP_TCPIP_CORE_LOCKING=y
CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT=y
```

**Throughput using the settings.**

Using SPI-FD Transport, running at 40MHz, connected to a 2.4GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to C61     |      25 |
| Raw        | C61 to P4     |      26 |
| iPerf, TCP | P4 to Test PC |      12 |
| iPerf, UDP | P4 to Test PC |      18 |
| iPerf, TCP | Test PC to P4 |      15 |
| iPerf, UDP | Test PC to P4 |      23 |

### 1.4 ESP32-C2 as Co-Processor

```
# Let P4 know, C2 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32C2=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C2=y

### sdkconfig for ESP32-P4 + C2 as co-processor
# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=10
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=32
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=6
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=6

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384
CONFIG_LWIP_TCP_WND_DEFAULT=32768
CONFIG_LWIP_TCP_RECVMBOX_SIZE=20
CONFIG_LWIP_UDP_RECVMBOX_SIZE=20
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=20

CONFIG_LWIP_TCP_SACK_OUT=y
```

**Throughput using the settings.**

Using SPI-FD Transport, running at 40MHz, connected to a 2.4GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to C2      |      25 |
| Raw        | C2 to P4      |      26 |
| iPerf, TCP | P4 to Test PC |      12 |
| iPerf, UDP | P4 to Test PC |      18 |
| iPerf, TCP | Test PC to P4 |      13 |
| iPerf, UDP | Test PC to P4 |      15 |

### 1.5 ESP32-S2 as Co-Processor

```
### sdkconfig for ESP32-P4 + S2 as co-processor

# Let P4 know, S2 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32S2=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32S2=y

# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=8
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=24
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=24
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=16
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=16

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=17280
CONFIG_LWIP_TCP_WND_DEFAULT=28000
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32
CONFIG_LWIP_UDP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32

CONFIG_LWIP_TCP_SACK_OUT=y
```

### 1.6 ESP32-C3 as Co-Processor

```
### sdkconfig for ESP32-P4 + C3 as co-processor

# Let P4 know, C3 is attached as slave
CONFIG_SLAVE_IDF_TARGET_ESP32C3=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C3=y

# Wi-Fi Performance
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=20
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=40
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=40
CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_AMPDU_RX_ENABLED=y
CONFIG_WIFI_RMT_RX_BA_WIN=32

# TCP/IP Performance
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=40960
CONFIG_LWIP_TCP_WND_DEFAULT=40960
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64
```

**Throughput using the settings.**

Using SPI-FD Transport, running at 40MHz, connected to a 2.4GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to C3      |      26 |
| Raw        | C3 to P4      |      26 |
| iPerf, TCP | P4 to Test PC |      19 |
| iPerf, UDP | P4 to Test PC |      25 |
| iPerf, TCP | Test PC to P4 |      18 |
| iPerf, UDP | Test PC to P4 |      20 |

**Throughput using the settings.**

Using SPI-FD Transport, running at 40MHz, connected to a 2.4GHz network over the air

| Type       | Direction     | MBits/s |
|------------|---------------|--------:|
| Raw        | P4 to S2      |      25 |
| Raw        | S2 to P4      |      26 |
| iPerf, TCP | P4 to Test PC |       8 |
| iPerf, UDP | P4 to Test PC |      11 |
| iPerf, TCP | Test PC to P4 |      12 |
| iPerf, UDP | Test PC to P4 |      15 |

## 2 Transport Optimization

### 2.1 SDIO (Highest Performance)
- **Clock Speed**: Start at 20 MHz, optimize up to 50 MHz
- **Bus Width**: Use 4-bit mode
- **Hardware**: Use PCB with controlled impedance, external pull-ups (51kΩ)
- **Checksum**: Optional (SDIO hardware handles verification)

```
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4
```

> [!NOTE]
> See [Performance and Memory Usage](sdio.md#9-performance-and-memory-usage) on the trade-off between SDIO Performance and Memory Use

### 2.2 SPI Full-Duplex
- **Clock Speed**: ESP32: ≤10 MHz, Others: ≤40 MHz
- **Hardware**: Use IO_MUX pins, short traces (≤10cm for jumpers)
- **Checksum**: Mandatory (SPI hardware lacks error detection)

```
CONFIG_ESP_HOSTED_SPI_CLK_FREQ=40
```

### 2.3 SPI Half-Duplex
- **Data Lines**: Use 4-line (Quad SPI) mode
- **Similar optimizations as SPI Full-Duplex**

### 2.4 UART (Lowest Performance)
- **Baud Rate**: Use 921600 (highest stable rate)
- **Best for**: Low-throughput applications, debugging

## 3 Memory Optimization

- Reduce memory footprint for resource-constrained applications:

  ```
  # Reduce queue sizes
  CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=10    # Default: 20
  CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=10    # Default: 20

  # Enable memory pooling
  CONFIG_ESP_HOSTED_USE_MEMPOOL=y
  ```

- Disable the not-in-use features
  - For example, disable bluetooth if not needed
- Use external RAM, for higher memory (PSRAM is supported)
- Optimise internal RAM using [ESP-IDF iram optimization tricks](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/ram-usage.html) 
## 4 Hardware Guidelines

### 4.1 Critical Requirements
1. **Signal Integrity**: Use PCB designs for production, jumpers only for prototyping
2. **Power Supply**: Stable 3.3V, proper decoupling capacitors
3. **Trace Length**: Match lengths, especially clock vs data lines
4. **Pull-ups**: Required for SDIO (51kΩ) on CMD, D0-D3 lines

### 4.2 PCB Design Checklist
- [ ] Equal trace lengths for communication signals
- [ ] Ground plane for signal stability
- [ ] Controlled impedance traces (50Ω typical)
- [ ] Series termination resistors for high-speed signals
- [ ] Extra GPIOs reserved for future features (deep sleep, etc.)

### 4.3 Development Workflow
1. **Proof of Concept**: Start with jumper wires, low clock speeds
2. **Incremental Optimization**: Increase transport clock step by step
3. **Hardware Validation**: Move to PCB for final validation
4. **Performance Tuning**: Optimize buffers and configurations
5. **Disable features**: Any unused components from ESP-IDF or

ESP-Hosted-MCU features could be disabled for more memory
availability.
