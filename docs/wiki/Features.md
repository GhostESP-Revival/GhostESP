# Features


```anything you read here may or may not be entirely accurate or up to date.``` 


GhostESP comes packed with various features for wireless network exploration and testing.

## WiFi Capabilities

### Network Analysis

- **AP Scanning**
  - Detect all nearby WiFi networks
  - Detailed visibility into wireless environments
  - Channel congestion chart
  - Combined AP & Station scan (`scanall`)

- **Station Scanning**
  - Identify connected WiFi clients
  - Monitor active devices on networks

- **Probe Request Listening**
  - Listen for and log probe requests from devices
  - Channel hopping or fixed channel

### Network Interaction

- **Beacon Spam**
  - Deploy customizable SSID beacons
  - Multiple operation modes: random, Rickroll, AP list, custom SSID
  - Beacon spam list management (add, remove, clear, show, spamlist)

- **Deauthentication Attacks**
  - Disconnect clients from WiFi networks
  - For testing network security
  - EAPOL logoff attack
  - SAE handshake flood (ESP32-C5/C6 only)
  - DHCP starvation attack

- **Karma Attack**
  - Responds to probe requests with fake access points
  - Automatically learns SSIDs from nearby devices
  - Creates fake networks with discovered SSIDs
  - Integrates with evil portal for captive portal attacks
  - SSID rotation every 5 seconds for maximum effectiveness
  - Manual SSID specification for targeted attacks

- **Evil Portal**
  - Custom SSID and domain setup
  - Start built-in or custom HTML captive portals
  - List available portals on SD card
  - Prompt for SSID and optional PSK
  - Stop portal at any time
  - HTML upload via Flipper Zero App (max 2048 bytes)

### Data Collection

- **WiFi Capture**
  - Capture probe requests
  - Record beacon frames
  - Log deauthentication packets
  - Raw wireless data collection
  - EAPOL/handshake, WPS, Pwnagotchi, and PineAP detection
  - Save to SD card or Flipper
  - BLE packet capture (non-S2)

## BLE Functions

### Scanning Capabilities

- **General BLE Scanning**
  - Detect BLE devices
  - Monitor BLE advertisements
  - BLE Wardriving with GPS Logging

- **Specialized Detection**
  - AirTag detection mode
  - Flipper Zero detection and RSSI tracking
  - BLE skimmer detection

### BLE Attacks

- **BLE Spam**
  - Apple, Microsoft, Samsung, Google, and random BLE spam modes
  - Stop BLE spam at any time

## Chameleon Ultra Integration

### NFC/RFID Research Tool Support

- **Bluetooth Connectivity**
  - Wireless control of Chameleon Ultra device
  - Real-time status monitoring and battery level checking
  - Remote mode switching (Reader/Emulator)

### HF (13.56MHz) Capabilities

- **MIFARE Classic Analysis**
  - Automated comprehensive analysis
  - Basic card detection and UID reading
  - Professional forensic reporting

- **NTAG Support**
  - Intelligent NTAG213/215/216 detection
  - Password-protected card analysis
  - Professional security assessment reports
  - Multi-method detection algorithms

- **General HF Operations**
  - ISO14443 Type A/B card scanning
  - Complete card data dumping with authentication summary
  - Slot management (8 emulation slots)

### LF (125KHz) Capabilities

- **Low Frequency Scanning**
  - EM410X tag detection and reading
  - HID Proximity card support
  - Multi-format LF scanning

### Advanced Security Features

- **Professional Security Analysis**
  - Comprehensive card analysis and reporting
  - Compatible with standard security research tools

- **Data Management**
  - Automatic file organization with UID-based naming
  - Timestamped forensic reports
  - SD card storage in `/mnt/ghostesp/chameleon/`
  - Custom filename support for organized research

### Memory Safety & Stability

- **Robust Operation**
  - Memory monitoring and validation (20KB minimum requirement)
  - Crash protection with graceful error handling
  - Automatic connection management
  - Professional error reporting

## Infrared (IR) Functions

### IR Receive Support

- **Easy Learn Mode**<sup>™</sup> Auto button naming (it's not a real trademark)

### IR Transmit Support

- **FlipperZero IR File Compatibility**
  - Use FlipperZero formatted IR files
  - Store files in `/ghostesp/infrared/remotes` or `/ghostesp/infrared/universals` on SD card
  - Universal Library IR Transmit
  - Signals File IR Transmit

### IR Protocol Support

- **Supported Protocols**:
  - NEC, NECext
  - Kaseikyo
  - Pioneer
  - RCA
  - Samsung
  - SIRC, SIRC15, SIRC20
  - RC5
  - RC6

> **Note**: IR functionality is available on LilyGo S3TWatch, ESP32-S3-Cardputer, and LilyGo TEmbed C1101 devices only as of firmware v1.7.

## Device Controls

### RGB LED Modes

- Stealth
- Normal
- Rainbow
- Police
- Strobe
- Static color (red, green, blue, yellow, purple, cyan, orange, white, pink)
- Pin configuration for single-pin or separate RGB

### AP Controls

- Change or reset GhostNet AP credentials
- Enable/disable AP across reboots

## Additional Features

### Media Device Integration

- DIAL protocol support
- Chromecast V2 compatibility
- Roku device interaction
- PowerPrinter: Print custom text to network printers

### GPS Features

- Wardriving with GPS logging (WiFi and BLE)
- Live GPS info display

### Port Scanning

- Scan local subnet or specific IP
- Scan common, all, or custom port ranges

### System & Utilities

- Web UI authentication toggle
- Set Wi-Fi country code (ESP32-C5)
- Set timezone for clock view
- Chip and memory info
- Reboot, stop all operations, crash for debugging
- Power saving mode for extended battery life
- Fuel gauge support (BQ27220 initially)

### SD Card Management

- Show and configure SD card pinout (MMC/SPI)
- Save SD config to card

### Dual ESP32 Communication

- Connect two ESP32 devices with UART
- Remote control through WebUI
- Automatic device discovery
- Send commands between devices
- Coordinated attacks and operations

### Display & Interface

- Touch screen navigation
- Terminal App for keyboard input
- Power saving display timeout options
- PWM backlight control on supported devices

---

> **Note:** Some features require specific hardware (e.g., SD card, GPS, BLE, Flipper Zero, etc.).  
> For a full list of commands and usage, see the [Commands Guide](Commands.md).
