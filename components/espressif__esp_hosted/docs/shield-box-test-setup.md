# Shield Box Test Setup for ESP-Hosted

Controlled RF environment for consistent throughput measurements and performance evaluation.

## Overview

**Shield Box Testing** uses RF-shielded enclosure to eliminate external interference and provide repeatable test conditions.

**Key Benefits:**
- Controlled RF environment (no external Wi-Fi/cellular interference)
- Repeatable, consistent results
- Better measurement accuracy vs open air


## Equipment Required

### Essential Components
- **RF Shield Box/Chamber**: Faraday cage enclosure
- **ESP32-P4 Function EV Board**: Host device
- **ESP32-C6/C5 Test Board**: Co-processor device  
- **External PC**: For iPerf client/server
- **Router/Access Point**: Wi-Fi infrastructure
- **Ethernet Connection**: Wired backhaul to PC

Please change the host and co-processor nodes as per current use-case under test.

## Test Setup

### Physical Configuration

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

### Data Flow
- **PC to MCU Host**:
  ```
  PC -> Router -> ESP Co-processor == SDIO/SPI/UART ==> ESP32-P4
  ```
- **MCU Host to PC**:
  ```
  PC <- Router <- ESP Co-processor <== SDIO/SPI/UART == ESP32-P4
  ```

**Traffic route:**
- PC-to-Router: Ethernet with static IP (eliminates wireless variables)
- Router-to-ESP: Wi-Fi connection (only wireless link in test chain)

## Transport Configurations

### SDIO (Highest Performance)
- **Clock**: 20-50 MHz (start low, optimize up)
- **Bus Width**: 4-bit mode
- **Hardware**: External pull-ups (51kΩ) on CMD, D0-D3

### SPI 
- **Clock**: ESP32: ≤10 MHz, Others: ≤40 MHz
- **Mode**: Full-duplex (simple) or Quad SPI (highest throughput)

### UART
- **Baud Rate**: 921600 (highest stable rate)
- **Use Case**: Low-throughput validation, debugging


## Shield Box vs Open Air

| Aspect | Shield Box | Open Air |
|--------|------------|----------|
| **Repeatability** | High | Variable |
| **Interference** | Eliminated | Present |
| **Debugging** | Easier | Complex |
| **Reality** | Lower | Higher |


---

*For transport setup details: [SDIO](sdio.md) | [SPI Full-Duplex](spi_full_duplex.md) | [SPI Half-Duplex](spi_half_duplex.md) | [UART](uart.md)*