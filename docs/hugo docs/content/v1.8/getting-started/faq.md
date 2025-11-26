---
title: "FAQ"
weight: 40
description: Frequently asked questions about GhostESP setup, credentials, and troubleshooting.
---

# Frequently Asked Questions

---

### 1. What are the default network credentials?
- **SSID:** `GhostNet`
- **Password:** `GhostNet`

---

### 2. What are the default credentials for the web interface?
- **Username:** `GhostNet`
- **Password:** `GhostNet`

---

### 3. How do I access the web interface?
- Connect to the `GhostNet` WiFi network.
- Open your browser and visit:
  - [`ghostesp.local`](http://ghostesp.local)
  - or [`192.168.4.1`](http://192.168.4.1)

---

### 4. Why don’t the default credentials work for the web interface?
- The web interface uses the same credentials as your WiFi AP.
- If you’ve changed your WiFi SSID or password, your web interface credentials also change.
- If you want to disable authentication entirely, open the GhostESP terminal or a [serial console](https://ghostesp.net/serial) and run `webauth off`, then restart the device.

> <p class="note-heading"><strong>Note</strong></p>
> <p>The firmware has a subtle password-length mismatch: the AP code falls back to the default password unless the saved password is longer than 8 characters, while web authentication treats passwords of length 8 as valid. If you set an exactly 8-character password you may see the web UI and AP use different credentials; use a password longer than 8 characters to avoid this.</p>



---

### 5. How do I flash my board?
- See the [Installation Guide]({{< relref "installation-guide.md" >}}).
- Use the web flasher at: [https://flasher.ghostesp.net/](https://flasher.ghostesp.net/)

---

### 6. Can I upload custom Evil Portal HTML over Serial or from my Flipper Zero?
- **SD Card:** Place custom HTML in `/ghostesp/evil_portal/portals` on your SD card.
- **Flipper Zero App:** You can upload simple HTML (max 2048 bytes) directly via the app (v1.4+ and firmware v1.7+).

---

### 7. My board isn’t currently supported. Will you add support?
- Unless something is said otherwise, no. Feel free to do it yourself—we accept and appreciate PRs.

---

### 8. Why does my connection to the GhostESP AP drop when issuing WiFi commands?
- The ESP32 can’t operate as both an Access Point and WiFi Client at the same time.
  Switching modes will disconnect your device from the AP.

---

### 9. I’m not seeing any output when connecting via Serial.
- The firmware is silent unless a command is running.
- Try sending the `help` command to verify your connection or reconnecting; sometimes you will have a bad connection.

---

### 10. Why won’t my SD card work?
- "Generic" firmware builds include SD card support by default; use `sd_config` to see the pins or `sd_pins_spi <cs> <clk> <miso> <mosi>` to set SPI pins and `sd_save_config` to save the pin config to SD card.
- Ensure your SD card is formatted as **FAT32**.
- Try using a SanDisk brand SD card 32GB or less.
