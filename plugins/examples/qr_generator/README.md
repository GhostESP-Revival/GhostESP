# QR Generator

Native SD app example that turns text and URLs into QR codes. Its compact
options menu works across screen sizes; selecting Edit opens the firmware
keyboard and then switches to a dedicated, full-size QR preview.

## Controls

- Select **Edit text or URL**, then submit the firmware keyboard to open the preview.
- Select **Show QR code** or press `P` to preview the current payload.
- Select **Reset to GhostESP URL** or press `R` to restore the default payload.
- Use the D-pad or encoder to select menu rows.
- Swipe right, press `Back`, `Esc`, `Delete`, or `Q` to return from the preview or exit.

The renderer always reserves a four-module white quiet zone and sizes the QR
code from the active content area. The preview is a square canvas that uses the
full available content area in both portrait and landscape.

## Build

```powershell
python plugins/tools/build_app.py plugins/examples/qr_generator --target esp32c5
python plugins/tools/package_app.py plugins/examples/qr_generator --gapp
```

The current app accepts up to 120 bytes, keeping the code scannable on compact
screens while covering typical URLs and pairing payloads.
