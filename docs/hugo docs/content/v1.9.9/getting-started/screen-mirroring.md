---
title: "Screen Mirroring"
description: "Mirror your GhostESP display to your desktop for easier viewing and remote control."
weight: 35
---

View and control your device's screen from your computer.

## Prerequisites

- A GhostESP device with a display (CYD, T-Deck, Cardputer, etc.)
- USB cable to connect the device
- Python 3.8 or newer (for desktop script only)
- **Note**: CYD devices require 460800 baud instead of the default 115200, and use 8-bit color mode which may result in reduced color quality and slowdowns

## Installation (Desktop Script)

The script is in `scripts/screen_mirror/`. It installs dependencies (pygame, pyserial, numpy) on first run.

**For Web-Based Mirror**: If you prefer not to install Python, use the web mirror at [ghostesp.net/serial](https://ghostesp.net/serial) by opening the **Web Serial** tab.

## Starting the Mirror

### Desktop Script

1. Connect your GhostESP device via USB
2. Open a terminal in the `scripts/screen_mirror` folder
3. Run the script:

```
python ghost_mirror.py
```

Or specify a port directly:

```
python ghost_mirror.py COM3
```

**For CYD devices**, you must specify the higher baud rate:

```
python ghost_mirror.py COM3 --baud 460800
```

To list available serial ports:

```
python ghost_mirror.py --list
```

### Web-Based Mirror

Alternatively, visit [ghostesp.net/serial](https://ghostesp.net/serial) and open the **Screen Mirror** tab to use the browser-based mirror without installing Python. The web serial mirror also includes a console and file browser on the same page.

## Device-Specific Considerations

### CYD Devices

CYD devices need specific settings due to their 8-bit color mode:

- **Baud Rate**: Use `--baud 460800`; the default 115200 is too slow for CYD refresh rates.
- **Color Mode**: The mirror stream is truncated to 8 bits, so colors may shift compared to the physical display.
- **Performance**: Expect reduced frame rates and occasional slowdowns because the 8-bit stream is bandwidth-constrained.

To connect to a CYD device:

```
python ghost_mirror.py COM3 --baud 460800
```

Click **Swap** to fix inverted colors. Note that CYD mirroring is slower and has lower color quality.

## Using the Mirror

### Window Controls

- **Title bar**: Drag to move the window
- **× button**: Close the application

### Port Selection

Use the **◄** and **►** buttons in the header to cycle through available COM ports. The current port is displayed between the buttons.

### Connection

- **Connect**: Opens the serial connection and enables mirroring
- **Disconnect**: Closes the connection cleanly

### Display Controls

The virtual D-pad on the right side mirrors the physical controls on your device:

| Button | Action |
|--------|--------|
| ▲ | Navigate up |
| ▼ | Navigate down |
| ◄ | Navigate left / Go back |
| ► | Navigate right |
| ● | Select / Confirm |

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| W / ↑ | Up |
| S / ↓ | Down |
| A / ← | Left |
| D / → | Right |
| Enter / Space | Select |
| Escape | Exit |

### Byte Swap Toggle

If colors appear wrong (inverted or incorrect), click the **Swap** button to toggle byte order. This forces a full screen refresh.

**Note on CYD Color Issues**: CYD devices use 8-bit color mode during mirroring, which inherently produces less accurate colors than 16-bit modes. If toggling the **Swap** button doesn't fully resolve color issues on a CYD device, this is expected behavior due to the 8-bit limitation.

## Command Line Options

| Option | Description |
|--------|-------------|
| `--scale N` | Scale the display by factor N (default: 2) |
| `--baud N` | Set baud rate (default: 115200) |
| `--list` | List available serial ports |

### Examples

```
# Run with 3x scaling
python ghost_mirror.py COM3 --scale 3

# CYD device (requires 460800 baud)
python ghost_mirror.py COM3 --baud 460800

# Use a different baud rate
python ghost_mirror.py COM3 --baud 921600
```

## Status Indicators

- **Green dot**: Connected and receiving data
- **Red dot**: Disconnected or connection lost
- **FPS counter**: Shows current frame rate
- **Resolution**: Displays current screen dimensions

## Troubleshooting

### No display or black screen

- Ensure the device is powered on and showing content on its physical display
- Try clicking **Connect** to re-establish the connection
- Check that the correct COM port is selected

### Wrong colors

- Click the **Swap** button to toggle byte order
- The display will refresh with corrected colors
- **For CYD devices**: Poor color fidelity is normal due to 8-bit color mode.

### Connection lost

- The status indicator will turn red
- Click **Connect** to reconnect
- If the device was reset, wait for it to boot before reconnecting

### Device resets when connecting

- This is normal for some boards on first connection
- The script disables DTR/RTS to prevent resets after initial connection

### Slow or laggy display

- Screen mirroring uses USB serial which has bandwidth limitations
- Reduce scale factor if needed
- Close other applications using the serial port
- **For CYD devices**: Slowdowns are expected due to 8-bit color mode and bandwidth constraints. Ensure you're using `--baud 460800` for optimal performance. If the display is still slow, try reducing the scale factor further.

## Notes

- **Baud Rates**: Desktop script uses 115200 baud by default. CYD devices **require 460800 baud** for proper operation.
- **Color Modes**: Most devices support 16-bit color. CYD devices use 8-bit color mode, resulting in reduced color accuracy and potential slowdowns.
- **Performance**: CYD devices may experience frame rate reductions and occasional slowdowns due to 8-bit mode and serial bandwidth limitations. This is normal and expected behavior.
- **Web vs Desktop**: The desktop script offers better performance and more features. The web-based mirror is a convenience option that works without additional software installation.
- **Device Display**: The mirror only shows content when the device's display is actively updating.
- **Input Control**: Virtual D-pad buttons and keyboard shortcuts send commands as text over the same serial connection.
- **Compatibility**: Works with any GhostESP device that has a display (CYD, T-Deck, Cardputer, etc.).