---
title: "Frequency Analyzer"
description: "Visualize signal strength across SubGHz bands to find active frequencies"
weight: 40
---

The Frequency Analyzer scans across all supported SubGHz bands to visualize signal activity and help identify the frequency used by target devices.

## Use cases

- **Unknown frequency**: When you don't know which band a device uses
- **Multi-band environments**: Areas with devices on different frequencies
- **Signal hunting**: Locating active transmitters in the area
- **Verification**: Confirming a device's transmission frequency before capture

## Using the Frequency Analyzer

### On-device UI

1. Open **Menu → SubGHz → Freq Analyzer**.
2. The device will scan across all five bands (315, 390, 433.92, 868.35, 915 MHz).
3. Signal strength is displayed as bar graphs for each band.
4. The analyzer updates in real-time to show current activity.
5. Press **Back** to stop the analyzer.

### Command line

The Frequency Analyzer is primarily a UI tool. Use the standard scanner with frequency cycling for CLI-based frequency detection:

```bash
# Start scanner
subghz start

# Cycle through frequencies while watching signal strength
subghz cycle_freq
```

## Band visualization

The analyzer displays five bands, each showing signal strength levels:

- **315** - North American garage doors and remotes
- **390** - Security systems and remote controls
- **433** - Most common worldwide (garage doors, gates, sensors)
- **868** - European remotes and alarm systems
- **915** - North America ISM band devices

Higher bars indicate stronger signals on that band.

## Signal detection workflow

1. **Start the analyzer** and let it run for 30-60 seconds to establish a baseline.
2. **Activate the target device** (press the remote button, trigger the sensor, etc.).
3. **Watch for spikes** in the band visualization when the device transmits.
4. **Note the active band** - this is the frequency to use for capture.
5. **Switch to the scanner** on that band for detailed signal analysis.

## Tips

- **Environment matters**: Background noise from other devices can create false positives. Test in a quiet RF environment if possible.
- **Distance**: Start closer to the target device for clearer signal detection.
- **Multiple activations**: Some devices transmit intermittently; activate the target multiple times while watching the analyzer.
- **Baseline comparison**: Note the baseline noise levels before activating the target to distinguish its signal from background.

## Notes

- The Frequency Analyzer provides a quick overview but is less precise than the detailed 64-channel scanner.
- For precise frequency identification within a band, use the scanner on that band.
- Some devices transmit very briefly; you may need to activate them repeatedly to see the signal.

## Troubleshooting

- **No bands show activity**: Ensure the target device is transmitting. Check antenna connection. Try moving closer to the device.
- **All bands show high activity**: You may be in an area with high RF interference. Try moving to a different location or testing at a different time.
- **Unclear which band is the target**: Activate the device multiple times and watch for consistent spikes on a specific band.
