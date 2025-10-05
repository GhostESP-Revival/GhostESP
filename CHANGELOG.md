# Ghost ESP Changelog

## Revival v1.8

### Added

- Added support for Cardputer ADV
- Added 802.15.4 packet capture (only on C5, C6)
- Added 2 alternate main menu layouts (Grid and List)
- Added glog - a lightweight logging helper
- Added command history with up/down navigation and full in-line cursor editing (left/right, insert, delete, backspace) to the serial console. - @tototo31
- Added 'set/getneopixelbrightness' commands to set or get the max brightness of the neopixels - @tototo31
- Added Kconfig support for a secondary status display
- Added Kconfig support for IO Expander - @Play2BReal
- Added heartbeat-based auto-reconnect for dual communication
- Ghost (asset by @the1anonlypr3) and Game of Life idle animations for status display
- Added ability to set settings via cli - @tototo31
- Add joystick support for keyboard input in terminal view - @tototo31
  
- **PN532 NFC Capability**
  - **NTAG Support (Type 2)**
    - Read NTAG213/215/216 with NDEF parsing
    - Write NTAG213/215/216 from .nfc files
    - Save to Flipper .nfc format (fully compatible)
  - **MIFARE Classic Support (Mini/1K/4K)**
    - Flipper's 1000+ key dictionary attack
    - Magic backdoor detection (Gen1A clones)
    - Parse and display NDEF TLV data
  - **File Management**
    - 'Saved' menu to browse .nfc files and rename/delete them from the UI
    - 'User Keys' view to list `/mnt/ghostesp/nfc/mfc_user_dict.nfc`

### Changed

- Use a fixed-size active-key buffer for keyboard
- Refactor popups to use reusable popup helpers
- Refactor options menu to use reusable options view helpers
- Refactor comm manager to centralize packet handling, add state mutex and handshake timeout, and guard UART driver install
- Update main menu icons to RGB565A8
- Enabled software back buttons made for encoder controls on joystick too
- Joystick builds now use touch keyboard layout with selection highlighting and navigation
- Changed the C5 to use a single display buffer to save memory
- Reduced VFS allocation unit size to 4KB
- Size popup buttons based on what's in them
- Refactored dualcomm logic to be more robust

### Bug Fixes

#### Core

- Fixed intermittent IR learning errors by properly owning and copying received RMT symbol data before passing from ISR to task.
- Fixed memory leak, race conditions and add buffer error handling in pcap.c
- Track SPI host/mount state and only free initialized SPI host on unmount
- Added NMEA handle null-checks
- Flush PCAP header on open and close PCAP on generic stop command
- Miscellaneous fixes and improvements
- Small miscellaneous memory saves
- Fixed RMT channel allocation on C5 to prevent conflicts with IR TX
- Disable duplicate filtering in general BLE scanning


#### Network & Comms

- Ignore self when discovering peers for dual comm
- Prevent crash and spam in EAPOL Logoff attack
- Fixed minor issues with the dns server
- Fixed BLE capture stopping itself after recieving an event
- Added sanity checks to IE parsing to prevent OOB reads
- Accepted HCI packet types now include CMD, ACL, SCO, and ISO

#### Input & UI

- Fix keyboard not using SHIFT correctly and the keyboard view forcing lowercase
- Remove artificial delay in cardputer keyboard task to make more responsive
- Improve and refactor terminal message handling

## Revival v1.7.2

### Added

- Added navigation arrows to the main menu - @tototo31
- Add support for Lolin S3 pro - @tototo31
- Echo backspace, newline, and characters directly to UART and JTAG when supported - @tototo31

### Changed

- WebUI Redesign (Part 1)
- Flush PCAP and CSV data to SD Card on a timer
- Switch to single display buffer on cardputer for extra memory

### Bug Fixes

- Fix not saving or using saved dual comm pins correctly
- Shift main menu down to account for status bar
- Prevent UART conflicts on TDECK by conditionally disabling serial manager and UART driver installation in esp_comm_manager.c - @tototo31

## Revival v1.7.1

