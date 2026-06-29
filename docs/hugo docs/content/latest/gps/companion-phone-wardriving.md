---
title: "Companion Phone GPS Wardriving"
description: "Use your phone's GPS to wardrive with the GhostESP Companion app"
weight: 10
toc: true
---

The GhostESP Companion app supports **Phone GPS Wardriving**, a mode that uses your phone's built-in GPS to tag Wi-Fi access points observed by the connected GhostESP device. This is useful when your GhostESP board does not have a GPS module attached.

## How It Works

1. The Companion app streams wardrive AP observations from the GhostESP device over the active connection (USB or BLE)
2. Each observed AP is tagged with the phone's current GPS coordinates
3. When you stop the session, a WiGLE-compatible CSV file is saved to your phone's Downloads folder
4. The CSV can be browsed, shared, or deleted from the built-in CSV explorer

## Requirements

- GhostESP device connected via USB or wireless bridge
- Phone location permission granted
- Phone GPS enabled with a valid fix (outdoor or near a window for best results)

## Starting Phone GPS Wardriving

1. Open the **GPS / Wardrive** screen in the Companion app
2. Ensure your GhostESP device is connected
3. Toggle the **Phone GPS Wardrive** switch to ON
4. Move through your route — APs will appear as circular markers on the map, colored by RSSI strength
5. Toggle the switch to OFF when done — the CSV is saved automatically

## Map Display

The map shows individual access points as small circles:

- **Color**: Ranges from red (weak RSSI, around -100 dBm) to blue (strong RSSI, around -40 dBm)
- **Size**: Scales with zoom level (3–14 px radius)
- **Stroke**: Outline appears at zoom level 15 and above

Your phone's current GPS position is shown as a blue marker, and the device's GPS position (if available) is shown as a red marker.

## CSV Explorer

The **Saved CSVs** button in the bottom card opens a bottom sheet listing all Phone GPS Wardrive CSV files saved to your phone's Downloads folder. From here you can:

- **Refresh** the list to pick up newly saved files
- **Share** a CSV via the system share sheet (email, messaging, cloud storage, etc.)
- **Delete** a CSV with confirmation

## CSV Format

Phone GPS Wardrive CSVs use the **WiGLE v1.6** format, matching the firmware's on-device wardriving output:

```
WigleWifi-1.6,appRelease=GhostESP Companion,model=Android,release=,device=phone,display=NONE,board=Android,brand=GhostESP,star=Sol,body=3,subBody=0
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type
```

Each row contains:

| Field | Description |
|-------|-------------|
| MAC | BSSID of the access point |
| SSID | Network name (empty for hidden networks) |
| AuthMode | WiGLE capabilities string (e.g., `[WPA2-PSK][ESS]`) |
| FirstSeen | UTC timestamp (`yyyy-MM-dd HH:mm:ss`) |
| Channel | Wi-Fi channel number |
| Frequency | Computed frequency in MHz |
| RSSI | Signal strength in dBm |
| CurrentLatitude | GPS latitude (6 decimal places) |
| CurrentLongitude | GPS longitude (6 decimal places) |
| AltitudeMeters | Altitude rounded to integer meters |
| AccuracyMeters | GPS accuracy in meters |
| RCOIs | Empty (reserved by WiGLE format) |
| MfgrId | Empty (reserved by WiGLE format) |
| Type | Always `WIFI` |

Files are named `ghostesp_phone_wardrive_<timestamp>.csv` and saved to the phone's Downloads directory.

## Uploading to WiGLE

CSVs saved by Phone GPS Wardriving are fully WiGLE-compatible and can be uploaded to [wigle.net](https://wigle.net):

1. Use the **Share** button in the CSV explorer to send the file to your computer
2. Upload via the WiGLE website or desktop app

Future versions of the Companion app will support direct WiGLE upload from the CSV explorer.

## Next Step

For firmware-based wardriving with an attached GPS module, see [Wardriving](/latest/gps/wardriving/).
