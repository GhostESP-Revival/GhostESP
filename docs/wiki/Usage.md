# Touch Screen Usage

GhostESP firmware includes touch support, enabling users to interact directly with on-screen elements. Here's how to navigate the different menus and screens effectively.


```anything you read here may or may not be entirely accurate or up to date.``` 

## Input Handling

Ghost ESP implements a custom input handling system that supports multiple input methods. While the firmware uses LVGL for UI rendering, it processes all input events through its own custom event queue system rather than LVGL's built-in input drivers.

### Main Menu Navigation

In the main menu, simply tap on an item directly to select it. The interface supports both touch and hardware input methods (encoder, joystick, keyboard) depending on your board configuration.

### List Menus (Wi-Fi, Bluetooth, Infrared)

For list-based menus, you can:

- **Tap directly on items** to select them
- **Use hardware controls** (encoder/joystick) to navigate up/down and press to select
- **Scroll through long lists** using touch or hardware controls

### Submenus and Actions

- When you select a menu item that opens a submenu, the new menu will appear with appropriate navigation options
- For actions that require text input, a keyboard screen will appear. Use touch or hardware keyboard to enter text, then select "Done" to submit
- Some menus include management options like "Rename", "Add", or "Delete" for file operations

### Status Bar

A status bar at the top of the screen shows:
- Current menu/view name
- SD card status (mounted/unmounted)
- Wi-Fi status (reflects actual AP state)
- Bluetooth status
- Battery percentage, charging status and power saving mode (on devices with support for reading battery data)

### Terminal and Output

The terminal view displays command output and logs. Navigation depends on your board:
- **Touch boards**: Tap to interact or return to previous menu
- **Keyboard boards**: Use keyboard shortcuts and commands directly

> **Note**: Touch responsiveness varies by board configuration. Some boards use encoder/joystick input instead of or in addition to touch.

## Terminal App

The terminal app provides direct command-line access to Ghost ESP functionality.

### Accessing Terminal App

1. Navigate to the main menu
2. Select "Terminal" or "Terminal App"
3. The terminal view will open with a command prompt

### Terminal Features

- **Real-time Output**: Command results display immediately
- **Command Queue**: Messages are queued and processed efficiently
- **Hardware Input Support**: Works with keyboard and touch input
- **Scrolling**: Navigate through command history and output

### Using the Terminal

**For Keyboard-Equipped Boards (Cardputer, etc.):**
- Type commands directly using the physical keyboard
- Press Enter to execute commands
- Use keyboard shortcuts for navigation

**Common Operations:**
- View real-time logs from Wi-Fi operations, attacks, and system events
- Execute commands through the serial interface
- Monitor system status and debug information

## Power Management

### Display Timeout Settings

Configurable display timeout options are available through the settings:

- **Never**: Display stays on indefinitely (UINT32_MAX)
- **30 seconds**: Display dims/turns off after 30 seconds of inactivity
- **1 minute**: Display dims/turns off after 1 minute of inactivity
- **5 minutes**: Display dims/turns off after 5 minutes of inactivity
- **Custom timeouts**: Additional timeout values may be available

### Power Saving Mode

Power saving features include:

- **AP Management**: Access Point can be disabled to save power
- **CPU Frequency Scaling**: Dynamic frequency adjustment based on power saving settings
- **Display Brightness Control**: Automatic brightness adjustment and dimming
- **Rainbow Mode Management**: RGB effects are properly managed to prevent resource conflicts

### Power Saving Features

- **Automatic AP Disable**: When power saving is enabled, the AP is automatically disabled
- **Status Bar Updates**: Wi-Fi icon accurately reflects AP state and power saving mode
- **Battery Monitoring**: Real-time battery percentage and charging status (on supported boards)
- **Graceful Resource Management**: Proper cleanup of power-intensive tasks like RGB effects

### Enabling Power Management

1. Navigate to Settings menu
2. Look for Display or Power options
3. Configure timeout settings
4. Enable power saving mode if available for your board

> **Note**: Power saving effectiveness varies by board type and configuration. Some features may not be available on all hardware variants.

---

## Quick Tips

- **Evil Portal Operations**:  
  Navigate to Wi-Fi menu → Evil Portal for captive portal options. Configuration and management available through the interface.
 
- **Chameleon Ultra / GhostNet AP**:  
  Starting a Chameleon Ultra connection from `NFC → Chameleon Ultra` in the display ui temporarily disables the `GhostNet` AP so BLE has priority. When the session ends, the firmware restores the AP automatically after a short delay.

- **Karma Attack Operations**:  
  Navigate to Wi-Fi menu → Attacks → Start Karma Attack for automatic SSID learning, or Start Karma Attack (Custom SSIDs) to specify target networks manually.
  
- **Infrared Operations**:  
  Use "Learn Remote" to capture IR signals, "Easy Learn" mode for auto button naming, and "Add Signal" to append signals to existing remotes.
  
- **Navigation**:  
  - Direct touch/tap on menu items to select
  - Use encoder/joystick controls where available
  - Hardware back button or on-screen navigation to return to previous menus
  
- **File Management**:  
  - Rename, add, or delete remotes through the management interface
  - Signals are saved in standard .ir format with automatic protocol detection
  
- **Terminal Access**:  
  - Use Terminal app for command-line access and real-time system logs
  - Keyboard input supported on compatible boards
  
- **Power Management**:  
  - Configure display timeouts and power saving through Settings
  - Monitor battery status in the status bar
  
- **RGB Control**:  
  - Multiple RGB modes available (Normal, Stealth, Rainbow)
  - Proper resource management prevents device freezes

---

For a full list of features and commands, see the [Features](Features.md) and [Commands](Commands.md) guides.
