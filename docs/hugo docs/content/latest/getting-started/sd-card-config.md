---
title: "SD Card Configuration"
description: "Automatically load WiFi and WiGLE settings from SD card"
weight: 25
---

Load WiFi credentials and WiGLE settings from a configuration file on your SD card for quick device setup.

## Quick Start

1. Create a file named `config.cfg` in the root of your SD card
2. Add your settings (see format below)
3. Load via **Settings → WiGLE → Load Config from SD** or CLI command `loadconfig`
4. Confirm loaded settings in the popup/output

## Configuration File Format

Create `config.cfg` in the root directory of your SD card (`/mnt/ghostesp/config.cfg`):

```ini
#-------------
#-----WIFI----
#-------------
SSID=YourNetworkName
PASSKEY=YourWiFiPassword

#------------
#------Wigle-
#------------
# Encoded for Use token from Wigle.net/account
# Auto Upload Values = true/false
# Donate values = true/false (true preferred)
EncodedForUseToken=YourBase64EncodedToken
AutoUpload=true
Donate=true
```

## Configuration Fields

| Field | Required | Description |
|-------|----------|-------------|
| `SSID` | Optional | WiFi network name to connect to |
| `PASSKEY` | Optional | WiFi password |
| `EncodedForUseToken` | Optional | WiGLE "Encoded for Use" token from [wigle.net/account](https://wigle.net/account) |
| `AutoUpload` | Optional | Enable automatic upload when WiFi connects (`true`/`false`) |
| `Donate` | Optional | Share wardriving data with WiGLE (`true`/`false`, recommended: `true`) |

### Notes
- All fields are optional - only include what you want to configure
- Lines starting with `#` are comments and ignored
- Blank lines are ignored
- Field names are case-insensitive (`SSID` = `ssid` = `Ssid`)

## Getting Your WiGLE Token

### Option 1: Encoded for Use Token (Recommended)

1. Log in to [wigle.net](https://wigle.net)
2. Go to [wigle.net/account](https://wigle.net/account)
3. Find the **"Encoded for Use"** token in the API section
4. Copy the entire base64-encoded string
5. Paste it as `EncodedForUseToken=` in your config.cfg

**Example:**
```
EncodedForUseToken=QUlEMTIzNDU2Nzg5MDphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5eg==
```

### Option 2: Legacy API Format

If you prefer the legacy format, you can still use `APIName:APIToken`:

```
# Legacy format still works
EncodedForUseToken=AID0123456789:abcdefghijklmnopqrstuvwxyz123456
```

The system automatically detects the format:
- **No colon**: Treated as pre-encoded token (used directly)
- **With colon**: Treated as `APIName:APIToken` (base64 encoded automatically)

## Loading Configuration

### Via GUI (Display Builds)

1. Insert SD card with `config.cfg` in root directory
2. Navigate to **Settings → WiGLE**
3. Select **"Load Config from SD"**
4. Review the confirmation popup showing loaded settings:
   - WiFi SSID (if configured)
   - WiGLE token status (set/not set)
   - AutoUpload status
   - Donate status

### Via Command Line

```bash
loadconfig
```

**Example Output:**
```
Loading config from SD card...
  WiFi SSID: MyHomeNetwork
  Wigle token: Set
  AutoUpload: true
  Donate: true
Configuration loaded successfully
```

## How It Works

### JIT Mounting (Banshee Boards)

On boards where the SD card and display share an SPI bus (like Wired Hatters Banshee), the system uses **Just-In-Time (JIT) mounting**:

1. Display SPI is suspended
2. SD card is mounted
3. Config file is read
4. Settings are saved to NVS
5. SD card is unmounted
6. Display SPI is resumed

This prevents display freezes during SD access.

### Standard Mounting

On boards with dedicated SPI buses, the SD card remains mounted during the entire operation.

### Settings Storage

All settings from config.cfg are saved to **Non-Volatile Storage (NVS)**, so they persist across reboots even if the SD card is removed.

## Use Cases

### Quick Device Setup
Set up multiple devices quickly by copying the same config.cfg to each SD card.

### Field Deployment
Pre-configure devices before deployment - no need to connect to CLI or navigate menus.

### Token Updates
Easily update WiGLE credentials across multiple devices by updating config.cfg.

### Backup Configuration
Keep a backup config.cfg with your preferred settings for easy restoration.

## Troubleshooting

### "SD mount failed"
- Ensure SD card is properly inserted
- Check that SD card is formatted (FAT32 recommended)
- Verify SD card is not corrupted

### "Config file not found"
- Verify file is named exactly `config.cfg` (case-sensitive)
- Ensure file is in the root directory of SD card, not in a subfolder
- Check that file path is `/mnt/ghostesp/config.cfg`

### "Failed to parse config"
- Check file format matches the example above
- Ensure no invalid characters in values
- Verify field names are spelled correctly

### Settings Not Applying
- Check NVS storage is not full (run `settings list` to verify CLI access)
- Verify values are valid (e.g., `true`/`false` for boolean fields)
- Try loading config again

## Example Configurations

### Minimal WiFi Only
```ini
SSID=MyNetwork
PASSKEY=MyPassword123
```

### WiGLE Only
```ini
EncodedForUseToken=QUlEMTIzNDU2Nzg5MDphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5eg==
AutoUpload=true
Donate=true
```

### Complete Configuration
```ini
#-------------
#-----WIFI----
#-------------
SSID=WarDriveNet
PASSKEY=SecurePassword456!

#------------
#------Wigle-
#------------
EncodedForUseToken=QUlEMTIzNDU2Nzg5MDphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5eg==
AutoUpload=true
Donate=true
```

### Legacy API Format
```ini
# Using legacy APIName:APIToken format
EncodedForUseToken=AID0123456789:abcdefghijklmnopqrstuvwxyz123456
AutoUpload=false
Donate=true
```

## Security Considerations

- Config file contains sensitive information (WiFi password, API tokens)
- Keep SD card secure when not in device
- Consider encrypting sensitive values if sharing config files
- Delete config.cfg after loading if you're concerned about physical access
- Remember: Settings persist in NVS after loading, so config.cfg can be removed

## Related Commands

| Command | Description |
|---------|-------------|
| `loadconfig` | Load settings from config.cfg on SD card |
| `wigle show` | Display current WiGLE settings |
| `wifistatus` | Display current WiFi connection and saved SSID status |
| `settings list` | List all available settings |

## See Also

- [WiGLE Upload Guide](../gps/wigle) - Complete WiGLE integration documentation
- [SD Card Setup](sd-card) - SD card formatting and mounting
- [WiFi Configuration](../wifi/) - WiFi setup and management
