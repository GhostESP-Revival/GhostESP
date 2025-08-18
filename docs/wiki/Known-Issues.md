# Known issues in Ghost ESP


```anything you read here may or may not be entirely accurate or up to date.``` 


1. **Display Color Inversion**: A number of CYDs invert colors by default
2. **SD Card Support**: **Marauder V6**, **Generic ESP32** builds, and **Awok variants** have no SD card support.
3. **GPS Pin Limitations**: Some pins cannot be assigned as GPS input. Notably the Rabbit Labs Yapper board exhibits this behavior.
4. **ESP32-S2 Bluetooth**: ESP32-S2 boards do not support Bluetooth functionality due to lack of hardware.
5. **mDNS Compatibility**: `ghostesp.local` access requires mDNS support on your device/network. Use `192.168.4.1` as alternative.
6. **Web Flasher Cache**: Browser cache clearing may be required when using the web flasher for proper functionality.
7. **HTML Buffer Size**: Flipper Zero App HTML upload is limited to 2048 bytes maximum.
8. **Infrared Support**: IR functionality is limited to LilyGo S3TWatch, ESP32-S3-Cardputer, and LilyGo TEmbed C1101 devices only.
