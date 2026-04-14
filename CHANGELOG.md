# Ghost ESP Changelog

## Revival v1.9.8

### Added
- Added New 'Ghostchi' App - assets by pr3!
- Added Wi-Fi multi-select flows for APs and stations in the Scan & Select menu so multiple targets can be selected from the paged detail lists before running attacks/actions
- Added GTK abuse testing flow for checking client isolation bypass behavior after joining a target Wi-Fi network
- Added beacon_spam_broadcast_karma function that uses real AP MAC so BSSID matches probe responses
- Added missing encoder controls to the detail view
- Added option to change background shade of options
- Added option to enable rounded menu items and set as default on
- Added a random ascii art boot banner to the serial log - @tototo31

### Changed
- Migrated project to ESP-IDF v6.0
- The Wired Hatter's Banshee C5 internal memory optimisations
- Improved The Wired Hatter's fuel gauge handling
- Karma now skips channel hopping when AP has connected clients
- Moved lvgl tick task back out of psram to resolve wd triggering on setup wizard
- Optimised LVGL memory footprint across all configs by disabling unused components
- Increased CYD display buffer for significantly smoother rendering
- Rename 'Normal' main menu layout to 'Carousel'
- Restyled touch control bar to be more compact and clean
- Wi-Fi capture commands now lock to the selected AP's channel when one AP is selected, or hop only across the selected AP channels when multiple APs are selected
- Removed unnecessary channel list rebuild every 100ms in PineAP detection hop timer
- Improved code readability by replacing comma operator with separate statement in reset_setting_value
- Replaced unsafe strcpy calls with snprintf in portal and AP credential commands
- Removed dead _WIN32 code path in file upload handler (never compiled on ESP32)
- Downgraded Digest auth header logging from INFO to DEBUG to avoid leaking credentials in logs
- Cleaned up duplicate includes in ap_manager.c and wifi_manager.c

### Fixed
- Miscellaneous stability fixes and code cleanup across the infrared, terminal, number pad, popup, options, badusb, and clock views
- Fixed crash on The Wired Hatter's Banshee S3 chip when enabling USB Keyboard caused by running out of input interrupts
- Potentially fixed issue where the C5 on the Banshee would run out of DMA
- Fixed channel_enabled flag not being reset when RMT operations fail
- Fixed crash when opening WebUI File Manager and improved styling
- Fixed airtag tracking not working
- Fixed TEmbedCC1101 fuel gague init
- Fixed WPS detection buffer overflow when exceeding MAX_WPS_NETWORKS limit
- Fixed out-of-bounds read in EAPOL detection on short packets
- Fixed EAPOL handshake M4 frames being misclassified as M2 by checking the Secure bit
- Fixed wardrive heartbeat timer using wrong interval (5s instead of 10s)
- Misc fixes: added packet validation to PWN scan callback, fixed symbol visibility on compare_bssid, added bounds checks to channel split loops
- Fixed capture command silently ignoring invalid capture types
- Fixed AP credentials command not validating SSID length (could overflow buffer)
- Fixed channel congestion command not checking malloc return values (potential crash on OOM)
- Fixed settime command rejecting valid Unix timestamp of 0 (Unix epoch)
- Fixed path traversal vulnerability in WebUI SD card file download and delete handlers (now enforces /mnt prefix)
- Fixed unbounded malloc in WebUI settings API handler (now capped at 4KB to prevent OOM crashes)
- Fixed missing HTTP error responses in settings API handler (previously left client hanging on parse errors)
- Fixed HCI buffer overflow in BLE PCAP callback when advertisement data exceeds 243 bytes
- Fixed snprintf size mismatch in WebUI file upload handler that could write past allocation
- Reset BLE spam detector state (company_id + counter) consistently on stop

## Revival v1.9.7

