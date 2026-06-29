---
title: "Mouse Jiggler"
description: "Keep your PC awake with periodic mouse movements"
weight: 30
---

Mouse Jiggler moves the mouse cursor back and forth periodically, preventing your computer from going idle or triggering screen lockers.

## How It Works

When activated, the ESP32-S3 presents itself as a USB HID mouse to the host PC. A background task sends alternating left/right mouse movements at half-second intervals. The movement is small (8px) so it won't interfere with your work but is large enough to reliably keep the host awake.

## Usage

### From the Display Menu

1. Navigate to **BadUSB**
2. Select **Mouse Jiggler**
3. The device installs as a USB mouse and begins jiggling
4. Select **Cancel** on the popup to stop

### Via CLI

Start:
```
badusb jiggle_start
```

Stop:
```
badusb jiggle_stop
```

### Remote (Dual Comm)

From a C5 display controller paired with an S3 over GhostLink:

1. Navigate to **BadUSB → Mouse Jiggler**
2. The C5 sends `badusb jiggle_start` to the S3
3. The S3 activates USB HID mouse mode and jiggles
4. Status updates are shown on the C5 display

## Notes

- **USB required**: The device must be plugged into the host PC before starting. If VSENSE is configured, it waits for USB connection automatically.
- **Mutually exclusive**: Mouse Jiggler cannot run at the same time as BadUSB script execution or USB Keyboard mode — they share the same USB HID interface.
- **Stops on cancel**: Selecting Cancel on the display or running `badusb stop` halts the jiggler and uninstalls the USB driver.