- Fix for RGB not properly being handled on devices with no LEDs
- Possible fix for captive portal not being effective on some devices
- Apply existing wroom display memory optimizations to c5
- Fix incorrect usage of mDNS
- Update setcountry command on the C5 to use the official esp_wifi_set_country_code function


## Revival v1.7

### Major Updates

- **Dual ESP32 Communication**
  - Connect two GhostESP devices together.
  - Dedicated WebUI section for managing linked devices.

- **Power Saving**
  - Up to 5x the battery life on compatible boards using the new Power Saving Mode (when compared with v1.6.1).

- **New Board Support**
  - LilyGo TEmbed C1101
  - LilyGo TDeck — @tototo31
  - LilyGo TDisplay S3 Touch
  - AITRIP CYD / ESP2432S028R — @tototo31
  - JCMK DevBoard Pro
  - Rabbit Labs Minion

- **Infrared RX** (only enabled on TEmbed C1101)
  - IR receive and decode support for all protocols supported by Flipper Zero firmware.
  - Ability to Rename, Delete, Add remotes 
  - Easy Learn Mode: **Name buttons automatically**
  
### Added

- Attacks
  - Support for setting an Evil Portal HTML via the Flipper Zero App with a max size of 2048 bytes (as of app v1.4)
  - Added option to select Custom Evil Portal html file from the SD Card - @tototo31

- Display
  - Added 'Never' display timeout setting.
  - Added 'Power Saving' setting which turns off the AP and lowers the CPU frequency on Cardputer and S3TWatch.
  - Display backlight percentage setting for PWM enabled devices - @tototo31
  - Placeholder text for keyboard view - @tototo31
  - Encoder friendly version of the keyboard view
  - Fuel Gauge support with manager and kconf setting (only BQ27220 support initially)
  - Add Vim keybindings for keyboard interactions in various screens - @tototo31
  - Add zebra menu styling and improve vertical alignment - @tototo31
  - Smooth mainmenu animations - @tototo31
  - Keyboard enhancements - @tototo31
  
- Commands
  - Help command reorganised into categories - @tototo31
  - 'chipinfo' command to display chip information
  - 'apenable' command to enable/disable the Access Point
  - 'disconnect' command to disconnect from the current network
  - 'setrgbmode' command to change the RGB mode
  - 'scanarp' command to initiate an ARP scan on the local network
  - 'scanssh [IP]' command to initiate an SSH scan on the target IP
  - '-live' arg for 'scanap' for a non blocking scan that lists APs as they're found

- Misc
  - Add build name config variable for debugging and auto-flash support - @tototo31
  - Try to connect to saved WiFi on boot if available
  - Add 'Stealth' mode for silencing RGB - @tototo31
  - Terminal App to use commands with the keyboard - @tototo31
  - Add memory checks for debugging before initializing AP, BLE, and WiFi managers - @tototo31
  
### Changed

- Display
  - Reuse options screen view for settings screen. Resolves #66 and #65
  - PWM backlight control using ledc on supported devices
  - Moved 'Terminal Color' and 'Third Control' to the Display section in settings
  - Color status bar icons based on their activity
  - S3TWatch: Disable tap-to-wake, use touch interrupt instead.
  - Exiting a view now returns to the previous view instead of the main menu - @tototo31
  - Add CAPSLOCK shift toggle to keyboard view - @tototo31
  - Restyle terminal view - @tototo31

- WebUI
  - Minor style tweaks
  - Update Help tab
  
- Attacks
  - Refactor packet capture
  - Refactor 'scanports' command to be more intuitive and user-friendly

- General
  - Cap displayed WiFi APs to 50 for 'scanap' output
  - Organise BLE menu into hierarchical sub-menus - @tototo31
  - If dualcomm is set to pins used by the serial UART, disable the serial UART

### Bug Fixes

- Display
  - Dynamically size error popup to content and center on screen
  - Reduce FatFS memory usage on S3TWatch and Cardputer
  - Improve battery reading accuracy on Cardputer
  - Fix keyboard view touch detection logic - @tototo31
  - Save settings when exiting the settings view
  - Update LEDs and Status bar when changing from rainbow mode
  - Refresh current item highlight when changing theme

