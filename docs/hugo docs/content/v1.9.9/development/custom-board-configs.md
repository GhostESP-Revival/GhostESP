---
title: "Custom Board Configurations"
description: "Create and customize board profiles for GhostESP."
weight: 20
---

Board configurations are stored in `configs/sdkconfig.*` files. Each file contains all Kconfig settings for a specific board.

## Available Base Configs

| Config File | Target | Description |
|-------------|--------|-------------|
| `sdkconfig.default.esp32` | ESP32 | Original ESP32 |
| `sdkconfig.default.esp32s2` | ESP32-S2 | USB-enabled, no BLE |
| `sdkconfig.default.esp32s3` | ESP32-S3 | USB OTG, BLE, PSRAM |
| `sdkconfig.default.esp32c3` | ESP32-C3 | RISC-V, low cost |
| `sdkconfig.default.esp32c6` | ESP32-C6 | RISC-V, Wi-Fi 6 |
| `sdkconfig.default.esp32c5` | ESP32-C5 | RISC-V, latest |

## Creating a New Board Profile

### 1. Copy a base config

Choose the closest matching chip family:

```
cp "configs/sdkconfig.default.esp32s3" "configs/sdkconfig.myboard"
```

### 2. Clean previous build

```
idf.py fullclean
```

### 3. Apply the config

```
cp "configs/sdkconfig.myboard" "sdkconfig"
cp "configs/sdkconfig.myboard" "sdkconfig.defaults"
```

### 4. Set the target

```
idf.py set-target esp32s3
```

### 5. Open menuconfig

```
idf.py menuconfig
```

Configure your hardware settings (see sections below).

### 6. Build and test

```
idf.py -p COM3 flash monitor
```

### 7. Save your config

After verifying everything works, copy back to your profile:

```
cp "sdkconfig" "configs/sdkconfig.myboard"
```

## Key Configuration Sections

### Ghost ESP Options

Navigate to **Ghost ESP Options** in menuconfig:

- **Device Details** → `BUILD_CONFIG_TEMPLATE` — Set your board name (e.g., "myboard")
- **LED Options** — Enable Neopixel or RGB LED, set data pins
- **Display Options** — Enable screen, set resolution, touchscreen
- **SPI and MMC Configuration** — SD card pins (SPI or 1-bit SDIO)
- **GPS Configuration** — UART RX pin and baud rate
- **NFC Options** — PN532 or Chameleon Ultra
- **NRF24 Options** — SPI pins for NRF24L01 module
- **Misc Options** — Joystick, battery ADC, BadUSB, ethernet, infrared

### LVGL Display Driver

Navigate to **Component config → LVGL → LVGL ESP Drivers → LVGL TFT Display controller**:

- Select your display controller (ILI9341, ST7789, GC9A01, etc.)
- Configure SPI pins (MOSI, MISO, SCK, CS, DC, RST)
- Set SPI clock speed

For touchscreens, configure under **LVGL Touch Input**.

### Log Level

Navigate to **Component config → Log output**:

Set **Default log verbosity** to:
- **Info** (recommended for debugging) — Standard operational messages
- **Warning** (default) — Minimal output
- **Error** — Only errors

### Console Baud Rate

Navigate to **Component config → ESP System Settings**:

Set **UART console baud rate** (default: 115200)

## Common Pin Assignments

| Feature | Config Option | Typical S3 Pins |
|---------|---------------|-----------------|
| SD SPI MOSI | `SD_SPI_MOSI_PIN` | 23 |
| SD SPI MISO | `SD_SPI_MISO_PIN` | 19 |
| SD SPI CLK | `SD_SPI_CLK_PIN` | 18 |
| SD SPI CS | `SD_SPI_CS_PIN` | 4 |
| GPS RX | `GPS_UART_RX_PIN` | 16 |
| NRF24 MOSI | `NRF24_SPI_MOSI_PIN` | 14 |
| NRF24 MISO | `NRF24_SPI_MISO_PIN` | 21 |
| NRF24 SCK | `NRF24_SPI_SCK_PIN` | 13 |
| NRF24 CSN | `NRF24_CSN_PIN` | 12 |
| NRF24 CE | `NRF24_CE_PIN` | 11 |

## Example: Adding a New ESP32-S3 Board

1. Start with the S3 default:
   ```
   idf.py fullclean
   cp "configs/sdkconfig.default.esp32s3" "sdkconfig"
   cp "configs/sdkconfig.default.esp32s3" "sdkconfig.defaults"
   idf.py set-target esp32s3
   ```

2. Open menuconfig and configure:
   - Set `BUILD_CONFIG_TEMPLATE` to "my_awesome_board"
   - Enable `WITH_SCREEN` and set `TFT_WIDTH`/`TFT_HEIGHT`
   - Configure display driver under LVGL settings
   - Set SD card SPI pins
   - Enable any extra features (GPS, NFC, etc.)

3. Build and test:
   ```
   idf.py -p COM3 flash monitor
   ```

4. Save when working:
   ```
   cp "sdkconfig" "configs/sdkconfig.my_awesome_board"
   ```

## Troubleshooting

- **Build fails after config change**: Run `idf.py fullclean` and rebuild
- **Menuconfig shows wrong target**: Re-run `idf.py set-target esp32s3`
- **Config not applying**: Ensure both `sdkconfig` and `sdkconfig.defaults` are copied