### Added
- MIC RGB visualizer adapted from SensoryBridge by Connor Nishijima (https://github.com/connornishijima/SensoryBridge)
- Ethernet ARP poisoning attack with bidirectional spoofing, ICMP ping sweep, passive host discovery, DNS interception using network's actual DNS server, and IP packet forwarding

### Changed
- Reduced WiFi RX/TX buffer counts and LWIP pool sizes across all configs to lower memory usage
- Standardised FATFS sector size to 512, disabled per-file cache, and enabled dynamic buffers across all configs for better SD stability
- Disabled mDNS for Ethernet interface to prevent crashes
- Disabled SD SPI on somethingsomething2
- Upped GhostLink baudrate between The Wired Hatter's Banshee chips to 460800
- GhostLink ethernet uses new detail view instead of terminal
- Moved Flipper, Airtag and Skimmer display options to New 'Detect Devices' menu using detail view like 'Scan APs'

### Fixed
- Fixed beacon spam not broadcasting any SSIDs due to race condition where task flag was set after task creation
- Added back missing RGB pulse for flipper and airtag detection
- Fixed boot crash loop on devices without RTC hardware by replacing ESP_ERROR_CHECK with graceful error handling in RTC driver

## Revival v1.9.6

### Added
- Added Channel Switch attack

### Changed
- Optimised CYD WiFi config values to save memory
- Enabled dynamic fatfs buffers for all CYD configs to save memory
- Optimised the terminal view to use half the memory with same functionality
- Offloaded misc memory allocations to PSRAM if enabled to lower internal pressure
- Universal remote transmit will cache signal batches into PSRAM if available to avoid suspending input processing on JIT mount configs

### Fixed
- Fixed FreeRTOS xTaskCreateStatic stack size bug, saving significant memory
- Fixed SD/SPI regression potentially causing some devices to not function properly

## Revival v1.9.5

### Added
- Added auto saving of coredumps and cli commands for debugging - @tototo31
- Add NRF24 native + ghostlink support for The Wired Hatter's Banshee with a frequency analyzer
- Add Wigle EncodedForUseToken support and SD config loader - @Hamspiced

### Changed
- Wardriving dedupe now includes APs when RSSI differs lower or higher for better trilateration support
- Wardriving screen now shows `GPS Stale` when GPS data stops refreshing
- The Wired Hatter's Banshee GPS routing now uses S3 UART GPS on the GhostLink peer streamed over GhostLink to the C5 primary instead of relying on C5 soft GPS RX which has reliability issues
- Refactored WiFi options for better UX
  - Removed individual select and track options
  - Added new details view for listing APs, Stations and both combined
- Station scan now parses 802.11 frame control (type/subtype/DS bits) for better validation
- Station scan now captures data frames in addition to management frames for better detection
- Improved new Wi-Fi details view on Cardputer-sized screens
- Rewrote 'Visualiser' and desktop streamer (formerly known as Rave)

### Fixed
- Fixed new soft GPS parser losing first bytes of sentences by implementing double-buffering to eliminate re-arm gap
- Fixed soft GPS receive getting stuck after an RMT re-arm failure by adding retry recovery and re-arm telemetry counters
- Fixed wardriving writing stale last-known coordinates when GPS fix flags remained set but no fresh `GPS_UPDATE` events were arriving
- Fixed watchdog timeout during CSV UART streaming by releasing mutex before slow writes
- Potentially fixed watchdog timeout wardriving crash when writing to SD by making CSV buffer flush asynchronous
- Potentially fixed wardriving crash caused by O(n) linear probing in dedupe table
- Fixed BLE stop/exit races by stopping spam, spoofing, and scan modules before NimBLE deinit and waiting for the BLE spam task to exit cleanly
- Fixed repeated saved-WiFi reconnect failures after BLE use by restarting the Wi-Fi driver when needed and cancelling in-progress retries when `stop` is used
- Fixed multiple potential crashes when re-connecting to saved WiFi
- Fixed potential crash on device start-up
- Fixed Wi-Fi AP/station detail view overlaying behind recreated main menu content when backing out of scan flows
- Fixed a peer-helper wardriving crash risk caused by reading the live GPS parser handle while it could be deinitialized during helper/local GPS handoff
- Fixed a wardriving packet parsing crash risk by validating short management frames before copying the 802.11 header
- Removed incorrect blescan help log

## Revival v1.9.4

### Added
- Added `wifistatus` CLI command to show connection status and saved network info
- Added new wardriving and GPS info display view
- Added GhostLink split-channel wardriving helper mode (`startwd --helper`) with helper-to-primary observation streaming
- Added optional software NMEA RX backend (`minmea_soft`) for template-specific GPS routing constraints
- Added Factory Reset option to wipe NVS and reboot
- Added auto upload to WiGLE - @Play2BReal
- Added WiGLE manual upload browser in display settings with paged CSV list and per-file upload actions
- Added WiGLE stats popup in display settings with scroll and close controls
- Added WiGLE CLI commands: `wigle files [page]`, `wigle upload <filename>`, and `wigle stats` - @Play2BReal, @jaylikesbunda
- Added control app updates - @tototo31
- Added Flipper Zero Companion App documentation - @tototo31
- Added SD JIT mounting for custom evil portal menu option
- Added hold to invert letter case on joystick select in keyboard view
- Added option to select custom portal for karma attack
- Add IO expander programmable button commands - @tototo31
- Added 'Cherry Blossom' and 'Soft Sand' themes

### Changed
- 'chipinfo' command now shows firmware version and enabled build features (Display, NFC, BadUSB, IR, GPS, etc.)
- Use country-appropriate channel list in main deauth task
- Improved GPS Info display with fix mode, satellites in view, and cleaner logging
- Moved multiple attacks and scans to separate files for maintainability
- Significantly optimised port scan memory usage
- Slightly increased IR Learn task size to prevent crash
- Improved BLE Spam
- Deauth: fixed 5GHz HT40 tuning, added burst loops, and removed rate limiting
- Reorganized settings menu into more categories
- Directly iterate to channels when deauthing multiple APs
- Optimised wardriving dwell times, added active probing and improved validation
- Wardriving now builds role-aware channel plans for split capture (primary 5 GHz, helper 2.4 GHz when both are available)
- Wardriving heartbeat now reports helper merge stats (`helper=merged/received`) for link visibility
- Reworked wardriving Wi-Fi dedupe into peek/commit flow to avoid consuming dedupe state before a successful CSV write
- Shortened delays for misc display menu building for more responsive feel
- Improved clock view responsiveness
- Increased BadUSB VSense delay to improve reliability of USB enumeration
- Improved CLI `scan` validation and status messaging for invalid durations and failed timed scans
- Improved CLI `sd` read/write/append reliability checks to report short writes and stream errors
- Improved task startup error handling for DIAL, Karma, Deauth, Beacon, EAPOL, DHCP Starvation, and SAE Flood
- Improved BLE capture startup flow to fail fast when handler registration or scan start fails
- Optimized PineAP detection memory model by lazily allocating detection tables at start and freeing them on stop
- Reworked PineAP detection logging to use a single queued worker task instead of per-detection task creation
- Reworked PCAP writer buffering to use a fixed static packet slot pool instead of per-packet heap allocations
- Reduced splash screen hold time from 2000ms to 900ms
- Refactored surface colors to be consistent across the UI
- Changed default screen timeout to 30s
- Miscellaneous fixes, improvements and refactors
- Fixed feberis pro spelling

### Fixed
- Fixed station deauth channel lookup
- Fixed potential NULL dereference in command registration when `strdup` fails under low memory
- Fixed silent serial startup failures by validating queue and task creation in `serial_manager_init`
- Fixed race-prone stop behavior in SAE flood by waiting for task exit before freeing crypto context
- Fixed race-prone restart behavior in Karma and Beacon by waiting/cleaning lingering tasks on stop
- Fixed DHCP starvation reporting success while socket/send operations were failing
- Fixed `select` CLI parse errors that omitted the invalid token and improved index list boundary checks
- Fixed GPS latitude parsing for GLL sentences (was using 3-digit degree width instead of 2)
- Fixed BLE not initializing when selecting a flipper
- Fixed crash when deinitializing BLE
- Fixed BLE stop hangs by draining active scan callbacks before shutdown and reducing heavy callback work
- Potentially fixed "Connect to saved WiFi" resets on repeated use
- Fixed GPS satellites logic
- Fixed misc wardriving issues
- Fixed wardriving AP loss where entries seen before a valid GPS fix could be skipped later by premature dedupe mutation
- Fixed 5GHz deauthing
- Fixed crash when stopping deauth
- Fixed joystick repeat only working vertically
- Fixed evil portal JIT mounting
- Fixed crash on the Setup Wizard screen
- Fixed touch handler for the WiGLE help popup
- Fixed saving of WiGLE API credentials to mirror other setting saves
- Fixed listing large amounts of evil portals on displays
- Fixed crash starting karma attack
- Fixed 'stop' not stopping the karma attack
- Fixed joystick and touch input not checking if display is dimmed
- Fixed wardrive exiting when waking the display with a touch press
- Fixed NFC saved tag popup having vertically aligned buttons instead of horizontal
- Fixed Marauder v4 SD Card mounting

## Revival v1.9.3

### Added
- Added support for the Febris Pro board
- Added support for a new upcoming board
- Added GPIO interrupt-based IR RX approach for improved reliability
- Added Knight Rider and Static RGB modes
- Added 12-bit color precision pipeline for RGB
- Added GhostLink display commands to enable/disable the on-device AP and change credentials
- Added touch support for The Wired Hatter's Banshee
- Added proper touch support to the Infrared view
- Added MAX17048 fuel gauge support for The Wired Hatter's Banshee
- Added external RTC support for saving time sync on The Wired Hatter's Banshee
- Added new compass app for The Wired Hatter's Banshee
- Added ADXL345 accelerometer app to The Wired Hatter's Banshee
- Added touch control bar to the BadUSB view
- Added BadUSB display and CLI support with built-in test script for:
  - Cardputer
  - Cardputer ADV
  - TEmbedC1101
  - The Wired Hatter's Banshee
- Added more status display logs
- Added git commit hash retrieval and logging at build time - @tototo31
- Added helper for saving scan data to SD card with incremental file numbering
- Added auto saving of scans for:
  - AP scan
  - Station scan
  - Flipper scan
  - Airtag scan
  - BLE GATT scan

### Changed
- Set IR universal send RGB pulse brightness to 20% (reduced from 100%)
- Only save changed setting to NVS when changing in settings menu to prevent hangs/crashes
- Reduced the wait time when switching RGB modes
- Refactored and optimised Rainbow and Knight Rider RGB modes
- Show highlight border on all displays regardless of touch support
- Updated main menu item order
- Remove dependencies.lock file - @tototo31
- Adjust battery voltage threshold to allow for very dead batteries - @tototo31
- Replace "Ghost ESP Ready ;)" startup message with GHOST ESP ASCII and ghostcli help prompt
- Refactored NFC and IR menus to use shared options_view helpers for consistent styling
- Downgrade GPS errors to ESP_LOGW to prevent printing in terminal

### Fixed
- Issue where RGBs would stay lit after stopping a deauth attack
- Potential crash when stopping wardriving - Thanks to @10Evansr for reporting with a fix
- Issue where AP disable wouldn't work
- Minor RGB issues on The Wired Hatter's Banshee
- IR RX issues on The Wired Hatter's Banshee
- Display hanging when going to save NFC tags on The Wired Hatter's Banshee
- Crash on the TEmbedC1101 when processing large IR signals
- Clock icon not recoloring based on theme
- Country and timezone not properly persisting
- Airtag rgb pulsing in silent rgb mode
- Fixed incorrect speed conversion in NMEA GPS parser
- Bumped NMEA queue size from 16→32 to fix the UART pattern queue overflow
- Fixed issues with saving most settings to NVS
- Fixed deauth reverse-direction frames using station MAC as BSSID instead of AP BSSID

## Revival v1.9.2

- Added Wireshark dongle mode for real-time PCAP streaming over USB/UART
- Added "No portal files found" placeholder for evil portal when SD folder is empty
- Optimized evil portal listing memory usage
- Added T-Deck keyboard shift, symbol key support with key repeat functionality
- Rewrote DIAL functionality to remove the need for HTTPS, decrease ram usage and increase reliability
- Fixed RGB LED error spam on devices without LEDs configured
- Fix EAPOL capture channel lock by stopping ALL hopping timers before capture
- Improved reliability of PCAP capture to SD card
- Fixed regression when using C5 with RGB + IR 
- Added ADC battery reading for the LilyGo T-Deck
- Fixed inverted touch scrolling in main menu list layout
- Fixed not being able to scroll up in options menus on some configs
- Fixed apps menu always opening top app instead of tapped app
- Disable light sleep in power saving mode on the T-Deck
- Added the back button to the terminal view on the T-Display S3 Touch
- Updated NimBLE config options to mirror the TEmbedC1101 for improved BLE reliability during certain tasks like AirTag detection
- Misc small fixes

## Revival v1.9.1

- Fixed WebUI AP-only restriction to correctly allow AP clients (including IPv6-mapped IPv4 addresses)
- 'setcountry' command is now case-insensitive
- Fixed T-Deck trackball spamming inputs
- Removed limit of 50 for 'scanap' to prevent getting rid of early entries
- Changed "Unknown command" to "Unsupported command" in CLI error messages for better UX.
- Improved Cardputer charging detection
- Fixed dedicated GhostLink webui terminal not showing responses

## Revival v1.9

### Added

#### Display & UI
- Added GhostLink display menu when connected to a peer with split view terminal showing normal/peer response logs
- Added 'Invert Encoder' setting to display UI for configs with encoders
- Added 9 new animations for the status display - @jaylikesbunda, @tototo31
- Added support for wired screen mirroring
- Added a first time boot setup wizard for display enabled configs

#### NFC
- Added Flipper Zero NFC parser compatibility layer with support for:
  - Aime
  - CSC Service Works
  - WashCity (Verified working)
  - Metromoney
  - Bip
  - CharlieCard
  - Disney Infinity (Verified working)
  - HI!
  - HID PACS (Verified working)
  - H World
  - Kazan
  - Microel
  - MiZIP
  - Plantain
  - Saflok (Verified working)
  - Skylanders (Verified working)
  - SmartRider (Verified working)
  - Social Moscow
  - Troika
  - Two Cities
  - Umarsh
  - Zolotaya Korona
  - Zolotaya Korona Online
- Added basic Mifare Desfire detection

#### BLE
- Added AirTag RSSI update logging so existing tags report RSSI changes every few seconds
- Added GATT scanning, service scanning and RSSI tracking

#### Wi-Fi & Networking
- Added Ethernet support and docs - @tototo31
- Added Ethernet Fingerprint scanning
- Added Unique AP counter to wardriving summary
- Added a Sweep scan to capture WiFi, BLE, GPS and 802154 data in a csv file on SD
- Added RSSI tracking for selected APs and stations
- Added OUI vendor lookup for access points and stations
- Added drone detection and spoofing

#### IR
- Added IR CLI support
- Added IR Dazzler functionality to pulse IR at 38kHz 95% duty load

#### Core & CLI
- Added CLI commands for changing the status display animations
- Added command to set amount of RGB LEDs
- Added SD Card CLI for control via WebSerial File Browser
- Added a shared string for the firmware name and version number
- Added JTAG support for ESP32C5
- Added USB HID keyboard host support on ESP32-S3 devices for controlling the UI and inputting text
- Added 'gpspin' command to set the GPS pin for recieving data

#### Hardware
- Added support for the RabbitLabs Poltergeist board
- Added basic support for the Heltec v3 (NO LORA/MESH) - @tototo31

#### Build & Docs
- Add Docker support for HTML header generation with build script - @tototo31

### Changed

#### Display & UI
- IR and NFC display views and popups now properly use active set UI theme
- Standardized LVGL screen root creation across display views and added status-bar content offset GUI helpers
- Grid menu now scrolls up and down instead of left and right
- BLE wardriving now uses dedicated wardriving screen with GPS stats and reliable device name parsing
- Reorganised the settings menu and adjusted styling
- Minor keyboard view logic and styling refactor
- Terminal enter/select now submits text if typed, otherwise opens keyboard view
- Enable clock menu for all boards by using built-in ESP32 RTC and changed icon
- Reorganised and renamed wifi display sections
- Small improvements to encoder handling
- Joystick Up/Down hold in options menus now auto-repeat

#### NFC
- Refactors to NFC logic to make more maintainable
- After scanning, NFC popup title now specifies the tag type
- Avoid redundant PN532 Mifare Classic reads for a minor speed up

#### Wi-Fi & Networking
- Changed WiGLE CSV header brand/model to report GhostESP and build template name
- Added wardriving deduplication, WiGLE CSV v1.6 pre-header escaping, and improved C5 channel hopping
- Free pcap queue and task when not capturing
- Optimized multi-AP deauth by grouping targets by channel and reducing inter-frame delays
- Added Pineapple OUI detection to existing Pineapple detection logic

#### Core & CLI
- Serial console UX improvements - @tototo31
- Use Kconfig baud rate for UART instead of hardcoded 115200
- Removed unused buffer to save 8KB RAM
- Added shared MAC formatting helper for refactors
- Added LVGL-safe helpers for NULL-safe object/timer deletion and scheduling
- Miscellaneous small code refactors and improvements
- Sync RTC time when a valid GPS fix is received
- Renamed Dual Comm UI and documentation branding to GhostLink
- Station scan now lists entries like AP scan results

#### Hardware
- Increase CPU clock speed on certain configs
- Changed default CPU clock speed to 240MHz instead of 160MHz for:
  - sdkconfig.awokimini
  - sdkconfig.CYD2432S028R
  - sdkconfig.CYD2USB
  - sdkconfig.CYD2USB2.4Inch
  - sdkconfig.CYD2USB2.4Inch_C_Varient
  - sdkconfig.CYDDualUSB
  - sdkconfig.CYDMicroUSB
  - sdkconfig.default.esp32
  - sdkconfig.default.esp2c5
  - sdkconfig.default.esp2s2
  - sdkconfig.flipper.jcmk_gps
  - sdkconfig.JCMK_DevBoardPro
  - sdkconfig.marauderv6
  - sdkconfig.minion
  - sdkconfig.poltergeist

#### Build & Docs
- Add vendor board support and images to documentation - @tototo31
- Removed Flappy Ghost app and related build/docs references

### Fixed

#### Display & UI
- Apps menu now follows main menu theme, controls and layout
- Main menu app colors are now consistent across devices
- Centralized UI theme palette definitions into a shared helper to reduce duplicate display code
- Fixed a crash when entering SYM menu on keyboard view - @dagnazty
- Fixed issues causing glitches with rainbow modes on certain devices and flicker when the RGB rainbow effect runs with power saving disabled
- Fixed status bar not resetting from rainbow styling when switching RGB mode back to normal
- Fixed apps menu not using the correct directions for joystick control
- Fixed an issue with layout of more than 6 apps on the grid menu layout

#### NFC
- Fixed an issue that would cause MFC dictionary attack to not try all possible keys
- Fixed an issue that would cause Chameleon Ultra to recover less keys than a PN532
- Adjusted NTAG model detection to infer 213/215/216 from read length even when CC size byte is incorrect
- Decoded NDEF URI fields so symbols like “@” display correctly

#### BLE
- Switched AirTag scanner to active BLE scanning for more reliable AirTag detection
- Fixed BLE scanning not being reliable
- Fixed crash after BLE deinit and during WiFi init
- Fixed RAW BLE Capture not working

#### Wi-Fi & Networking
- Fixed wardriving WiGLE v1.6 CSV output formatting
- Route evil portal HTML requests through the UART HTML buffer when active instead of the SD-backed file handler

#### IR
- Fixed IR send failing with long raw signals
- Fixed IR learn remote popup Cancel button not responding

#### Core & Hardware
- Fixed issues with GPS and GhostLink UART being shared
- Fixed gpsinfo display not logging anything when the GPS info task fails to start
- Fixed an issue where setting RGB pins would fail on configs with no LEDs set
- Debounce T-Deck trackball/keyboard I2C input
- Wrapped power-management transitions with RGB pause/resume to prevent a crash
- Fixed TEmbed C1101-specific hardware initialization running on all encoder configs
- Raise sys event task size to prevent intermittent crashes

## Revival v1.8.1

### Added

- Added included TURNHISTVOFF universal IR file with popular TV Power buttons

### Changed

- Suspend Wi-Fi services during BLE commands to guarantee enough free memory for NimBLE to initialize successfully
- Updated MFC dictionary with new additions
- Only show touchscreen scroll buttons when the options list is scrollable
- Refactored the standard mainmenu to reduce memory usage and improve performance
- Changed default UI theme to 'Bright'
- Default to WebUI authentication disabled
- Replaced Basic HTTP auth with HTTP Digest (RFC2617) using HMAC-signed stateless nonces to avoid sending plaintext credentials
- Default AP authmode changed to WPA2/WPA3 mixed for ESP32‑C5 and ESP32‑C6
- Refactor NFC to use static pools instead of heap allocations for less fragmentation and better performance
- WebUI is now served as a gzipped file to reduce loading times
- IR remotes and universals menus now show “No .ir files” placeholder when no IR files are found

### Fixed

- Prevent accidental mainmenu nav button activation during swipes
- Fixed main menu color theming to match actually enabled items
- Fixed potential status bar display issues during screen transitions
- Fixed potential issue with menu navigation after clearing lists
- Correct ADC battery percentage scaling math to prevent incorrect readings
- Fixed BQ27220 reset/reseal flow to more accurately reflect battery state

## Revival v1.8

### TL;DR

- PN532 and Chameleon Ultra support for NTAG and MIFARE Classic (read/write, NDEF, Flipper exports)
- Cardputer ADV, optional secondary status display and IO expander support
- WebUI redesign, 2 new main menu layouts
- Karma attack and 802.15.4 packet capture on C5/C6
- Heartbeat-based auto-reconnect for dual communication, stability fixes, and QoL improvements
- Miscellaneous fixes across core, display, Wi‑Fi/BLE, DNS, IR, and wardriving

### Added

#### NFC

##### PN532

- NTAG (Type 2) support: read/write NTAG213/215/216 with NDEF parsing and save to Flipper .nfc format
- MIFARE Classic support (Mini/1K/4K): Flipper dictionary attack, magic backdoor detection, and NDEF TLV parsing
- File management: 'Saved' menu for .nfc files and 'User Keys' view for `/mnt/ghostesp/nfc/mfc_user_dict.nfc`

##### Chameleon Ultra

- CLI support: connect/disconnect, status/battery, reader/emulator toggles - @tototo31
- UI support: PN532 parity with cached details, More/Save flows and dictionary attack
- NTAG and Mifare Classic NDEF parsing, Flipper `.nfc` exports from `chameleon savehf/savedump/saventag` - @tototo31, @jaylikesbunda

#### Hardware

- Added support for Cardputer ADV
- Added Kconfig support for a secondary status display
- Added Kconfig support for IO Expander - @Play2BReal
- Added heartbeat-based auto-reconnect for dual communication

#### UI

- Added 2 alternate main menu layouts (Grid and List)
- Ghost (asset by @the1anonlypr3) and Game of Life idle animations for status display
- Added command history with up/down navigation and full in-line cursor editing to the serial console - @tototo31
- Added joystick support for keyboard input in terminal view - @tototo31
- Added 'set/getneopixelbrightness' commands and ability to set settings via CLI - @tototo31

#### Attacks

- Added 802.15.4 packet capture (only on C5, C6)
- Added karma attack - @tototo31 in #108

#### Misc

- Added glog - a lightweight logging helper

### Changed

#### UI

- Use a fixed-size active-key buffer for keyboard
- Refactor popups to use reusable popup helpers
- Refactor options menu to use reusable options view helpers
- Refactor touch keyboard view to significantly reduce memory usage
- Enabled software back buttons made for encoder controls on joystick too
- Size popup buttons based on what's in them
- WebUI redesign (Part 2)
- Organise BLE menu into hierarchical sub-menus - @tototo31
- Lowered LV_MEM_SIZE from 32KB to 16KB on most display configs

#### Attacks

- Flush PCAP and CSV data to SD Card on a timer
- EAPOL capture now captures extra packet types for cracking and detects when a crackable handshake is found
- Added a summary log when starting a packet capture and reduce filter stats frequency

#### Misc

- Lowered pineap task size
- Changed the C5 to use a single display buffer to save memory
- Reduce VFS allocation unit size to 4KB
- Cap displayed WiFi APs to 50 for 'scanap' output
- Refactor comm manager to centralize packet handling, add state mutex and handshake timeout, and guard UART driver install
- If dualcomm is set to pins used by the serial UART, disable the serial UART
- Update main menu icons to RGB565A8
- Refactored dualcomm logic to be more robust
- lower all CYD LVGL memory buffers to 16KB and swap to single buffer for display

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
- Removed heap alloc per command
- Added deletions for VisualizerHandle on disconnect/stop and rgb_effect_task_handle on rgb off/stop to prevent lingering tasks 
- Removed second mdns init call
- Preallocate handlers array, remove reallocs; replace last_company_id malloc with value+flag in BLE manager
- Free all LED strip resources on deinit
- Ignore self when discovering peers for dual comm
- Prevent crash and spam in EAPOL Logoff attack
- Fixed minor issues with the dns server
- Fixed BLE capture stopping itself after recieving an event
- Added sanity checks to IE parsing to prevent OOB reads
- Accepted HCI packet types now include CMD, ACL, SCO, and ISO
- Reduce heap churn by reusing a single 4KB transfer buffer in wifi manager streaming
- Significantly improve reliability of capturing wifi frames
- Remove arbitrary limitation on the lines of text in the webUI dual comm terminal
- Fixed an issue causing potential corruption of pcaps saved to the Flipper Zero
- Fixed wardriving encryption detection
- Wardriving now properly hops channels for AP scanning

#### Display

- Possible fix for random rotation of ST7789 displays upon flashing
- Joystick builds now use touch keyboard layout with selection highlighting and navigation
- Fix keyboard not using SHIFT correctly and the keyboard view forcing lowercase
- Remove artificial delay in cardputer keyboard task to make more responsive
- Improve and refactor terminal message handling
- Remove key highlight on touch only devices for the keyboard view
- Fixed duplicate back button and wrong red styling in universals IR view

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

