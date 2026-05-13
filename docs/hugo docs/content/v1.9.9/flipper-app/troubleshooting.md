---
title: "Troubleshooting"
description: "Solve common issues with the Flipper Zero companion app."
weight: 80
keywords: ["troubleshooting", "issues", "problems", "fix", "error"]
---

## Connection Issues

### App can't connect to GhostESP
- Verify serial wiring (TX, RX, GND)
- Check that GhostESP device is powered on
- Try restarting both devices
- Check serial port configuration in app settings

### Connection drops during use
- Check wiring connections for loose connections
- Verify power supply is stable
- Move away from sources of electrical interference
- Try reducing baud rate if using high-speed connection

## Feature Not Working

### Wi-Fi scan returns no results
- Ensure GhostESP firmware supports Wi-Fi operations
- Check that device is not in a restricted regulatory region
- Verify antenna is properly connected
- Try scanning from a different location

### GPS not working
- Verify GPS module is connected and powered
- Ensure GPS has clear view of sky
- Wait for GPS to acquire satellite lock (may take 1-2 minutes)
- Check GPS module wiring and configuration

### BLE scan shows no devices
- Ensure BLE is enabled in GhostESP settings
- Move closer to target devices
- Check that target devices are advertising (not in sleep mode)
- Verify BLE antenna connection

## File Export Issues

### PCAP files not saving
- Ensure Flipper Zero SD card is inserted
- Check SD card has free space
- Verify SD card is properly formatted (FAT32)
- Check file path permissions

### CSV export fails
- Verify SD card is mounted
- Check available storage space
- Ensure GPS data is being received (for GPS-related exports)

## App Crashes or Freezes

### App freezes during operation
- Restart the Flipper Zero
- Update to latest app version
- Check for firmware compatibility issues
- Reduce capture size or scan duration

### App won't launch
- Verify `.fap` file is compatible with your Flipper firmware version
- Reinstall the app from app store or latest release
- Check Flipper Zero has sufficient memory
- Update Flipper Zero firmware if outdated

## Support and Resources

- **GitHub Repository**: [github.com/GhostESP-Revival/GhostESP-FlipperCompanion](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion)
- **Issues**: Report bugs or request features on the [GitHub Issues page](https://github.com/GhostESP-Revival/GhostESP-FlipperCompanion/issues)
- **Discord**: Join the community on [Discord](https://discord.gg/5cyNmUMgwh)
- **Documentation**: Check the docs at [docs.ghostesp.net](https://docs.ghostesp.net)

## Credits

- **Original Developer**: Spooky ([Spooks4576](https://github.com/Spooks4576))
- **Maintainer**: Jay Candel ([GhostESP-Revival](https://github.com/GhostESP-Revival))
- **Contributor**: @tototo31 ([tototo31](https://github.com/tototo31))