- Commands
  - Add terminal_view_add_text logs to commands missing them
  - Skip pcap flush if mutex is null
  - Fix stop command not stopping GPS task
  - Fix serial going unresponsive by using 'scanap -stop'
  - Small fixes to the process of connecting to a WiFi network
  - Refactor SAE Flood Attack - now requires a password to be set as an argument
  - Handle backspace and DEL properly in serial input
  - Airtag spoofing fixes
  
- General
  - Disable and re-enable ESP comm manager UART around GPS usage to avoid driver conflicts
  - Flush every packet to UART (Flipper) immediately when there's no sd card
  - Miscellaneous refactoring for memory usage
  - Add wifi_manager_stop_beacon function
  - Check if an RMT channel already exists and clean it up before making a new one
  - Randomise BLE Spam MAC addr and add more devices
  - Better EP credential handling
  - Keep one led strip rmt instance
  - Remove legacy led strip rmt driver
  - Tweaks to evil portal captive portal handling

## Revival 1.6.1

- Hotfix for 'BLE stack not ready' on CYD devices.


## Revival v1.6

### TLDR

Support for FlipperZero IR files, Better power consumption, BLE Spam, WPA3 SAE Flood, Deauth multiple APs at once, and more!

### Added

- IR Support (enabled on LilyGo S3TWatch and Cardputer)
  - Uses FlipperZero formatted IR files stored in sdcard: /ghostesp/infrared/remotes or /ghostesp/infrared/universals
  - Universal Library IR Transmit
  - Signals File IR Transmit
  - IR Protocol Encoders:
    - NEC
    - Kaseikyo
    - Pioneer
    - RCA
    - Samsung
    - SIRC
    - RC5
    - RC6

- BLE Spam (not supported on ESP32S2)
  - Apple
  - Microsoft
  - Samsung
  - Google

- Display
  - Added keyboard view
  - Connect to WiFi command with keyboard view
  - Add S3TWatch virtual storage (4MB) acessable through webUI

- Attacks
  - EAPOL Logoff Attack
  - SAE Flood Attack (WPA3 only)
  - Probe request listen

- Commands
  - 'webauth on/off' command to enable/disable webui authentication

- Cardputer
  - Add keyboard event handling functionality - @tototo31
  - Enable Cardputer's LED in config - @tototo31
  - Get cardputer keys working - @tototo31

### Changed

- Display
  - Removed touch controls from settings menu on non-touch devices - @tototo31
  - Refactor wifi menu into hierarchical sub-menus
  - Enable ESPIDF Power Management freq scaling on Cardputer, S3TWatch 2.4Inch CYD, and Phantom
  - First item is no longer highlighted on menu lists for touch devices - @tototo31

- Cardputer
  - Use backtick key to return to main menu

- Attacks
  - Deauth Attack now supports targeting multiple APs

- Commands
  - Allow selection of multiple APs (eg. select -a 2,3,4)
  - AP list now includes wifi channel
  
- WebUI
  - Refactor file explorer to be more user friendly

- Power
  - Set min PM freq to 20MHz instead of 80MHz

- General
  - replaced several large static allocations with dynamic heap allocations

### Bug Fixes

- Display
  - Fix blank bootup screen on cardputer and show flappy ghost icon out of necessity - @tototo31
  - Fix status bar icons - @tototo31
  - Added auto-cleanup of old terminal messages when text length exceeds threshold

- WiFi
  - preserve STA mode in ap_manager init and start_services

- Cardputer
  - Get cardputer battery status working - @tototo31
  - Ignore the key press that wakes the display from sleep

- General
  - Fix SD Card init on CYD devices - @tototo31
  - Capped stored wifi scan results at 100 and auto-truncate lists to prevent memory bloat and crashes

- WebUI
  - Fix file explorer not opening folders, erroring on upload.


## Revival v1.5.1

### Added

- add default sd pins for default configs
- 'setcountry' command to set country code for esp32c5

### Changed

