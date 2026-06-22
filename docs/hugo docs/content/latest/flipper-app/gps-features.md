---
title: "GPS Features"
description: "GPS tracking and wardriving capabilities in the Flipper Zero companion app."
weight: 50
keywords: ["gps", "wardriving", "location", "tracking", "coordinates"]
---

## GPS Information

- **Real-time Position**: View current latitude and longitude
- **Altitude Monitoring**: Track elevation above sea level
- **Speed Tracking**: Monitor current movement speed
- **Direction**: Display heading/bearing
- **Signal Quality**: View GPS signal strength and satellite count
- **Satellite Status**: See connected satellites

## Wardriving Capabilities

- **Wi-Fi Wardriving**: Log Wi-Fi networks with GPS coordinates (CSV export)
- **BLE Wardriving**: Log BLE devices with GPS coordinates (CSV export)
- **Combined Mapping**: Map both networks and devices on a single view

## Offline map tiles (device SD)

Display firmware can show **cached map imagery** from the SD card (not live map downloads on the device). Open **Apps** → **Maps** after copying Slippy tiles under `/mnt/ghostesp/maps/<style>/tiles/{z}/{x}/{y}.png` (or `.jpg` / `.jpeg`), where each **`<style>`** folder under `maps/` contains its own `tiles/` tree (multiple styles are supported; cycle with the on-map control, **`M`**, or the encoder button). PNGs are decoded by LVGL; JPEGs are decoded to RGB565 with stb when using 16-bit color, with an SJPG fallback for baseline JPEG. Optional `metadata.json` can supply bounds; **zoom limits follow the tile folders on disk** when the card can be scanned (so template min/max in JSON does not cap the viewer). Details: [Offline maps (SD tiles)](../maps/<Style>).

## Common Workflow: GPS Wardriving

1. Ensure GPS module is connected and receiving signal
2. Navigate to **GPS** → **Wardriving** → **WiFi Wardriving**
3. Start wardriving session
4. Drive around to collect data
5. Stop and export CSV file with GPS coordinates

## GPS Troubleshooting

**GPS not working**
- Verify GPS module is connected and powered
- Ensure GPS has clear view of sky
- Wait for GPS to acquire satellite lock (may take 1-2 minutes)
- Check GPS module wiring and configuration
