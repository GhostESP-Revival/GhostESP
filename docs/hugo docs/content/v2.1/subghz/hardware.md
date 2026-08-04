---
title: "Hardware Support"
description: "Boards and peripherals required for GhostESP SubGHz"
weight: 70
---

SubGHz functionality requires a CC1101-based radio module. Not all GhostESP boards include this hardware.

## Supported boards

SubGHz support depends on the board configuration. Check the build configuration (sdkconfig) for these options:

- **CONFIG_HAS_SUBGHZ** - Board has local CC1101 hardware
- **CONFIG_HAS_SUBGHZ_REMOTE** - Board supports remote SubGHz mode (display/control only)
.

## CC1101 module

The SubGHz functionality uses the Texas Instruments CC1101 low-power sub-1 GHz transceiver chip.

### Frequency coverage

The CC1101 supports the following frequency ranges:
- 300-348 MHz
- 387-464 MHz
- 779-928 MHz

GhostESP uses the common bands within these ranges: 315, 390, 433.92, 868.35, and 915 MHz.

## SPI configuration

The CC1101 communicates via SPI. The board configuration defines the following pins:

- **CONFIG_SUBGHZ_SPI_HOST** - SPI bus number (typically 2 or 3)
- **CONFIG_SUBGHZ_SPI_MOSI_PIN** - MOSI pin
- **CONFIG_SUBGHZ_SPI_MISO_PIN** - MISO pin
- **CONFIG_SUBGHZ_SPI_SCK_PIN** - SCK pin
- **CONFIG_SUBGHZ_CSN_PIN** - Chip select pin
- **CONFIG_SUBGHZ_GDO0_PIN** - GPIO0 interrupt pin
- **CONFIG_SUBGHZ_GDO2_PIN** - GPIO2 interrupt pin

These values are set in the board's sdkconfig file and should not be modified unless adding custom hardware.

## Antenna requirements

A properly matched antenna is essential for SubGHz operation:

- **Frequency-specific**: Use an antenna tuned to the target frequency band.
- **Impedance**: 50 ohm impedance matching is standard.
- **Connector**: SMA or u.FL connector depending on the board.
- **Gain**: Higher gain antennas improve range but may violate regulatory limits.

### Common antenna types

- **Dipole antennas** - Simple, omnidirectional, good for general use
- **Helical antennas** - Compact, moderate gain
- **Yagi antennas** - Directional, high gain for long-range applications

## Power considerations

The CC1101 operates at low power but current draw increases during transmission:

- **Receive mode**: ~15 mA
- **Transmit mode**: ~30-50 mA (depending on power setting)

Ensure your power supply can handle these currents, especially during extended transmission periods.

## Remote mode

Some boards support remote SubGHz mode (CONFIG_HAS_SUBGHZ_REMOTE only). In this mode:

- The board acts as a display/control interface
- Actual SubGHz operations are performed by a peer device
- Commands are sent over the communication link
- Useful for separating display from radio hardware

## Developer notes

When adding SubGHz support to a custom board:

1. Integrate a CC1101 module with appropriate SPI connections
2. Configure the SPI pins in the board's sdkconfig
3. Set CONFIG_HAS_SUBGHZ=y in the configuration
4. Add an appropriate antenna connector
5. Test with the `subghz status` command to verify hardware detection

## Regulatory compliance

SubGHz radio operation is subject to local regulations:

- **FCC (USA)** - Part 15 rules for unlicensed operation
- **ETSI (Europe)** - CE marking requirements
- **Other regions** - Check local regulations before transmission

Always ensure your usage complies with local laws and regulations regarding radio transmission.

## Troubleshooting

- **SubGHz not available**: Verify CONFIG_HAS_SUBGHZ or CONFIG_HAS_SUBGHZ_REMOTE is set in the build config.
- **Hardware detection fails**: Check SPI pin configuration and CC1101 power supply.
- **Poor reception**: Verify antenna connection and use a frequency-matched antenna.
- **Transmission issues**: Ensure adequate power supply and check regulatory compliance for transmission power.