- update pineap detection to use dualband channels with ESP32C5
- backlight dimming fix for cardputer - @tototo31
- handle button presses correctly on cardputer - @tototo31
- fix inputs not waking screen on cardputer - @tototo31
- bump M5GFX to 0.2.9
- wrap menu items once you hit the top or bottom of the screen - @tototo31


### Bug Fixes

- added warning that webui will disconnect you from the web interface when running wifi commands
- disable menu items in main menu if the device does not support them - @tototo31
- hide touch interface on non-touch devices - @tototo31
- fix cardputer settings menu crash
- fix rabbit labs' phantom n cyd build boot issues

## Revival v1.5

### Added

- Support for ESP32C5 (some channels may not work as expected for now)
- FlipperZero Devboard w/JCMK GPS module config file - #11 - @tototo31

- Attacks
  - Deauthentication & DoS

    - Added support for direct station deauthentication
    - Added DHCP-Starve attack

  - Spoofing & Tracking

    - Added support for AirTag selection and spoofing
    - Added support for selecting and tracking Flipper Zero rssi

  - Beacon Management

    - Custom beacon SSID list management and spam

- Commands
  - Added station selection capability to existing select command
  - Added a timezone command to set the timezone with a POSIX TZ string
  - Enable passing custom DIAL device name via CLI argument

- Display
  - Add back button to options screen bottom center to return to main menu
  - Added swipe handling for the main menu and app gallery views
  - Add vertical swipe navigation for scrolling of menu items (requires a capacitive touch screen)
  - Added station scanning and the new station options to the wifi options screen
  - Added simple digital clock view
  - Settings menu (with old screen controls as an option)
  - Configurable main menu themes (15 different ones to choose from)
  - Added "Connect to saved WiFi" command
  - Configurable terminal text color
  - Added "List APs" command
  - Added "Invert Colors" option to settings menu

### Changed

- Attacks
  - If station data is available, directly deauth known stations of the AP selected for deauth
  - Deauth task now deauths on each AP's primary channel
  - Station scan now uses discovered AP channels for scanning
  - ESP32C5 shows band in AP scan results
  - ESP32C6 and ESP32C5 show Security and if PMF is required in AP scan results
  - If company is unknown, it won't be shown in AP scan results
  
- Display

  - Performance Optimizations

    - Refactored options screen to use lv_list instead of a custom flex container to improve performance
    - Replaced single lv_textarea in terminal view with scrollable lv_page and per-line lv_label children to improve performance
    - Optimize terminal screen by batching text additions

  - UI & UX Adjustments

    - Offset terminal page vertically by status bar height and adjust its height accordingly.
    - Remove index reset in main_menu_create to maintain selection across view switches
    - Default display timeout is now 30 seconds instead of 10
    - Status bar now updates every second instead of when views change
    - Removed rounding on the status bar
    - Changed bootup icon
    - Removed default shadow/border from back buttons
    - Changed option menu item color to be black and white
    - Added text to the splash screen and removed animation

- Commands
  - List stations with sanitized ascii and numeric index
  - Label APs with blank SSID fields as "Hidden"
  - Make congestion command ASCII-only for compatibility
  - Change display EP option to start default EP with a default SSID "FreeWiFi"
  - Update congestion to work with dualband channels
  - Make GPS formatting renderable on devices w/ a limited font - #13 - @tototo31 

- Power
  - Suspend LVGL, status bar update timer, and misc tasks when backlight is off
  - Use wifi power saving mode if no client is connected
  - Poll touch 5x slower when backlight off
  - Enabled light-sleep idle and frequency scaling

- RGB
  - Refactored rgb_manager_set_color to use is_separate_pins flag instead of compile-time directives

- WebUI
  - Changed color theme to black and white
  - Improve loading 

### Bug Fixes

- General
  - Fixed NVS persistence issues for AP credentials by ensuring a single shared NVS handle and settings instance.
  - Addressed unaligned memory access warning in ICMP ping logic by using an aligned buffer for checksum calculation.
  - Restart mDNS service with AP
  
