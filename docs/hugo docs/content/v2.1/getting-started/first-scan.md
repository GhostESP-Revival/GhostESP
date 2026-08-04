---
title: "Try Your First Scan"
description: "Confirm GhostESP is working with a passive Wi-Fi scan."
weight: 40
toc: true
---

## What this does

This first task passively lists nearby Wi-Fi access points. It does not connect to a network or transmit anything.

## Before you start

- GhostESP is flashed and powered on.
- Use the on-device UI or a serial CLI connection. Do not run this scan from the WebUI on a single-board setup because the Wi-Fi radio is hosting `GhostNet`.
- Attach the board's supported Wi-Fi antenna if it has an external antenna connector.

## On-device steps

1. Open **Menu → WiFi → Scanning**.
2. Choose **Scan Access Points**.
3. Wait for the scan to complete.
4. Choose **List Access Points** to review the results.

## CLI steps

1. Open the GhostESP serial terminal.
2. Run `scanap`.
3. Wait for the scan to finish.
4. Run `list -a` to view the saved access-point list.

## Expected result

GhostESP lists nearby networks with their name, channel, signal strength, and manufacturer when available. If no networks appear, move closer to a known access point and try again.

## Troubleshooting

- **No networks found:** Confirm the board supports Wi-Fi, attach its antenna if applicable, move closer to an access point, and scan again.
- **The scan stops immediately:** Stop any active Wi-Fi task or portal, then retry.
- **The WebUI disconnects:** Run Wi-Fi scans from the on-device UI or serial CLI, not from the WebUI on a single-board setup.

## Related tasks

- [Scan nearby Wi-Fi networks]({{< relref "../wifi/survey.md" >}})
- [Connect GhostESP to a Wi-Fi network]({{< relref "../wifi/connect.md" >}})
- [Connect to GhostESP]({{< relref "control-methods.md" >}})
