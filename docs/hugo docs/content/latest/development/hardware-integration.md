---
title: "Hardware Integration"
description: "Understanding hardware quirks and bus conflicts in GhostESP."
weight: 30
---

GhostESP supports many hardware configurations, but some combinations require special handling due to shared resources.

## IO Expander (TCA9535 / CH422G)

Some boards use an I2C IO expander to provide additional GPIO pins for buttons, joysticks, or other peripherals.

### Shared I2C Bus

The IO expander typically shares its I2C bus with:
- **Status display (OLED)** — Secondary display on boards like Heltec V3
- **Compass (QMC6309)** — If configured on the expander

### Configuration

In menuconfig under **Ghost ESP Options → Misc Options**:

- `CONFIG_USE_IO_EXPANDER` — Enable TCA9535 IO expander
- `CONFIG_IO_EXPANDER_SDA_PIN` / `CONFIG_IO_EXPANDER_SCL_PIN` — I2C pins
- `CONFIG_IO_EXPANDER_I2C_ADDR` — I2C address (default 0x74)

### Code Considerations

When the compass is on the IO expander (`CONFIG_COMPASS_ON_EXPANDER`), I2C communication uses bit-banged GPIO through the expander rather than hardware I2C. This is slower but allows pin expansion.

```c
// Joystick manager checks for IO expander
#ifdef CONFIG_USE_IO_EXPANDER
esp_err_t joystick_io_expander_init(void);
#endif
```

## Shared TFT/SD SPI Bus

Some boards share SPI pins between the TFT display and SD card to reduce pin count.

### When It Applies

Enable `CONFIG_SHARED_TFT_SD_SPI` in menuconfig when:
- MOSI, MISO, and CLK pins are the same for both display and SD
- Only CS pins differ between the two devices

### How It Works

The SD card manager coordinates with LVGL to avoid bus conflicts:

```c
// sd_card_manager.c pauses LVGL during SD operations
bool shared_spi_guard_active = false;
if (is_shared_display_sd_spi()) {
    shared_spi_guard_active = true;
    // LVGL display updates paused
}
// ... SD operation ...
shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active);
```

### Performance Impact

- Shared bus limits SPI clock to 4 MHz for reliability
- Display may flicker or pause during heavy SD operations
- Separate SPI buses are recommended for best performance

### Configuring

In menuconfig under **Ghost ESP Options → SPI and MMC Configuration**:

- `CONFIG_SHARED_TFT_SD_SPI` — Enable shared bus coordination

## Multiple SPI Buses

ESP32 chips have multiple SPI controllers, but they must be allocated carefully:

| SPI Host | Typical Use | Notes |
|----------|-------------|-------|
| SPI1_HOST | Flash (internal) | Do not use |
| SPI2_HOST | Display, SD, or NRF24 | Most common for peripherals |
| SPI3_HOST | Alternative peripheral | Available on most chips |

### Conflicts to Avoid

- **Display + SD on same SPI**: Use `CONFIG_SHARED_TFT_SD_SPI`
- **NRF24 + Display on same SPI**: Possible but requires coordination
- **SD in 1-bit SDIO mode**: Uses separate pins, no SPI conflict

### NRF24 SPI Configuration

NRF24 can use SPI2 or SPI3:

```
CONFIG_NRF24_SPI_HOST=2  # SPI2_HOST
CONFIG_NRF24_SPI_MOSI_PIN=14
CONFIG_NRF24_SPI_MISO_PIN=21
CONFIG_NRF24_SPI_SCK_PIN=13
CONFIG_NRF24_CSN_PIN=12
CONFIG_NRF24_CE_PIN=11
```

## GhostLink Multi-Device Setup

GhostLink allows two ESP32 devices to communicate via UART, enabling split workloads.

### Use Cases

- **Display controller + attack radio**: C5 with screen controls S3 that runs Wi-Fi attacks
- **WebUI + scanning**: One device hosts AP, other performs scans
- **GPS routing**: S3 receives GPS data, streams to C5 via GhostLink

### UART Configuration

Default pins vary by chip:

| Chip | TX Pin | RX Pin |
|------|--------|--------|
| ESP32-S3 | GPIO 6 | GPIO 7 |
| ESP32 (base) | GPIO 17 | GPIO 16 |

Change via CLI: `commsetpins <tx> <rx>`

### Remote Commands

Use `commsend` to route commands to the peer device:

```bash
commsend scanap           # Scan on peer
commsend karma start      # Start karma on peer
commsend gpsinfo          # Get GPS info from peer
```

### Display Menu Integration

When GhostLink is active, the options screen shows a **GhostLink** menu with remote attack controls. These automatically wrap commands with `commsend`:

```c
// options_screen.c
simulateCommand("commsend scanap");
simulateCommand("commsend karma start");
```

## BadUSB (ESP32-S3 Only)

BadUSB uses TinyUSB in device mode, which conflicts with USB Host mode.

### Pin Configuration

```
CONFIG_HAS_BADUSB=y
CONFIG_BADUSB_VSENSE_PIN=-1  # Or valid GPIO for VBUS sense
```

### USB Mode Conflicts

- **BadUSB active**: USB port acts as HID keyboard to host PC
- **USB Host mode**: USB port reads from external USB keyboard
- **CDC Console**: USB serial for debugging

These modes are mutually exclusive. The device typically defaults to CDC console and switches modes on demand.

## Ethernet (W5500)

Ethernet uses SPI, which may conflict with other SPI peripherals.

### Pin Configuration

```
CONFIG_WITH_ETHERNET=y
CONFIG_ETH_W5500_MOSI_PIN=6
CONFIG_ETH_W5500_MISO_PIN=5
CONFIG_ETH_W5500_SCK_PIN=4
CONFIG_ETH_W5500_CS_PIN=15
CONFIG_ETH_W5500_INT_PIN=7  # Optional, -1 to disable
```

### SPI Bus Sharing

If Ethernet shares SPI with display or SD:
- Use a different CS pin for each device
- Consider SPI clock speed compatibility
- May require `CONFIG_SHARED_TFT_SD_SPI` style coordination

## Infrared

IR TX/RX uses RMT (Remote Control Transceiver) peripheral, which is separate from SPI.

### Pin Configuration

```
CONFIG_HAS_INFRARED=y
CONFIG_INFRARED_LED_PIN=0      # TX pin
CONFIG_HAS_INFRARED_RX=y
CONFIG_INFRARED_RX_PIN=0       # RX pin
```

### No Bus Conflicts

RMT is independent of SPI and I2C, so IR can operate alongside display, SD, and other peripherals without coordination.

## Troubleshooting Bus Conflicts

### Symptoms

- Display corruption during SD writes
- SD card mount failures when display active
- SPI devices not responding
- Random crashes during multi-peripheral operations

### Solutions

1. **Check pin assignments**: Ensure no two devices use the same CS pin
2. **Enable shared bus coordination**: `CONFIG_SHARED_TFT_SD_SPI`
3. **Reduce SPI clock speed**: Lower `CONFIG_NRF24_SPI_CLOCK_HZ` or SD clock
4. **Separate SPI buses**: Move one device to SPI3_HOST if available
5. **Check I2C address conflicts**: IO expander, status display, and compass must have unique addresses