- Display
  - Fixed an issue where an option would be duplicated and freeze the device.
  - Skip first touch event while backlight is dimmed so tap only wakes the screen without registering input
  - Fixed an issue where the numpad would register 2 inputs for a single tap.
  - Fixed screen timeout only resetting on the first wake-up tap
  - Add tap to wake functionality to non battery config models
  - Keep app gallery back button on top of icons

- Power
  - Fixed an issue where the device was reporting that it was not charging when it was.

- RGB
  - Persist RGB pin settings to NVS and auto-init from saved config, closes [jaylikesbunda/Ghost_ESP#5](https://github.com/jaylikesbunda/Ghost_ESP/issues/5)

- GPS
  - Initialize GPS quality data and zero-init wardriving entries to prevent crash in wardriving mode
  - Don't check for csv file before flushing buffer over UART
  - Actually open a CSV file for wardriving when an SD card is present
  - Fix CSV file timestamp to reflect GPS date/time on SD card close
  - Reset GPS timeout flag on initialization
  - Assign gps RX pin based on CONFIG if not explicitly set by the user - #12 - @tototo31

## Revival v1.4.9

### ❤️ New Stuff

- Basic changeable SD Card pin out through webUI and serial command line (requires existing sd support to be enabled in your board's build)
- Added default evil portal html directly in the firmware (credit to @breaching and @bigbrodude6119 for the tiny but great html file)
- Basic congestion command to quickly see channel usage
- Added scanall command to scan aps and stations together

### 🤏 Tweaks and Improvements

- Simplified the evil-portal command line arguments.
  - eg. ```startportal <google.html> (or <default>) <EVILAP> <PSK>```
- Save credentials in flash when using connect command
- Captive portal now supports Android devices
- Simplified the evil-portal command line arguments.
- set LWIP_MAX_SOCKETS to 16 instead of 10
- Save captured evil-portal credentials to SD card if available
- Added support for scanning aps for a specific amount of time eg. ```scanap 10```
- Connect command now uses saved credentials from flash when no arguments are provided
- Added channel hopping to station scan
- Include BSSID in scanap output

### 🐛 Bug Fixes

- Use "GhostNet" as fallback default webUI credentials if G_Settings fields are not set or invalid
- Fix webUI not using evilportal command line arguments
- Fix evil‑portal local file serving 
- Correctly parse station/AP MACs and ignore broadcast/multicast in Station Scan
- Fix station scanning using wrong frame bit fields and offsetting the mac addresses

----------------------

OK, we back. - 22 April 2025

-----------------------

Rest in Peace, GhostESP - 22 April 2025

______________________

## 1.4.7

### ❤️ New Stuff

General:

- Added WebUI "Terminal" for sending commands and receiving logs - @jaylikesbunda

Attacks:

- Added packet rate logging to deauth attacks with 5s intervals - @jaylikesbunda

Lighting:

- Added 'rgbmode' command to control the RGB LEDs directly with support for color and mode args- @jaylikesbunda
- Added new 'strobe' effect for RGB LEDs - @jaylikesbunda
- Added 'setrgbpins' command accessible through serial and webUI to set the RGB LED pins - @jaylikesbunda


### 🐛 Bug Fixes

- Immediate reconfiguration in apcred to bypass NVS dependency issues - @jaylikesbunda
- Disabled wifi_iram_opt for wroom models - @jaylikesbunda
- Fix station scanning not listing anything - @jaylikesbunda
- Connect command now supports SSID and PSK with spaces and special characters - @jaylikesbunda

### 🤏 Tweaks and Improvements

- General:
  - Added extra NVS recovery attempts - @jaylikesbunda
  - Cleaned up callbacks.c to reduce DIRAM usage - @jaylikesbunda
  - Removed some redundant checks to cleanup compiler warnings - @jaylikesbunda
  - Removed a bunch of dupe logs and reworded some - @jaylikesbunda
  - Updated police siren effect to use sine-based easing. - @jaylikesbunda
  - Improved WiFi connection output and connection state management - @jaylikesbunda
  - Optimised the WebUI to be smaller and faster to load - @jaylikesbunda

- Display Specific:
  - Update sdkconfig.CYD2USB2.4Inch_C_Varient config - @Spooks4576
  - Removed main menu icon shadow - @jaylikesbunda
  - Removed both options screen borders - @jaylikesbunda
  - Improved status bar containers - @jaylikesbunda
  - Tweaked terminal scrolling logic to be slightly more efficient - @jaylikesbunda
  - Added Reset AP Credentials as a display option - @jaylikesbunda


## 1.4.6

### ❤️ New Features

- Added Local Network Port Scanning - @Spooks4576
- Added support for New CYD Model (2432S024C) - @Spooks4576
- Added WiFi Pineapple/Evil Twin detection - @jaylikesbunda
- Added 'apcred' command to change or reset GhostNet AP credentials - @jaylikesbunda

### 🐛 Bug Fixes

- Fixed BLE Crash on some devices! - @Spooks4576
- Remove Incorrect PCAP log spam message - @jaylikesbunda
- retry deauth channel switch + vtaskdelays - @jaylikesbunda
- Resolve issues with JC3248W535EN devices #116 - @i-am-shodan, @jaylikesbunda

### 🤏 Tweaks and Improvements

- Overall Log Cleanup - @jaylikesbunda
- Added a IFDEF for Larger Display Buffers On Non ESP32 Devices - @Spooks4576
- Revised 'gpsinfo' logs to be more helpful and consistent - @jaylikesbunda
- Added logs to tell if GPS module is connected correctly- @jaylikesbunda
- Added RGB Pulse for AirTag and Card Skimmer detection - @jaylikesbunda
- Miscellaneous fixes and improvements - @Spooks4576, @jaylikesbunda
- Clang-Format main and include folders for better code readability - @jaylikesbunda

## 1.4.5

### 🛠️ Core Improvements

- Added starting logs to capture commands - @jaylikesbunda
- Improved WiFi connection logic - @jaylikesbunda
- Added support for variable display timeout on TWatch S3 - @jaylikesbunda
- Revise stop command callbacks to be more consistent - @jaylikesbunda, @Spooks4576

### 🌐 Network Features

- Enhanced Deauth Attack with bidirectional frames, proper 802.11 sequencing, and rate limiting (thank you @SpacehuhnTech for amazing reference code) - @jaylikesbunda  
- Added BLE Packet Capture support - @jaylikesbunda  
- Added BLE Wardriving - @jaylikesbunda  
- Added support for detecting and capturing packets from card skimmers - @jaylikesbunda  
- Added "gpsinfo" command to retrieve and display GPS information - @jaylikesbunda

### 🖥️ Interface & UI

- Added more terminal view logs - @jaylikesbunda, @Spooks4576  
- Better access for shared lvgl thread for panels where other work needs to be performed - @i-am-shodan
- Revised the WebUI styling to be more consistent with GhostESP.net - @jaylikesbunda
- Terminal View scrolling improvements - @jaylikesbunda
- Terminal_View_Add_Text queue system for adding text to the terminal view - @jaylikesbunda
- Revise options screen styling - @jaylikesbunda

### 🐛 Bug Fixes

- Fix GhostNet not coming back after stopping beacon - @Spooks4576
- Fixed GPS buffer overflow issue that could cause logging to stop - @jaylikesbunda
- Improved UART buffer handling to prevent task crashes in terminal view - @jaylikesbunda
- Terminal View trunication and cleanup to prevent overflow - @jaylikesbunda
- Fix and revise station scan command - @Spooks4576

### 🔧 Other Improvements

- Pulse LEDs Orange when Flipper is detected - @jaylikesbunda
- Refine DNS handling to more consistently handle redirects - @jaylikesbunda
- Removed Wi-Fi warnings and color codes for cleaner logs - @jaylikesbunda
- Miscellaneous fixes and improvements - @jaylikesbunda, @Spooks4576  
- WebUI fixes for better functionality - @Spooks4576

### 📦 External Updates

- New <https://ghostesp.net> website! - @jaylikesbunda
- Ghost ESP Flipper App v1.1.8 - @jaylikesbunda
- Cleanup README.md - @jaylikesbunda

