---
title: "Connection Setup"
description: "Configure the serial connection between your Flipper Zero and GhostESP device."
weight: 20
keywords: ["connection", "uart", "serial", "gpio", "wiring"]
---

## Hardware Connection

Connect your ESP32 device to your Flipper Zero via UART:

| ESP32 Pin | Flipper GPIO Pin |
|-----------|------------------|
| TX | GPIO 13 or 15 |
| RX | GPIO 14 or 16 |
| GND | GND |
| VCC | 3.3V (if needed, check your board's power requirements) |

> **Note**: Pin configurations may vary depending on your Flipper Zero setup. Refer to your specific hardware documentation or the companion app's connection guide.

## Software Connection

1. Power on your GhostESP device
2. Launch the GhostESP app on your Flipper Zero
3. The app will automatically attempt to connect via serial

## Connection Troubleshooting

**App can't connect to GhostESP**
- Verify serial wiring (TX, RX, GND)
- Check that GhostESP device is powered on
- Try restarting both devices
- Check serial port configuration in app settings

**Connection drops during use**
- Check wiring connections for loose connections
- Verify power supply is stable
- Move away from sources of electrical interference
- Try reducing baud rate if using high-speed connection

## Next Steps

Once connected, explore the available features:

- [Wi-Fi Features]({{< relref "wifi-features.md" >}})
- [BLE Features]({{< relref "ble-features.md" >}})
- [GPS Features]({{< relref "gps-features.md" >}})
