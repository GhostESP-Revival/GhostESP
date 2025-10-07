# Ghost ESP Web UI - Complete Guide

This comprehensive guide covers all aspects of the Ghost ESP Web UI system, from quick start to advanced architecture details.

## 📋 Table of Contents

1. [Quick Start](#quick-start)
2. [System Architecture](#system-architecture)
3. [API Reference](#api-reference)
4. [Development Workflow](#development-workflow)
5. [Memory Management](#memory-management)
6. [Security Features](#security-features)
7. [Troubleshooting](#troubleshooting)
8. [Performance Optimization](#performance-optimization)
9. [Configuration](#configuration)

---

## 🚀 Quick Start

### Making Web UI Changes

```bash
# 1. Edit the HTML source
vim scripts/site/ghost_site.html

# 2. Regenerate the C header
python3 scripts/site/html_to_header.py

# 3. Build the firmware
idf.py build

# 4. Flash to device
idf.py flash
```

### Development Mode (Non-Minified)

```bash
# Use non-minified version for debugging
python3 scripts/site/html_to_header_no_minify.py
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

## 🏗️ System Architecture

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
(Port 80)       ──▶  (60 handlers)   ──▶  (Basic Auth)
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

## 🔌 API Reference

### Authentication
All API endpoints require HTTP Basic Authentication:
- **Username**: `GhostNet` (or current AP SSID)
- **Password**: `GhostNet` (or current AP password)

### Main Web UI
- `GET /` - Main web interface (returns HTML)

### Device Management

#### POST /api/command
Execute a command on the device.

**Request:**
```json
{
  "command": "wifi scan"
}
```

**Response:**
- **Success**: `200 OK` - "Command executed"
- **Error**: `400 Bad Request` - "Invalid JSON" or "Missing or invalid 'command' field"

#### GET /api/logs
Get device logs.

**Request:** No body required

**Response:**
- **Success**: `200 OK` - Plain text log content
- **Error**: Empty response if no logs or system error

#### POST /api/clear_logs
Clear the log buffer.

**Request:** No body required

**Response:**
- **Success**: `200 OK` - `{"status":"success","message":"logs_cleared"}`
- **Error**: `200 OK` - `{"status":"error","message":"Log system not initialized"}` or `{"status":"error","message":"Failed to acquire lock"}`

### Settings

#### GET /api/settings
Get all device settings.

**Request:** No body required

**Response:**
```json
{
  "broadcast_speed": 100,
  "ap_ssid": "GhostNet",
  "ap_password": "GhostNet",
  "rgb_mode": 0,
  "rgb_speed": 50,
  "channel_delay": 200,
  "portal_url": "INTERNAL_DEFAULT_PORTAL",
  "portal_ssid": "",
  "portal_password": "",
  "portal_ap_ssid": "",
  "portal_domain": "",
  "portal_offline_mode": false,
  "printer_ip": "",
  "printer_text": "",
  "printer_font_size": 12,
  "printer_alignment": 0,
  "hex_accent_color": "#FF0000",
  "timezone_str": "UTC",
  "gps_rx_pin": 16,
  "display_timeout": 30,
  "rts_enabled_bool": 0,
  "web_auth_enabled": true,
  "ap_enabled": true,
  "esp_comm_tx_pin": 17,
  "esp_comm_rx_pin": 18,
  "sta_ip": "192.168.1.100",
  "sta_netmask": "255.255.255.0",
  "sta_gateway": "192.168.1.1"
}
```

#### POST /api/settings
Update device settings.

**Request:**
```json
{
  "broadcast_speed": 100,
  "ap_ssid": "MyGhostNet",
  "ap_password": "MyPassword123",
  "rainbow_mode": true,
  "rgb_speed": 75,
  "channel_delay": 150,
  "portal_url": "http://example.com/portal.html",
  "portal_ssid": "FreeWiFi",
  "portal_password": "password123",
  "portal_ap_ssid": "PortalAP",
  "portal_domain": "example.com",
  "portal_offline_mode": false,
  "printer_ip": "192.168.1.50",
  "printer_text": "Hello World",
  "printer_font_size": 14,
  "printer_alignment": 1,
  "hex_accent_color": "#00FF00",
  "timezone_str": "America/New_York",
  "gps_rx_pin": 16,
  "display_timeout": 60,
  "rts_enabled_bool": 1,
  "web_auth_enabled": true,
  "ap_enabled": true,
  "esp_comm_tx_pin": 17,
  "esp_comm_rx_pin": 18
}
```

**Response:**
- **Success**: `200 OK` - JSON response with updated settings
- **Error**: `500 Internal Server Error` - "Failed to parse JSON" or "Failed to create JSON object"

### File Management

#### GET /api/sdcard
List SD card files and storage information.

**Request:** No body required

**Response:**
```json
{
  "storage": {
    "total": 15728640,
    "used": 1048576
  },
  "files": [
    {
      "name": "portal.html",
      "path": "/mnt/portal.html",
      "size": 2048,
      "is_directory": false
    },
    {
      "name": "portals",
      "path": "/mnt/portals",
      "size": 0,
      "is_directory": true
    }
  ]
}
```

**Error Responses:**
- `500 Internal Server Error` - `{"error": "SD card not supported or not mounted."}`
- `500 Internal Server Error` - `{"error": "Failed to scan SD card."}`

#### POST /api/sdcard/download
Download a file from SD card.

**Request:**
```json
{
  "path": "/mnt/portal.html"
}
```

**Response:**
- **Success**: `200 OK` - File content (binary)
- **Error**: `400 Bad Request` - `{"error": "'path' is required and must be a string."}`
- **Error**: `404 Not Found` - `{"error": "File not found."}`

#### POST /api/sdcard/upload
Upload a file to SD card.

**Request:** Multipart form data with file content
- **Query Parameter**: `path` - Destination file path
- **Body**: File content

**Response:**
- **Success**: `200 OK` - `{"status": "success", "message": "File uploaded successfully"}`
- **Error**: `400 Bad Request` - `{"error": "Missing or invalid 'path' query parameter."}`
- **Error**: `500 Internal Server Error` - `{"error": "Memory allocation failed for buffer."}`

#### DELETE /api/sdcard
Delete a file from SD card.

**Request:** Query parameter `path`

**Response:**
- **Success**: `200 OK` - "File deleted successfully"
- **Error**: `400 Bad Request` - "Missing or invalid 'path' parameter"
- **Error**: `500 Internal Server Error` - "Failed to delete the file"

### ESP Communication

#### GET /api/esp_comm/status
Get ESP communication status.

**Request:** No body required

**Response:**
```json
{
  "state": "connected",
  "connected": true,
  "is_remote_command": false
}
```

**States:**
- `idle` - Not connected, not scanning
- `scanning` - Searching for other ESP32 devices
- `handshake` - Establishing connection
- `connected` - Successfully connected
- `error` - Connection error occurred

#### POST /api/esp_comm/control
Control ESP communication (start discovery, connect, disconnect).

**Request:**
```json
{
  "action": "start_discovery"
}
```

**Actions:**
- `start_discovery` - Start scanning for other ESP32 devices
- `connect` - Connect to discovered device
- `disconnect` - Disconnect from current device

**Response:**
```json
{
  "success": true,
  "message": "Discovery started successfully"
}
```

**Error Responses:**
- `400 Bad Request` - `{"error": "Invalid request payload"}`
- `400 Bad Request` - `{"error": "Missing or invalid action"}`

#### POST /api/esp_comm/send
Send command to connected ESP32 device.

**Request:**
```json
{
  "command": "wifi scan"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Command sent successfully"
}
```

**Error Responses:**
- `400 Bad Request` - `{"error": "Invalid request payload"}`
- `400 Bad Request` - `{"error": "Missing or invalid command"}`

### Captive Portal

#### GET /login
Portal page for credential capture.

**Request:** No body required

**Response:** HTML portal page

#### POST /api/log
Capture credentials from portal.

**Request:** Form data with captured credentials

**Response:**
- **Success**: `200 OK` - Portal success page
- **Error**: `400 Bad Request` - Invalid request

#### GET /get
Process captured credentials.

**Request:** No body required

**Response:** HTML response

### Error Responses

All endpoints may return these common error responses:

- **401 Unauthorized**: Invalid or missing authentication credentials
- **400 Bad Request**: Invalid request format or missing required parameters
- **500 Internal Server Error**: Server-side error or system failure

### Response Headers

- **Content-Type**: `application/json` for JSON responses, `text/plain` for logs, `text/html` for web pages
- **WWW-Authenticate**: `Basic realm="Protected Area"` for authentication errors

---

## 🔧 Development Workflow

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
3. **Build Firmware**: Use `idf.py build` or `build.py`
4. **Test**: Flash and test on device

### Common Build Commands

```bash
# Clean build
idf.py clean && idf.py build

# Full clean
idf.py fullclean && idf.py build

# Build with menuconfig
idf.py menuconfig && idf.py build

# Flash firmware
idf.py flash

# Flash and monitor
idf.py flash monitor

# Monitor serial output
idf.py monitor

# Monitor with filter
idf.py monitor --print_filter="*:INFO"
```

---

## 💾 Memory Management

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

### Memory Optimization Features

- **HTML Minification**: 20-40% size reduction
- **Chunked Transfer**: Large files streamed in chunks
- **Buffer Management**: Efficient memory allocation/deallocation
- **Heap Monitoring**: Real-time memory usage tracking
- **Flash Storage**: Static content stored in flash memory

### HTML Buffer System

The system uses multiple HTML serving strategies:

```c
// HTML buffer management
static char* html_buffer = NULL;
static size_t html_buffer_size = 0;
static bool use_html_buffer = false;

// Memory-optimized serving
if (html_buffer != NULL && html_buffer_size > 0) {
    // Serve from memory buffer
} else if (strcmp(PORTALURL, "INTERNAL_DEFAULT_PORTAL") == 0) {
    // Serve embedded default portal
} else {
    // Stream from file/URL
}
```

### Memory Monitoring

```c
// Monitor heap usage
ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

// Monitor heap usage (alternative)
size_t free_heap = esp_get_free_heap_size();
ESP_LOGI(TAG, "Free heap: %zu bytes", free_heap);
```

---

## 🔒 Security Features

### Authentication System

The web server implements HTTP Basic Authentication:

```c
// Authentication check
static bool check_auth(httpd_req_t *req) {
    char auth_header[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        // Decode and validate credentials
        return validate_credentials(auth_header);
    }
    return false;
}
```

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
```c
// In settings
#define WEB_AUTH_ENABLED true
```

### Input Validation

- **JSON Payload Validation**: cJSON parsing with error handling
- **Command Sanitization**: Input validation for command execution
- **File Upload Restrictions**: Size and type limitations
- **Request Size Limits**: Protection against large payloads

### Security Best Practices

#### Authentication
- Use strong passwords
- Enable authentication in production
- Implement session management
- Regular credential updates

#### Input Validation
- Validate all JSON inputs
- Sanitize command inputs
- Limit file upload sizes
- Implement request size limits

#### Network Security
- Use HTTPS when possible
- Implement CORS properly
- Validate all requests
- Monitor for suspicious activity

---

## 🐛 Troubleshooting

### Common Issues

#### Web UI Not Loading
1. Check if web server is running
2. Verify authentication credentials
3. Check network connectivity
4. Monitor serial output for errors

#### Memory Issues
1. Monitor heap usage
2. Reduce HTML buffer size
3. Check for memory leaks
4. Use smaller portal files

#### Build Issues
1. Install Python dependencies: `pip install htmlmin csscompressor jsmin`
2. Check HTML syntax
3. Verify file paths
4. Clean and rebuild

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

#### Memory Debug
```c
// Monitor heap usage
size_t free_heap = esp_get_free_heap_size();
ESP_LOGI(TAG, "Free heap: %zu bytes", free_heap);
```

### Common Issues and Solutions

#### Issue: Web UI shows blank page
**Solution**: Check if `ghost_esp_site.h` was generated correctly and included in build

#### Issue: Authentication not working
**Solution**: Verify credentials and ensure authentication is enabled in settings

#### Issue: Memory allocation failed
**Solution**: Reduce HTML buffer size or use file streaming for large content

#### Issue: Build fails with missing dependencies
**Solution**: Install required Python packages: `pip install htmlmin csscompressor jsmin`

#### Issue: Portal not capturing credentials
**Solution**: Check JavaScript injection and ensure `/api/log` endpoint is working

---

## ⚡ Performance Optimization

### HTML Optimization
- Minimize HTML size
- Use efficient CSS
- Optimize JavaScript
- Remove unnecessary comments

### Memory Optimization
- Use chunked transfer for large files
- Monitor heap usage
- Implement proper cleanup
- Use flash storage for static content

### Network Optimization
- Enable HTTP keep-alive
- Use compressed responses
- Implement proper caching
- Optimize API responses

### Optimization Strategies

1. **Memory Management**
   - Efficient buffer allocation
   - Heap monitoring and cleanup
   - Flash storage for static content

2. **Network Optimization**
   - Chunked transfer encoding
   - HTTP keep-alive connections
   - Compressed responses where possible

3. **Processing Efficiency**
   - Asynchronous request handling
   - Non-blocking I/O operations
   - Efficient JSON parsing

### Monitoring and Debugging

- **Heap Usage Tracking**: Real-time memory monitoring
- **Request Logging**: Detailed request/response logging
- **Performance Metrics**: Response time tracking
- **Error Handling**: Comprehensive error reporting

---

## ⚙️ Configuration

### Web Server Settings

```c
#define MAX_URI_HANDLERS 60
#define SERVER_PORT 80
#define STACK_SIZE 8192
#define MAX_LOG_BUFFER_SIZE (8 * 1024)
```

### Authentication Settings

```c
#define WEB_AUTH_ENABLED true
#define DEFAULT_USERNAME "GhostNet"
#define DEFAULT_PASSWORD "GhostNet"
```

### Memory Settings

```c
#define MAX_HTML_BUFFER_SIZE 2048
#define CHUNK_SIZE 1024
#define MAX_FILE_SIZE (5 * 1024 * 1024)
```

### Server Configuration

```c
// Server configuration
httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
server_config.server_port = 80;
server_config.max_uri_handlers = 60;
server_config.stack_size = 8192;
```

---

## 📁 File Structure

```
Ghost_ESP/
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
└── docs/
    └── wiki/
        └── Web-UI-Guide.md          # This comprehensive guide
```

---

## 🔗 Related Documentation

- **[Evil Portal Guide](Evil-Portal-Guide.md)** - Complete beginner's guide to captive portals
- **[Commands Reference](Commands.md)** - Complete command list and usage
- **[Features Overview](Features.md)** - All available features and capabilities
- **[Troubleshooting](Troubleshooting.md)** - Common issues and solutions

---

## 📚 Useful Links

- [ESP-IDF HTTP Server Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [ESP-IDF Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html)
- [HTML Minification Tools](https://www.w3.org/TR/html-minification/)
- [JavaScript Minification](https://github.com/tikitu/jsmin)

---

*This comprehensive guide covers all aspects of the Ghost ESP Web UI system. For specific implementation details, refer to the source code files mentioned throughout this document.*
