---
title: "FAQ"
weight: 130
description: Frequently asked questions about GhostESP setup, credentials, and troubleshooting.
---

# Frequently Asked Questions

---

### 1. What Are the Default Network Credentials?
- **SSID:** `GhostNet`
- **Password:** `GhostNet`

---

### 2. What Are the Default Credentials for the Web Interface?
- Authentication is **off by default**.
- If you enable it with `webauth on`, the defaults are:
  - **Username:** `GhostNet`
  - **Password:** `GhostNet`

---

### 3. How Do I Access the Web Interface?
- Connect to the `GhostNet` Wi-Fi network.
- Open your browser and visit:
  - [`ghostesp.local`](http://ghostesp.local)
  - or [`192.168.4.1`](http://192.168.4.1)

---

### 4. Why Don’t the Default Credentials Work for the Web Interface?
- The web interface uses the same credentials as your Wi-Fi AP.
- If you’ve changed your Wi-Fi SSID or password, your web interface credentials also change.
- To disable authentication, open the GhostESP terminal or a [serial console](https://ghostesp.net/serial) and run `webauth off`, then restart the device.

> <p class="note-heading"><strong>Note</strong></p>
> <p>Both the AP and web authentication require a password of at least 8 characters and treat an 8-character password the same way, so there is no length mismatch between the two.</p>



---

### 5. How Do I Flash My Board?
- See the [Installation Guide]({{< relref "installation-guide.md" >}}).
- Use the web flasher at: [https://ghostesp.net/flasher](https://ghostesp.net/flasher)

---

### 6. Can I Upload Custom Evil Portal HTML over Serial or from My Flipper Zero?
- **SD card:** Place custom HTML in `/ghostesp/evil_portal/portals` on your SD card.
- **Flipper Zero App:** You can upload simple HTML (max 2048 bytes) directly via the Flipper companion app.

---

### 7. My Board Isn’t Currently Supported. Will You Add Support?
- Unless something is said otherwise, no. Feel free to do it yourself-we accept and appreciate PRs.

---

### 8. Why Does My Connection to the GhostESP AP Drop When Issuing Wi-Fi Commands?
- The ESP32 can’t operate as both an Access Point and Wi-Fi Client at the same time.
  Switching modes will disconnect your device from the AP.

---

### 9. I’m Not Seeing Any Output When Connecting via Serial.
- The firmware is silent unless a command is running.
- Try sending the `help` command to verify your connection or reconnecting; sometimes you will have a bad connection.

---

### 10. Why Won’t My SD Card Work?
- "Generic" firmware builds include SD card support by default; use `sd_config` to see the pins or `sd_pins_spi <cs> <clk> <miso> <mosi>` to set SPI pins and `sd_save_config` to save the pin config to NVS.
- Ensure your SD card is formatted as **FAT32**.
- Try using a SanDisk brand SD card 32GB or less.

---

### 11. Do I Need to Install ESP-IDF or Build from Source?
- No. Flashing a released build needs nothing but a browser: see the [Installation Guide]({{< relref "installation-guide.md" >}}).
- Building from source is only required if you are changing firmware code or adding a board. See [Environment Setup]({{< relref "../development/environment-setup.md" >}}) if that is what you want to do.

---

### 12. Which Variant Do I Choose in the Web Flasher?
- The flasher asks for the **chip family**, not the board name. Look your board up in the **Chip** column of [Supported Hardware]({{< relref "supported-hardware.md" >}}).
- Choosing the wrong chip is the most common reason a board flashes but never boots.

---

### 13. I Already Flashed an Older Version. How Do I Update?
- See [Firmware Updates]({{< relref "firmware-updates.md" >}}) for the OTA and manual options available for your board.
