---
title: "Evil Portal"
description: "Host a fake Wi-Fi login page to test security awareness."
weight: 40
---

Create a fake Wi-Fi network that shows a login page when users connect.

> **Note**: Only test this on networks you own or have explicit permission to test. Unauthorized access to networks is illegal in most jurisdictions.

## Prerequisites
- SD card inserted and mounted.
- Portal files saved to the SD card (optional; there's a default portal built in).
- Custom portal files go in `/mnt/ghostesp/evil_portal/portals/` on the SD card. Place your HTML (and any referenced assets) there so `listportals` can find them.

## Starting a portal

### On-device UI
1. Open **WiFi → Evil Portal → Start Evil Portal**.
   The device will launch the built-in default portal.
2. To use a custom HTML page, choose **Start Custom Evil Portal** instead.
   Select your page and enter the network name and optional password.
3. The portal is now running. Clients connecting to the network will see a login page.
4. To stop, go back to the menu or run `stopportal` in the terminal.

### From the Flipper app
1. On the Flipper app, open **WiFi → Evil Portal & Network → Set Evil Portal HTML**.
2. Pick your HTML file (up to 2048 bytes) from the file browser; it is sent and stored for the next start.
3. Back in **Evil Portal**, choose **Evil Portal** to run the command. The UI appends `startportal` and prompts for arguments.
   - Command format: `startportal file/default SSID PSK`.
   - SSID/PSK should be entered without spaces (use dashes/underscores instead) or the command parser will split them incorrectly.
   - If you already pushed HTML from the Flipper app, enter `default` for `<file>` to use the stored page automatically.
4. Clients connecting to the network will see your uploaded portal.

### Command line
1. Run `listportals` to see available portal pages.
2. Run `startportal default MyNetworkName` to start with the built-in portal.
   Or use `startportal mypage.html MyNetworkName` for a custom page.
3. (Optional) Add a password: `startportal mypage.html MyNetworkName MyPassword`.
   - Avoid spaces in SSID/PSK here as well; use dashes/underscores so the CLI doesn't misparse arguments.
4. Run `stopportal` or `stop` to shut it down.

## What gets recorded
- Submitted credentials are saved to `/mnt/ghostesp/evil_portal/portal_creds_<n>.txt` on the SD card.
- Keystrokes are logged to `/mnt/ghostesp/evil_portal/portal_keystrokes_<n>.txt`.

## Testing the portal
1. Connect to the network from another device.
2. Open a web browser and navigate to any website.
   You should see the login page instead.
3. Submit test credentials.
   Check the SD card files to confirm they were recorded.

## Tips
- Keep custom portal pages simple and small for faster loading.
- Use a card reader to transfer files to/from the SD card quickly.

## Troubleshooting
- **No portal pages found**: Make sure the SD card is mounted and has a `/mnt/ghostesp/evil_portal/portals/` folder.
- **Credentials not being saved**: Verify the SD card has free space and is properly mounted.
- **Clients don't see the login page**: Try opening a new browser tab or clearing the browser cache on the client device. Also make sure any 'Private DNS' or similar setting on the client is turned off.
