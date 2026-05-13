---
title: "Scanning"
description: "Monitor SubGHz radio activity across frequency bands"
weight: 10
---

Monitor radio activity across 64 channels in real-time to detect signals on supported frequency bands.

## Prerequisites

- GhostESP device with CC1101 SubGHz hardware (see [Hardware Support]({{< relref "hardware.md" >}}))
- Device powered on with antenna attached

## Scanning for signals

### On-device UI

1. Open **Menu → SubGHz → Capture**.
2. The device will start scanning the current frequency band.
3. Signal strength is displayed across 64 channels.
4. Use **Cycle Frequency** to switch between bands (315, 390, 433.92, 868.35, 915 MHz).
5. Press **Back** to stop scanning.

### Command line

```bash
# Start scanning on current frequency
subghz start

# Stop scanning
subghz stop

# Pause/resume scanning
subghz pause
subghz resume

# Cycle to next frequency band
subghz cycle_freq

# Check scanner status
subghz status
```

## Frequency bands

The scanner supports five common SubGHz bands. Cycle through them to find the one used by your target device:

- **315 MHz** - Common in North America for garage doors and remote controls
- **390 MHz** - Used by some security systems and remote controls
- **433.92 MHz** - Most common worldwide for garage doors, gates, and wireless sensors
- **868.35 MHz** - European standard for remote controls and alarm systems
- **915 MHz** - North America ISM band for various devices

## Signal strength visualization

The scanner displays signal strength across 64 channels within the selected band. Higher bars indicate stronger signals. Use this to:

- Identify active channels
- Find the frequency of a transmitting device
- Verify signal reception before capturing

## Notes

- SubGHz scanning is not available on builds without CC1101 hardware (CONFIG_HAS_SUBGHZ or CONFIG_HAS_SUBGHZ_REMOTE must be set).
- Scanning is mutually exclusive with other SubGHz operations; starting a new scan will stop any ongoing capture or transmission.
- Signal strength is displayed as relative levels; higher values indicate stronger signals.
- Some devices transmit intermittently; leave the scanner running for several minutes to detect periodic signals.

## Troubleshooting

- **No signals detected**: Try different frequency bands or move closer to the target device. Ensure the antenna is properly connected.
- **Scanner fails to start**: Check that your device has SubGHz hardware enabled. Verify SPI pin configuration in the build config.
- **Weak signals**: Improve antenna placement or use a higher-gain antenna compatible with the CC1101 frequency range.
