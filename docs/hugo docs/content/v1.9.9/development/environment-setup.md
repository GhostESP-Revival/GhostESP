---
title: "Environment Setup"
description: "Install ESP-IDF and configure your development environment for GhostESP."
weight: 5
---

## Prerequisites

- **Hardware**: ESP32-based board (S3, S2, C3, C6, C5, or original ESP32), USB cable
- **Software**: Python 3.8+, Git

## Install ESP-IDF

GhostESP requires **ESP-IDF v5.5.3**. Follow the official Espressif guide for your OS:

- [Windows Installation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/windows-setup.html)
- [Linux Installation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/linux-setup.html)
- [macOS Installation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/macos-setup.html)

### Quick Start (Windows)

1. Install via EIM:
   ```
   winget install Espressif.EIM-CLI
   eim install -i v5.5.3
   ```

2. Or manually:
   ```
   git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   install.bat
   ```

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

## Verify Installation

```
idf.py --version
```

Should output `ESP-IDF v5.5.3`.

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

## Next Steps

- [Adjust build options](/development/build-py-kconfig/) - Configure features via menuconfig
- [Create custom board configs](/development/custom-board-configs/) - Add support for new hardware
