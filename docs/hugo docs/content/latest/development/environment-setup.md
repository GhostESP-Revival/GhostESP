---
title: "Environment Setup"
description: "Install ESP-IDF and configure your development environment for GhostESP"
weight: 5
---

With Python 3.8+, Git, and an ESP32-based board (S3, S2, C3, C6, C5, or original ESP32) plus a USB cable, you can build GhostESP from source. Start by installing ESP-IDF.

## Install ESP-IDF

GhostESP requires **ESP-IDF v6.1**. The version in use is tracked by the badge in the repository [README](https://github.com/GhostESP-Revival/GhostESP). Follow the official Espressif guide for your OS:

- [Windows Installation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/windows-setup.html)
- [Linux Installation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/linux-setup.html)
- [macOS Installation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/macos-setup.html)

### Quick Start (Windows)

1. Install via EIM:
   ```
   winget install Espressif.EIM-CLI
   eim install -i v6.1
   ```

2. Or manually:
   ```
   git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   install.bat
   ```

3. Or use `build.py` - it can auto-detect common install locations or download ESP-IDF for you. See [Adjusting build options](/development/build-py-kconfig/).

## Activate the Environment

Each new terminal session needs ESP-IDF exported:

**PowerShell:**
```
. $env:IDF_PATH\export.ps1
```

**CMD:**
```
%IDF_PATH%\export.bat
```

**Linux/macOS:**
```
source $IDF_PATH/export.sh
```

Confirm the install with:

```
idf.py --version
```

It should output `ESP-IDF v6.1.x`.

## Clone GhostESP

```
git clone https://github.com/GhostESP-Revival/GhostESP.git
cd GhostESP
```

## Common Commands

| Command | Description |
|---------|-------------|
| `idf.py set-target esp32s3` | Set target chip |
| `idf.py menuconfig` | Open configuration menu |
| `idf.py build` | Build firmware |
| `idf.py -p COM3 flash` | Flash to device |
| `idf.py -p COM3 monitor` | Open serial monitor |
| `idf.py -p COM3 flash monitor` | Flash and monitor |
| `idf.py fullclean` | Clean build artifacts |

To add support for new hardware, see [Create custom board configs](/development/custom-board-configs/).
