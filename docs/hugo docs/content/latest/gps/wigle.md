---
title: "WiGLE Upload"
description: "Configure WiGLE API integration for uploading wardriving data"
weight: 10
---

Upload your wardriving CSV captures to [WiGLE.net](https://wigle.net) to contribute to the global wireless network mapping project.

## Prerequisites

- GhostESP device with GPS module connected
- SD card mounted for storing CSV captures
- Wi-Fi connection (for uploading)
- WiGLE account with API credentials

## Setup

Get your API credentials from [wigle.net/account](https://wigle.net/account) - create a free account at [wigle.net](https://wigle.net) first if you need one. Either copy the **"Encoded for Use"** token (recommended) or the **API Name** and **API Token** in legacy `APIName:APIToken` form.

Set the API key:
```
wigle API <EncodedForUseToken>
```
Or with legacy credentials:
```
wigle API <APIName>:<APIToken>
```

Then, recommended:
```
wigle auto on
```
```
wigle donate on
```

Verify:
```
wigle show
```

On display builds the same options live under **Menu → Settings → WiGLE**:

1. Toggle **Auto Upload** to enable automatic uploads when Wi-Fi (STA) connects.
2. Toggle **Donate Data** to share your scans with WiGLE (recommended).
3. Use **Manual Upload** to browse CSV files in `/mnt/ghostesp/gps/`.
4. Select a CSV to view details (file name, Wi-Fi row count, total row count), then press **Upload**.

## CLI Commands

| Command | Description |
|---------|-------------|
| `wigle API <encoded>` | Set WiGLE Encoded for Use token (recommended) |
| `wigle API <name>:<token>` | Set WiGLE API credentials (legacy format) |
| `wigle auto on/off` | Enable/disable auto-upload on Wi-Fi (STA) connect |
| `wigle donate on/off` | Enable/disable data donation |
| `wigle show` | Display current settings |
| `wigle list` | List previously uploaded files |
| `wigle files [page]` | List CSV files in `/mnt/ghostesp/gps/` (8 per page) |
| `wigle upload <filename>` | Upload one CSV file manually |
| `wigle upload` | With no filename, behaves like `wigle upload all` |
| `wigle upload all` | Upload all pending queued files |
| `wigle stats` | Show WiGLE account stats for current credentials |

## Auto Upload

When **Auto Upload** is enabled, GhostESP automatically starts `wigle upload all` when:

1. The device gets a Wi-Fi STA IP address
2. An API key is configured
3. There are queued/pending CSV files

The auto-upload task starts in the background shortly after connect, so the CLI/UI stays responsive.

## Queueing and Duplicate Protection (Important)

GhostESP uses two small state files on SD:

- Upload queue: `/mnt/ghostesp/.wigle_queue`
- Uploaded memory: `/mnt/ghostesp/.wigle_uploaded`

How this works:

1. Wardriving CSV files are queued after capture/close
2. Upload worker reads queue and validates each CSV
3. Successful uploads are recorded in uploaded-memory as `basename,size`
4. Failed uploads stay queued for retry

Duplicate prevention key is **file basename + size**.

- Same name and same size: skipped as already uploaded
- Renamed file (different basename): treated as a different upload record

## Manual Upload

List available CSVs (page 1 by default):
```
wigle files
```

List another page:
```
wigle files 2
```

Upload a single file:
```
wigle upload wardriving_12.csv
```

Upload all queued files:
```
wigle upload all
```

Running `wigle upload` with no filename behaves the same as `wigle upload all`.

Manual upload checks:

- Filename safety (must be a CSV basename, no path traversal)
- File exists and is not empty
- WiGLE CSV pre-header present (`WigleWifi-1.6...`)
- At least one data row exists (header-only files are skipped)

The display menu and CLI return immediate success/failure feedback.

## View WiGLE Stats

Fetch user stats for the current account:
```
wigle stats
```

The same stats call is available from **Settings → WiGLE → View WiGLE Stats** on display builds.

**Note:** Stats now work with both EncodedForUseToken and legacy APIName:APIToken formats.

## Data Donation

When **Donate Data** is enabled (default), your uploads contribute to WiGLE's public database. This helps:

- Map wireless networks globally
- Research wireless security trends
- Improve coverage in undermapped areas

You can disable this if you prefer private uploads (WiGLE Pro feature).

## CSV Format

GhostESP generates WiGLE-compatible CSV files in the standard format:

```
WigleWifi-1.6,appRelease=GhostESP <version>,model=<board>,release=<version>,device=GhostESP,display=NONE,board=<board>,brand=GhostESP,star=Sol,body=3,subBody=0
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type
```

Files are saved to `/mnt/ghostesp/gps/` on your SD card.

Typical names:

- `wardriving_<n>.csv`
- `ble_wardriving_<n>.csv`

## SD Card Config Loader

WiGLE credentials and Wi-Fi settings can also be loaded from `config.cfg` on the SD card. See [SD card configuration]({{< relref "sd-card-config.md" >}}) for the file format and loading steps.

## Troubleshooting

- **"no API key set"**: Run `wigle API <encoded>` or `wigle API <name>:<token>` to configure credentials
- **Upload fails**: Ensure device is connected to Wi-Fi STA mode and has internet access
- **Files not uploading**: Check that CSV files exist in `/mnt/ghostesp/gps/`
- **"CSV has no data rows"**: Usually means capture had no accepted GPS rows (for example no valid fix)
- **Duplicate uploads**: Upload memory tracks files by `basename,size` to prevent re-uploading the same file

## Notes

- Uploads require active Wi-Fi STA connectivity (AP mode alone is not enough)
- Large uploads may take time depending on file size and connection speed
- Upload progress is shown in the terminal/logs
- Files remain on SD card after successful upload
