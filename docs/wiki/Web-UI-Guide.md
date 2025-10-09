# Ghost ESP Web UI - Complete Guide

This comprehensive guide covers all aspects of the Ghost ESP Web UI system, from quick start to advanced architecture details.

## Table of Contents

1. [Quick Start](#quick-start)
2. [System Architecture](#system-architecture)
3. [Development Workflow](#development-workflow)
4. [Memory Management](#memory-management)
5. [Security Features](#security-features)
6. [Troubleshooting](#troubleshooting)
7. [Performance Optimization](#performance-optimization)
8. [Configuration](#configuration)

---

## Quick Start

### Making Web UI Changes

```bash
# 1. Edit the HTML source
# Use your preferred text editor to edit the file
scripts/site/ghost_site.html

# 2. Regenerate the C header
python3 scripts/site/html_to_header.py

# 3. Build the firmware using build.py script
python3 build.py

# 4. Flash to device (build.py creates flashable binaries in local_builds/)
# Use esptool.py or your preferred flashing tool with the generated binaries
```

### Key Files

| File | Purpose | Location |
|------|---------|----------|
| `ghost_site.html` | Main web UI source | `scripts/site/` |
| `html_to_header.py` | HTML to C converter | `scripts/site/` |
| `ghost_esp_site.h` | Generated C header | `include/managers/` |
| `ap_manager.c` | Main web server | `main/managers/` |
| `wifi_manager.c` | Captive portal | `main/managers/` |
---

## System Architecture

### Overview

The Ghost ESP Web UI system follows a sophisticated build process that converts HTML, CSS, and JavaScript into embedded C code:

```
Ghost ESP Web UI System
=======================

HTML Source          Build Tool           C Header
ghost_site.html  ──▶ html_to_header.py ──▶ ghost_esp_site.h
     │                    │                    │
     └────────────────────┼────────────────────┘
                          ▼
                   ESP32 Device
                   ├─ Web Server (Port 80)
                   ├─ API Layer
                   └─ Portal System
                          │
                          ▼
                       Browser
```

### Component Interaction Flow

#### 1. Build Time Process

```
HTML Source (ghost_site.html)
    │
    ▼
Python Script (html_to_header.py)
    │
    ├── HTML Minification
    ├── CSS Compression  
    ├── JavaScript Minification
    │
    ▼
C Header File (ghost_esp_site.h)
    │
    ▼
ESP-IDF Build System
    │
    ▼
Embedded Firmware
```

#### 2. Runtime Process

```
Client Browser Request
    │
    ▼
ESP32 Web Server (Port 80)
    │
    ├── Authentication Check
    ├── Route to Handler
    │
    ▼
Handler Functions
    │
    ├── / → Main Web UI
    ├── /api/command → Command Execution
    ├── /api/logs → Log Retrieval
    ├── /api/settings → Settings Management
    ├── /api/sdcard → File Management
    └── /login → Captive Portal
    │
    ▼
Response Generation
    │
    ├── HTML Content (from embedded buffer)
    ├── JSON API Responses
    ├── File Downloads/Uploads
    └── JavaScript Injection (for portals)
    │
    ▼
Client Browser
```

### Web Server Architecture

#### Main Web Server (AP Manager)

```
AP Manager Web Server
=====================

HTTP Server          URI Handlers         Auth System
(Port 80)       ──▶  (13 handlers)   ──▶  (Basic Auth)
     │                    │                    │
     └────────────────────┼────────────────────┘
                          ▼
                   Request Processing
                   ├─ JSON parsing
                   ├─ Validation
                   └─ Response generation
```

#### Captive Portal System

```
Captive Portal System
=====================

DNS Server           Portal Handler        OS Detection
(Hijack requests) ──▶ (/login page)    ──▶ (Android/iOS/Windows/Linux)
     │                    │                    │
     └────────────────────┼────────────────────┘
                          ▼
                   JavaScript Injection
                   ├─ Keystroke capture
                   ├─ Form data capture
                   └─ Credential logging
```

---

## Development Workflow

### HTML to Header Conversion

**Location**: `scripts/site/html_to_header.py`

The web UI generation process uses a Python script that:

1. **Reads the source HTML file**: `scripts/site/ghost_site.html`
2. **Minifies the content**:
   - HTML minification using `htmlmin` library
   - CSS compression using `csscompressor` library  
   - JavaScript minification using `jsmin` library
3. **Converts to C header**: Generates `include/managers/ghost_esp_site.h`
4. **Optimizes for embedded use**: Creates a byte array with null termination

```python
# Key features of the conversion process:
- Automatic dependency installation (htmlmin, csscompressor, jsmin)
- Size reduction reporting (typically 20-40% reduction)
- UTF-8 encoding with null termination
- 20 bytes per line formatting for readability
```

### Alternative Non-Minified Version

**Location**: `scripts/site/html_to_header_no_minify.py`

For development purposes, there's also a non-minified version that:
- Removes only HTML comments
- Preserves original formatting
- Useful for debugging and development

### Build Integration

The web UI is integrated into the build process through:

1. **Header Generation**: HTML converted to C header during build
2. **Compilation**: Header included in firmware compilation
3. **Memory Management**: Optimized for embedded memory constraints

### Making Changes to Web UI

1. **Edit HTML**: Modify `scripts/site/ghost_site.html`
2. **Regenerate Header**: Run `python3 scripts/site/html_to_header.py`
3. **Build Firmware**: Use `python3 build.py` (recommended) or direct `idf.py build`
4. **Test**: Flash and test on device using generated binaries from `local_builds/`

### Build Script Commands

The Ghost ESP project uses a comprehensive `build.py` script that handles ESP-IDF setup, target selection, and building for multiple device configurations.

#### Basic Build Commands

```bash
# Interactive build (select targets from menu)
python3 build.py

# Build all targets
python3 build.py --targets all

# Build specific targets by index
python3 build.py --targets 0 1 2

# Build with menuconfig (configure before building)
python3 build.py --menuconfig

# Skip banner display
python3 build.py --no-banner

# Disable automatic ESP-IDF download
python3 build.py --no-auto-download

# Specify custom ESP-IDF path
python3 build.py --idf-path /path/to/esp-idf
```

#### Available Build Targets

The build script supports 30+ device configurations including:

- **Generic ESP32 variants**: esp32, esp32s2, esp32s3, esp32c3, esp32c5, esp32c6
- **Development boards**: Awok V5, ghostboard, MarauderV4, MarauderV6
- **Display devices**: CYD series, Waveshare LCD, Crowtech LCD, Sunton LCD
- **Specialized boards**: T-Deck, TEmbedC1101, S3TWatch, Flipper devices

#### Build Output

The build script creates organized output in the `local_builds/` directory:

```
local_builds/
├── esp32-generic/
│   ├── bootloader.bin
│   ├── partitions.bin
│   └── firmware.bin
├── esp32-generic.zip
└── esp32-generic-merged-gesp.bin
```

#### Flashing Built Firmware

```bash
# Flash using esptool.py with merged binary
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 esp32-generic-merged-gesp.bin

# Flash individual components
esptool.py --chip esp32 --port /dev/ttyUSB0 \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin
```

---

## Memory Management

### HTML Serving Hierarchy

```
1. Custom Uploaded Portal (Memory Buffer)
   ├── html_buffer (RAM)
   ├── html_buffer_size
   └── use_html_buffer flag

2. Default Embedded Portal (Flash)
   ├── default_portal_html (Flash/ROM)
   └── INTERNAL_DEFAULT_PORTAL flag

3. File/URL Streaming (SD Card/Network)
   ├── stream_data_to_client()
   └── Chunked transfer encoding
```

---

## Security Features

### Authentication System

The web server implements HTTP Basic Authentication:

### Authentication Flow

```
Client Request
    │
    ▼
Authentication Check
    │
    ├── Basic Auth Header Present?
    │   ├── Yes → Validate Credentials
    │   └── No → Return 401 Unauthorized
    │
    ▼
Authorization
    │
    ├── Valid Credentials?
    │   ├── Yes → Process Request
    │   └── No → Return 401 Unauthorized
    │
    ▼
Request Processing
```

### Default Credentials
- Username: `GhostNet`
- Password: `GhostNet`

### Enable/Disable Authentication

Web authentication can be controlled via serial commands:

```bash
# Enable web authentication
webauth on

# Disable web authentication  
webauth off
```

The authentication state is stored in NVS (Non-Volatile Storage) and persists across reboots.

## Troubleshooting

### Common Issues

#### Web UI Not Loading
1. Check if web server is running
2. Verify authentication credentials
3. Check network connectivity
4. Monitor serial output for errors

#### Build Issues
1. Install Python dependencies: `pip install htmlmin csscompressor jsmin`
2. Check HTML syntax
3. Verify file paths
4. Use build.py for clean builds: `python3 build.py --targets all`
5. Check ESP-IDF installation: `python3 build.py --idf-path /path/to/esp-idf`

#### Authentication Issues
1. Check credentials in settings
2. Verify authentication is enabled
3. Clear browser cache
4. Check for CORS issues

### Debug Tools

#### Enable Verbose Logging
```c
// In sdkconfig or menuconfig
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
```

#### Web Server Debug
```c
// Add to handlers
ESP_LOGI(TAG, "Request URI: %s", req->uri);
ESP_LOGI(TAG, "Content length: %d", req->content_len);
```

### Common Issues and Solutions

#### Issue: Web UI shows blank page
**Solution**: Check if `ghost_esp_site.h` was generated correctly and included in build

#### Issue: Authentication not working
**Solution**: Verify credentials and ensure authentication is enabled in settings

#### Issue: Build fails with missing dependencies
**Solution**: Install required Python packages: `pip install htmlmin csscompressor jsmin`

#### Issue: Portal not capturing credentials
**Solution**: Check JavaScript injection and ensure `/api/log` endpoint is working

---

## Performance Optimization

### HTML Optimization
- Minimize HTML size
- Use efficient CSS
- Optimize JavaScript
- Remove unnecessary comments

### Network Optimization
- Enable HTTP keep-alive
- Use compressed responses
- Implement proper caching
- Optimize API responses

## File Structure

```
Ghost_ESP/
├── build.py                         # Cross-platform build script
├── scripts/site/
│   ├── ghost_site.html              # Main UI source
│   ├── html_to_header.py            # Minified converter
│   └── html_to_header_no_minify.py  # Non-minified converter
├── include/managers/
│   ├── ghost_esp_site.h             # Generated header
│   └── default_portal.h             # Default portal
├── main/managers/
│   ├── ap_manager.c                 # Main web server
│   └── wifi_manager.c               # Captive portal
├── configs/                         # Build configurations
│   ├── sdkconfig.default.esp32      # ESP32 configuration
│   ├── sdkconfig.default.esp32s2    # ESP32-S2 configuration
│   └── ...                          # Other target configurations
├── local_builds/                    # Build output directory
│   ├── esp32-generic/               # Individual target builds
│   ├── esp32-generic.zip            # Packaged builds
│   └── esp32-generic-merged-gesp.bin # Merged flashable binaries
└── docs/
    └── wiki/
        └── Web-UI-Guide.md          # This comprehensive guide
```

---

## Related Documentation

- **[Evil Portal Guide](Evil-Portal-Guide.md)** - Complete beginner's guide to captive portals
- **[Commands Reference](Commands.md)** - Complete command list and usage
- **[Features Overview](Features.md)** - All available features and capabilities
- **[Troubleshooting](Troubleshooting.md)** - Common issues and solutions

---

## Useful Links

- [ESP-IDF HTTP Server Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [ESP-IDF Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html)
- [HTML Minification Tools](https://www.w3.org/TR/html-minification/)
- [JavaScript Minification](https://github.com/tikitu/jsmin)

---

*This comprehensive guide covers all aspects of the Ghost ESP Web UI system. For specific implementation details, refer to the source code files mentioned throughout this document.*
