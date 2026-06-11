---
title: "Supported Hardware"
description: "Device compatibility matrix for GhostESP features"
weight: 15
toc: true
---

## Overview

GhostESP runs on a variety of ESP32 boards with varying feature support. This compatibility matrix helps you identify which features are available on your device.

## Compatibility Matrix

<div class="compat-table">
  <table>
    <thead>
      <tr>
        <th>Board</th>
        <th>Bluetooth</th>
        <th>NFC (PN532)</th>
        <th>NFC (Chameleon)</th>
        <th>IR TX</th>
        <th>IR RX</th>
        <th>GPS</th>
        <th>Keyboard</th>
        <th>Display</th>
        <th>SD Card</th>
      </tr>
    </thead>
    <tbody>
      <tr><th scope="row">CYD2USB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYDMicroUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYDDualUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYD2432S028R</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYD 2.4″ variants</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Waveshare 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Crowtech 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Sunton 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Cardputer</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Cardputer ADV</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">MarauderV4</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">MarauderV6</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">AwokMini</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Awok V5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">T-Display S3 Touch</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">S3TWatch</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>has 4MB vfs partition</td></tr>
      <tr><th scope="row">TEmbed C1101</th><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">GhostBoard</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">T-Deck</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">JCMK DevBoardPro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">RabbitLabs Minion</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Lolin S3 Pro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">XIAO ESP32-S3 Sense</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">XIAO ESP32-C5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">Flipper JCMK GPS</th><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S2 (generic)</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C5 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C6 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">Heltec V3</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>Status Display</td><td>✓</td></tr>
    </tbody>
  </table>
</div>

## Camera Support

- **XIAO ESP32-S3 Sense** currently has the dedicated camera-enabled build and supports the [Motion Detector]({{< relref "../camera/motion-detector.md" >}}) feature.
- The current motion detector implementation uses **QQVGA grayscale** capture for low RAM usage and fast comparisons.
- **XIAO ESP32-C5** is a supported board config, but it does not include the onboard camera motion detector path used by the S3 Sense build.

## Vendor Boards

The following table lists the vendor-specific boards supported by GhostESP with their corresponding build names:

| Board Name | Build Name | Image |
|---|---|---|
| CYDMicroUSB | `CYDMicroUSB.zip` | |
| CYDDualUSB | `CYDDualUSB.zip` | |
| CYD2432S028R | `CYD2432S028R.zip` | <img src="../images/CYD2432S028R.jpg" alt="CYD2432S028R"> |
| Waveshare 7″ | `Waveshare_LCD.zip` | |
| Crowtech 7″ | `Crowtech_LCD.zip` | |
| Sunton 7″ | `Sunton_LCD.zip` | |
| Cardputer | `ESP32-S3-Cardputer.zip` | <img src="../images/m5_cardputer.jpg" alt="M5 Stack Cardputer"> |
| Cardputer ADV | `CardputerADV.zip` | |
| MarauderV4 | `MarauderV4_FlipperHub.zip` | |
| MarauderV6 & AwokDual | `MarauderV6_AwokDual.zip` | |
| AwokMini | `AwokMini.zip` | |
| Awok V5 | `esp32v5_awok.zip` | |
| T-Display S3 Touch | `LilyGo-TDisplayS3-Touch.zip` | |
| S3TWatch | `LilyGo-S3TWatch-2020.zip` | |
| TEmbed CC1101 | `LilyGo-TEmbedC1101.zip` | <img src="../images/lilygo_tembed_cc1101.jpg" alt="Lily Go Tembed cc1101"> |
| GhostBoard | `ghostboard.zip` | <img src="../images/rabbit_labs_ghost_board_black.jpg" alt="Black Rabbit Labs Ghost Board"> |
| T-Deck | `LilyGo-T-Deck.zip` | <img src="../images/lilygo_tdeck_plus.jpg" alt="LilyGo T-Deck Plus"> |
| JCMK DevBoardPro | `JCMK_DevBoardPro.zip` | |
| RabbitLabs Minion | `RabbitLabs_Minion.zip` | <img src="../images/rabbit_labs_minion.jpg" alt="Rabbit Labs Minion"> |
| RabbitLabs Phantom | `CYD2USB2.4Inch.zip` | <img src="../images/rabbit_labs_phantom.jpg" alt="Rabbit Labs Phantom"> |
| Lolin S3 Pro | `Lolin_S3_Pro.zip` | <img src="../images/lolin_s3_pro.jpg" alt="Lolin S3 Pro"> |
| Seeed Studio XIAO ESP32-S3 Sense | `xiao_esp32s3_sense.zip` | |
| Seeed Studio XIAO ESP32-C5 | `xiao_esp32c5.zip` | |
| Flipper JCMK GPS | `Flipper_JCMK_GPS.zip` | <img src="../images/flipper_wifi_devboard.jpg" alt="Flipper Wifi Dev Board + JCMK GPS Mod"> |
| JC3248W535EN | `JC3248W535EN_LCD.zip` | |
| Wired Hatters ESPRocket | `esp32-generic.zip` | <img src="../images/wired_hatters_rocket.jpg" alt="Wired Hatters ESPRocket"> |
| Wired Hatters Ultimate Marauder | Red Port: `esp32-generic.zip` and Blue Port: `MarauderV4_FlipperHub.zip` | <img src="../images/wired_hatters_ultimate_marauder.jpg" alt="Wired Hatters Ultimate Marauder"> |
| Heltec V3 | `heltecv3.zip` | |

> **Note:** Images are being added as they become available.
