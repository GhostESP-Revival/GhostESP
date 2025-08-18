# About GhostESP

GhostESP: Revival is an open source firmware for ESP32 microcontrollers.

## Supported Hardware

The following ESP32 models are officially supported:

### Display-Enabled Boards

- **CYD (Cheap Yellow Display) variants**
  - **CYD2USB** (USB-C)
  - **CYDMicroUSB**
  - **CYDDualUSB** (Both ports)
  - **AITRIP CYD** (ESP32-2432S028R)
  - **Compatibility Note**: Stable only with **ESP32-2432S028 (2.8 inches)**. Incompatible with **ESP32-2432S024 (2.4 inches)**.
- **Waveshare LCD (7-inch)**: 800x480 resolution, using ESP32-S3
- **Crowtech LCD (7-inch)**: 800x480 resolution, using ESP32-S3
- **ESP32-S3-Cardputer**: Compact design with a built-in display and keyboard
- **MarauderV6 & AwokDual**: 240x320 touchscreen models
- **AwokMini**: 128x128 display with joystick for navigation
- **Awok ESP32 v5**
- **LilyGo T-Watch S3**
- **LilyGo TEmbed C1101**

### Generic Boards

- **ESP32 Wroom**: Standard model
- **ESP32-S2**
- **ESP32-C3**
- **ESP32-S3**
- **ESP32-C6**: Partial support with limited feature availability
- **ESP32-C5**: Partial support with limited feature availability

## Important Considerations

- **Performance** varies across ESP32 models and may impact certain features.
- **Hardware-specific limitations**: Certain features, like SD card support and touchscreen responsiveness, depend on board compatibility.
- **Firmware updates**: Beta development may include experimental features.

## Version Status

- **Current version**: v1.7dev (Revival)
- **Framework**: ESP-IDF

### Recent Feature Additions

- **Dual ESP32 communication** with WebUI control
- **Power saving mode** for up to 5X battery life on supported devices
- **Infrared (IR) support** with FlipperZero .ir file compatibility
- **Support for ESP32C5**: With Limitations
- **DHCP-Starve attack**
- **New UI** for a better user experience
- **AirTag selection and spoofing**
- **Direct station deauthentication**
- **BLE Wardriving**: with GPS Logging
- **Terminal App** for keyboard-based command input
- **Fuel Gauge support** (BQ27220 initially)

### Features Under Development

- **Additional screen support optimizations** to improve compatibility
- **Enhanced SD card compatibility** for more board models
- **BLE feature improvements** for device discovery and interaction
- **Touch screen calibration refinements** to enhance touch accuracy

## Acknowledgments

GhostESP is based on the work of several open-source projects and developers that may or may not be listed here:

- **JustCallMeKoKo**: ESP32 development foundations
- **thibauts**: CastV2 protocol insights
- **MarcoLucidi01**: DIAL protocol integration
- Community contributors for board support
- **FlipperZero**: IR support
- **Spooky**: Original GhostESP developer
- **Tototo31**: Bug-Fixes, Feature Additions and Board Support

## Project Goals

GhostESP aims to provide:

- Comprehensive wireless testing capabilities
- A user-friendly interface
- Stable and reliable performance
- Educational value for security researchers
- Regular feature updates and improvements
