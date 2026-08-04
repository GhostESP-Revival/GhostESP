# HackChat

HackChat is a native GhostESP app for short-range, device-to-device text messages over ESP-NOW. Each device uses its deterministic Ghostchi identity, generated from its WiFi STA MAC address.

Both devices must run a GhostESP build that includes the ESP-NOW SDK APIs and open HackChat. The app disconnects a connected WiFi STA while it runs, uses 2.4 GHz channel 1 for consistent nearby discovery, and reconnects using the saved GhostESP WiFi settings when it exits. HackChat automatically announces every three seconds and lists peers as they reply.

ESP-NOW messaging in this app is plaintext. Do not send secrets. HackChat refuses to start while WiFi monitoring is active so it does not disrupt capture workflows.

Build the app for the matching GhostESP target with `gbt build` and package it with `gbt package`. The checked-in manifest targets ESP32-C5; change `target` to the target being built before packaging an ESP32-S3 variant.
