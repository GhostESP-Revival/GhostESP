---
title: "Wiring Modules and Peripherals"
description: "Find the pins your board uses for CC1101, NRF24, NFC, GPS, and SD cards, and wire add-on modules safely."
weight: 70
toc: true
keywords: ["wiring", "pinout", "pins", "GPIO", "CC1101", "NRF24", "PN532", "GPS", "SD card", "SPI", "I2C", "UART"]
---

## What this does

This page explains how GhostESP maps add-on modules to ESP32 pins, and how to find the exact pins your board is already using.

## Do I need this page?

**If you bought or already own a supported board, you probably don't.** Every release build ships with its pins already configured, so a board with a built-in CC1101, NRF24, NFC, or GPS works out of the box.

You need this page when you are:

- Adding a module to a generic ESP32 board
- Changing SD card pins
- Building a custom board configuration

## Before you start

- **3.3 V logic only.** ESP32 GPIOs are not 5 V tolerant. Feeding 5 V into a pin can destroy the chip.
- Check the [Supported Hardware]({{< relref "supported-hardware.md" >}}) table first — many "missing feature" problems are really "this board doesn't have that hardware."
- Have a way to read serial output at 115200 baud. Boot logs and status commands are how you confirm wiring.

## Find your board's pins

### SD card (changeable at runtime)

```
sd_config
```

prints the pins currently in use. To change them without rebuilding:

```
sd_pins_spi <cs> <clk> <miso> <mosi>
sd_save_config
```

`sd_save_config` stores the pins in NVS so they survive a reboot. This is the only peripheral most users ever need to reconfigure by hand.

### CC1101 SubGHz

Start the scanner and read the first log line:

```
subghz start
```

GhostESP prints the pins it is actually using:

```
SubGHz cfg: SPI2 MOSI=18 MISO=38 SCK=40 CSN=39 GDO0=41 GDO2=-1
```

If that line does not appear, the build was compiled without SubGHz support for your board.

### Build-time pins

If you are compiling your own build, every peripheral pin is set under **Ghost ESP Options** in `menuconfig`:

| Peripheral | menuconfig section |
|---|---|
| CC1101 | **SubGHz Options** |
| NRF24 | **Misc Options → NRF24 Options** |
| NFC (PN532) | **NFC Options** |
| GPS | **GPS Options** |
| IR | **Infrared Options** |

The Kconfig defaults for a CC1101 are MOSI `18`, MISO `38`, SCK `40`, CSN `39`, GDO0 `41`, and GDO2 disabled (`-1`). These are generic defaults, **not** a wiring recommendation for any specific board — confirm against your board's own configuration before connecting anything.

## Module wiring reference

These are the signals each module needs. Match them to the pin numbers from the section above.

### CC1101 (SubGHz)

| CC1101 pin | Connects to |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| MOSI / SI | ESP32 MOSI |
| MISO / SO | ESP32 MISO |
| SCK / SCLK | ESP32 SCK |
| CSN / CS | ESP32 chip select |
| GDO0 | ESP32 interrupt pin |
| GDO2 | Optional (usually left unconnected) |

Attach a **frequency-matched antenna before transmitting**. Powering a CC1101 without an antenna can damage the module's output stage.

### NRF24L01+

| NRF24 pin | Connects to |
|---|---|
| VCC | 3.3 V (see note) |
| GND | GND |
| MOSI | ESP32 MOSI |
| MISO | ESP32 MISO |
| SCK | ESP32 SCK |
| CSN | ESP32 chip select |
| CE | ESP32 CE pin |

> **Note:** the PA+LNA versions of the NRF24 draw far more current than the plain modules. A weak 3.3 V supply causes "radio not responding" failures that look like wiring bugs — add a decoupling capacitor across VCC/GND, or power it from a dedicated 3.3 V regulator.

### PN532 (NFC)

Most PN532 modules support I2C, SPI, and UART, and the onboard DIP switches select the mode. GhostESP uses **I2C** for PN532:

| PN532 pin | Connects to |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | ESP32 SDA |
| SCL | ESP32 SCL |
| IRQ | Optional |
| RST | Optional |

Set the module's switches to I2C before wiring. A PN532 left in SPI or UART mode will not respond.

### GPS

GPS modules are UART devices. Cross the lines:

| GPS pin | Connects to |
|---|---|
| TX | ESP32 RX |
| RX | ESP32 TX |
| VCC | 3.3 V |
| GND | GND |

Most modules default to 9600 baud. Some need a battery or a clear view of the sky before they output a fix — no data indoors is normal.

## Verified board wiring

These pinouts are documented elsewhere in the docs and are safe to follow directly.

### Flipper Zero to ESP32

| ESP32 pin | Flipper GPIO pin |
|---|---|
| TX | GPIO 13 or 15 |
| RX | GPIO 14 or 16 |

See the [Flipper app connection guide]({{< relref "../flipper-app/connection.md" >}}) for the full setup.

### GhostLink (two GhostESP boards)

| Signal | Connection |
|---|---|
| TX of Device A | RX of Device B (GPIO 6 → GPIO 7; 17 → 16 on base ESP32 models) |
| RX of Device A | TX of Device B (GPIO 7 → GPIO 6; 16 → 17 on base ESP32 models) |

See [GhostLink]({{< relref "dual-communication.md" >}}) for pairing and transport setup.

### GhostLink P1

The GhostLink P1 handheld has a complete, board-specific wiring map for its display, joystick, SD card, and charging circuit in its [owner's guide]({{< relref "../hardware/ghostlink-p1.md" >}}).

## Troubleshooting

- **Module not detected:** confirm 3.3 V power first, then check that MOSI and MISO are not swapped. Swapped data lines are the most common SPI wiring mistake.
- **One module works, another does not:** modules on a shared SPI bus must each have their own chip-select pin, and only one may be active at a time. Boards that share a bus (the TEmbed C1101 is one) hold unused CS pins high to avoid conflicts.
- **Works until you start a Wi-Fi or BLE task:** radios compete for power and pins. See [Control methods]({{< relref "control-methods.md" >}}) for what can run at the same time.
- **Pins keep resetting after reboot:** you changed them at runtime but did not run `sd_save_config`, or the build config overrides them.

## Related tasks

- [Supported Hardware]({{< relref "supported-hardware.md" >}})
- [SD card storage]({{< relref "sd-card.md" >}})
- [Custom board configurations]({{< relref "../development/custom-board-configs.md" >}})
- [Command line reference]({{< relref "command-line-reference.md" >}})
