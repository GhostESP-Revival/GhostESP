---
title: "Firmware Updates"
description: "How GhostESP firmware update checks, installs, peer updates and SD card updates work."
keywords: ["firmware", "ota", "updates", "sd card", "banshee", "ghostlink"]
weight: 120
toc: true
---

GhostESP supports several firmware update paths. The options shown on your device depend on the board, flash layout, network support, and whether a GhostLink peer is configured.

Open **Settings → Firmware Update** to access update actions.

## Update Channel

Use **Update Channel** to choose which release feed to check:

- **Stable** checks normal release firmware.
- **Prerelease** checks prerelease firmware first, then falls back to stable if no prerelease exists for your board.

## Device Firmware Updates

Use these options to update the device you are holding.

### Check Device Update

**Check Device Update** contacts the update server and checks whether firmware exists for this board.

If firmware is found, GhostESP shows the available version and build relation:

- **newer** means the available build is newer than the running build.
- **same build** means the available build matches the running build.
- **older** means the available build is older than the running build.
- **available** means the build number was not available or could not be compared.

GhostESP may still show a firmware image even when it is the same or older build. This is intentional so you can reinstall or roll back when needed.

### Install Update

**Install Update** installs this device's firmware only. It does not update a GhostLink peer.

Before the install starts, GhostESP shows a confirmation dialog with the target firmware and a power warning. Confirm only when the device can stay powered until the update finishes.

Boards with a normal dual-partition OTA layout download the firmware, write it to the inactive app slot, verify it, then reboot into the updated firmware.

## Banshee C5 Update Behavior

> **v2.1.2: Banshee C5 self-OTA disabled.** The `Banshee_C5` build previously used a dedicated updater app (reserve `updater` partition + 1.12 MB embedded updater) to work around the fixed native-app flash-XIP partition. The flow was unreliable and repeatedly pushed the app over flash (`app0 0x550000` overflow `0x17BF0` in v2.1.2). Self-OTA is disabled for this board for now; flash via USB using the [Installation Guide]({{< relref "installation-guide.md" >}}) or the Web Flasher at https://ghostesp.net/flasher . OTA will return in a future release with a leaner layout.

<details><summary>Previous updater flow (historical, currently disabled)</summary>

Banshee C5 previously used a special updater flow because it cannot fit two full GhostESP app slots alongside its fixed native-app flash-XIP partition.

On Banshee C5 (when enabled):

- **Check Device Update** checked the Banshee C5 firmware manifest entry.
- **Install Update** stored the pending update request, switched boot to a small updater app, and rebooted.
- The updater app connected to Wi-Fi, downloaded the firmware, verified SHA-256, then rewrote `app0` while running from the updater partition.
- The main app partition was not touched until the updater had downloaded and verified the new firmware.

If the updater failed before touching `app0`, it recorded the error, switched boot back to `app0`, and rebooted into normal GhostESP. On boot, GhostESP showed an **Update Failed** popup with the updater error.

If failure happened after `app0` erase or write had started, the updater did not boot back to `app0` because the app could be incomplete. It stayed in updater recovery and retried after reboot. If it could not recover, USB/manual flashing was required.

Common safe failures included Wi-Fi connection failed, no pending update found, download failed, SHA-256 verification failed, and firmware image too large.

</details>

## Peer Firmware Updates

Some devices can update a paired GhostLink peer. Peer updates are separate from device updates.

### Check Peer Update

**Check Peer Update** checks firmware availability for the connected peer. It also asks the peer for its current build when possible, so the result can show whether the peer firmware is newer, older, or the same build.

If GhostLink is not connected, GhostESP shows a connection error instead of starting the check.

### Update Peer

**Update Peer** updates the connected peer only. It does not update the primary device afterward.

During a peer update, the primary downloads the peer firmware and streams it over GhostLink. The peer verifies the received firmware before committing it. If the stream fails, the peer is told to abort and remains on its current firmware.

## SD Card Firmware Updates

On boards with a normal dual-partition OTA layout, **Install from SD Card** can install firmware from the SD card.

The action is hidden on boards where SD-card OTA is not supported, including Banshee C5 (self-OTA disabled in v2.1.2).

To use SD-card OTA:

1. Copy the firmware image to the SD card as `/ghostesp/firmware_update.bin`.
2. Optional: copy a SHA-256 sidecar to `/ghostesp/firmware_update.sha256`.
3. Open **Settings → Firmware Update → Install from SD Card**.
4. Confirm the install dialog.
5. Keep the device powered until it reboots.

If `firmware_update.sha256` exists, GhostESP verifies the SD firmware before booting it. If the sidecar is missing, GhostESP flashes the SD firmware without a sidecar hash check.

## Temporary AP and Web UI Disable During Updates

On boards without PSRAM, internal RAM is too tight to run the Web UI at the same time as the OTA download. To keep the download from stalling or out-of-memory killing itself, GhostESP automatically turns off the access point and Web UI for the duration of the update, then turns it back on when the new firmware boots.

This only happens on no-PSRAM boards (for example, Cardputer ADV). Boards with PSRAM keep the AP and Web UI running through the whole update.

What you should expect on a no-PSRAM board during a network update:

- The `GhostNet` access point disappears as soon as the download starts.
- The Web UI at `http://192.168.4.1` becomes unreachable.
- When the new firmware boots successfully, the access point and Web UI come back automatically.
- The setting **Wi-Fi AP → Enable Access Point** is restored to its previous value (on or off) after the reboot.

What happens if the download is interrupted or fails:

- The access point and Web UI are restored before you return to the menu, without needing a reboot.
- A failed install does not leave the device in a no-AP state.

What you can do if the access point does not come back:

1. Reboot the device. The restore is persisted to NVS, so a normal boot reapplies it.
2. If it is still off, open **Settings → Wi-Fi → Enable Access Point** and turn it back on.
3. If a recent OTA install failed, reflash manually with USB using the [Installation Guide]({{< relref "installation-guide.md" >}}).

> Manual update checks (from **Settings → Firmware Update → Check Device Update**) are still available on no-PSRAM boards. Only the automatic background check that runs at boot is skipped, so the device stays responsive on a memory-constrained board.

## Recovery Tips

- If an update check says no firmware was found, confirm that **Update Channel** is set correctly.
- If install says no update is ready, run **Check Device Update** first.
- Banshee C5: self-OTA is disabled in v2.1.2 (see above); use USB/manual flashing. The updater failure popup only appears on builds with the updater partition (pre-v2.1.2).
- If the device cannot boot normal firmware after a failed update, reflash manually with USB using the [Installation Guide]({{< relref "installation-guide.md" >}}).
