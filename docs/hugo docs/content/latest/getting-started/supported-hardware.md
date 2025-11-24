---
title: "Supported Hardware"
description: "Device compatibility matrix for GhostESP features"
weight: 15
toc: true
---

## Overview

GhostESP runs on a variety of ESP32 boards with varying feature support. This compatibility matrix helps you identify which features are available on your device.

## Compatibility Matrix

<style>
  .compat-table {
    --compat-bg: #ffffff;
    --compat-header-bg: #f5f5f5;
    --compat-text: #000000;
    --compat-border: #e0e0e0;
    --compat-hover: #f9f9f9;
    border-radius: 0.5rem;
    max-height: 70vh;
    overflow: auto;
    box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    background: var(--compat-bg);
    position: relative;
    isolation: isolate;
  }
  .compat-table table {
    margin: 0;
    width: 100%;
    border-collapse: separate;
    border-spacing: 0;
    min-width: 720px;
    color: var(--compat-text);
  }
  .compat-table th,
  .compat-table td {
    padding: 0.75rem;
    text-align: center;
    vertical-align: middle;
    border-bottom: 1px solid var(--compat-border);
  }
  .compat-table th:first-child,
  .compat-table td:first-child {
    text-align: left;
    position: sticky;
    left: 0;
    background: var(--compat-bg);
    box-shadow: 1px 0 0 var(--compat-border);
    z-index: 2;
  }
  .compat-table tbody tr:hover { background: var(--compat-hover); }
  .compat-table thead th {
    position: sticky;
    top: 0;
    z-index: 1;
    background: var(--compat-header-bg);
    box-shadow: 0 1px 2px rgba(0,0,0,0.25);
  }
  .compat-table thead th:first-child {
    left: 0;
    z-index: 3;
    background: var(--compat-header-bg);
  }

  :where([data-theme="dark"], html[data-bs-theme="dark"], body[data-bs-theme="dark"], [data-bs-theme="dark"], html.dark, body.dark, .dark-mode, .theme-dark) .compat-table {
    --compat-bg: #1a1a1a;
    --compat-header-bg: #2d2d2d;
    --compat-text: #ffffff;
    --compat-border: #3d3d3d;
    --compat-hover: #252525;
  }
  :where([data-theme="light"], html[data-bs-theme="light"], body[data-bs-theme="light"], [data-bs-theme="light"], html.light, body.light, .light-mode, .theme-light) .compat-table {
    --compat-bg: #ffffff;
    --compat-header-bg: #f5f5f5;
    --compat-text: #000000;
    --compat-border: #e0e0e0;
    --compat-hover: #f9f9f9;
  }

  @media (prefers-color-scheme: dark) {
    :root:not([data-theme]) .compat-table {
      --compat-bg: #1a1a1a;
      --compat-header-bg: #2d2d2d;
      --compat-text: #ffffff;
      --compat-border: #3d3d3d;
      --compat-hover: #252525;
    }
  }

  .vendor-table {
    --vendor-bg: #ffffff;
    --vendor-header-bg: #f5f5f5;
    --vendor-text: #000000;
    --vendor-border: #e0e0e0;
    --vendor-hover: #f9f9f9;
    border-radius: 0.5rem;
    overflow: auto;
    box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    background: var(--vendor-bg);
    position: relative;
    isolation: isolate;
  }
  .vendor-table table {
    margin: 0;
    width: 100%;
    border-collapse: separate;
    border-spacing: 0;
    min-width: 600px;
    color: var(--vendor-text);
  }
  .vendor-table th,
  .vendor-table td {
    padding: 0.75rem;
    text-align: left;
    vertical-align: middle;
    border-bottom: 1px solid var(--vendor-border);
  }
  .vendor-table th:first-child,
  .vendor-table td:first-child {
    position: sticky;
    left: 0;
    background: var(--vendor-bg);
    box-shadow: 1px 0 0 var(--vendor-border);
    z-index: 2;
  }
  .vendor-table tbody tr:hover { background: var(--vendor-hover); }
  .vendor-table thead th {
    position: sticky;
    top: 0;
    z-index: 1;
    background: var(--vendor-header-bg);
    box-shadow: 0 1px 2px rgba(0,0,0,0.25);
  }
  .vendor-table thead th:first-child {
    left: 0;
    z-index: 3;
    background: var(--vendor-header-bg);
  }
  .vendor-table img {
    max-width: 150px;
    max-height: 100px;
    object-fit: contain;
  }

  :where([data-theme="dark"], html[data-bs-theme="dark"], body[data-bs-theme="dark"], [data-bs-theme="dark"], html.dark, body.dark, .dark-mode, .theme-dark) .vendor-table {
    --vendor-bg: #1a1a1a;
    --vendor-header-bg: #2d2d2d;
    --vendor-text: #ffffff;
    --vendor-border: #3d3d3d;
    --vendor-hover: #252525;
  }
  :where([data-theme="light"], html[data-bs-theme="light"], body[data-bs-theme="light"], [data-bs-theme="light"], html.light, body.light, .light-mode, .theme-light) .vendor-table {
    --vendor-bg: #ffffff;
    --vendor-header-bg: #f5f5f5;
    --vendor-text: #000000;
    --vendor-border: #e0e0e0;
    --vendor-hover: #f9f9f9;
  }

  @media (prefers-color-scheme: dark) {
    :root:not([data-theme]) .vendor-table {
      --vendor-bg: #1a1a1a;
      --vendor-header-bg: #2d2d2d;
      --vendor-text: #ffffff;
      --vendor-border: #3d3d3d;
      --vendor-hover: #252525;
    }
  }
</style>

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
      <tr><th scope="row">Flipper JCMK GPS</th><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S2 (generic)</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C5 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C6 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
    </tbody>
  </table>
</div>

## Vendor Boards

The following table lists the vendor-specific boards supported by GhostESP with their corresponding build names:

<div class="vendor-table">
  <table>
    <thead>
      <tr>
        <th>Board Name</th>
        <th>Build Name</th>
        <th>Image</th>
      </tr>
    </thead>
    <tbody>
      <tr><td>CYD2USB</td><td><code>CYD2USB.zip</code></td><td><img src="../images/x.jpeg" alt="CYD2USB"></td></tr>
      <tr><td>CYDMicroUSB</td><td><code>CYDMicroUSB.zip</code></td><td></td></tr>
      <tr><td>CYDDualUSB</td><td><code>CYDDualUSB.zip</code></td><td></td></tr>
      <tr><td>CYD2432S028R</td><td><code>CYD2432S028R.zip</code></td><td><img src="../images/CYD2432S028R.jpg" alt="CYD2432S028R"></td></tr>
      <tr><td>CYD 2.4″ variants</td><td><code>CYD2USB2.4Inch.zip</code> or <code>CYD2USB2.4Inch_C.zip</code></td><td><img src="" alt="CYD 2.4″"></td></tr>
      <tr><td>Waveshare 7″</td><td><code>Waveshare_LCD.zip</code></td><td></td></tr>
      <tr><td>Crowtech 7″</td><td><code>Crowtech_LCD.zip</code></td><td></td></tr>
      <tr><td>Sunton 7″</td><td><code>Sunton_LCD.zip</code></td><td></td></tr>
      <tr><td>Cardputer</td><td><code>ESP32-S3-Cardputer.zip</code></td><td><img src="../images/m5_cardputer.jpg" alt="M5 Stack Cardputer"></td></tr>
      <tr><td>Cardputer ADV</td><td><code>CardputerADV.zip</code></td><td></td></tr>
      <tr><td>MarauderV4</td><td><code>MarauderV4_FlipperHub.zip</code></td><td></td></tr>
      <tr><td>MarauderV6 & AwokDual</td><td><code>MarauderV6_AwokDual.zip</code></td><td></td></tr>
      <tr><td>AwokMini</td><td><code>AwokMini.zip</code></td><td></td></tr>
      <tr><td>Awok V5</td><td><code>esp32v5_awok.zip</code></td><td></td></tr>
      <tr><td>T-Display S3 Touch</td><td><code>LilyGo-TDisplayS3-Touch.zip</code></td><td></td></tr>
      <tr><td>S3TWatch</td><td><code>LilyGo-S3TWatch-2020.zip</code></td><td></td></tr>
      <tr><td>TEmbed CC1101</td><td><code>LilyGo-TEmbedC1101.zip</code></td><td><img src="../images/lilygo_tembed_cc1101.jpg" alt="Lily Go Tembed cc1101"></td></tr>
      <tr><td>GhostBoard</td><td><code>ghostboard.zip</code></td><td><img src="../images/rabbit_labs_ghost_board_black.jpg" alt="Black Rabbit Labs Ghost Board"></td></tr>
      <tr><td>T-Deck</td><td><code>LilyGo-T-Deck.zip</code></td><td><img src="../images/lilygo_tdeck_plus.jpg" alt="LilyGo T-Deck Plus"></td></tr>
      <tr><td>JCMK DevBoardPro</td><td><code>JCMK_DevBoardPro.zip</code></td><td></td></tr>
      <tr><td>RabbitLabs Minion</td><td><code>RabbitLabs_Minion.zip</code></td><td><img src="../images/rabbit_labs_minion.jpg" alt="Rabbit Labs Minion"></td></tr>
      <tr><td>RabbitLabs Phantom</td><td><code>CYD2USB2.4Inch.zip</code></td><td><img src="../images/rabbit_labs_phantom.jpg" alt="Rabbit Labs Phantom"></td></tr>
      <tr><td>Lolin S3 Pro</td><td><code>Lolin_S3_Pro.zip</code></td><td><img src="../images/lolin_s3_pro.jpg" alt="Lolin S3 Pro"></td></tr>
      <tr><td>Flipper JCMK GPS</td><td><code>Flipper_JCMK_GPS.zip</code></td><td><img src="../images/flipper_wifi_devboard.jpg" alt="Flipper Wifi Dev Board + JCMK GPS Mod"></td></tr>
      <tr><td>JC3248W535EN</td><td><code>JC3248W535EN_LCD.zip</code></td><td></td></tr>
      <tr><td>Wired Hatters ESPRocket</td><td><code>esp32-generic.zip</code></td><td><img src="../images/wired_hatters_rocket.jpg" alt="Wired Hatters ESPRocket"></td></tr>
      <tr><td>Wired Hatters Ultimate Marauder</td><td>Red Port: <code>esp32-generic.zip</code> and Blue Port: <code>MarauderV4_FlipperHub.zip</code></td><td><img src="../images/wired_hatters_ultimate_marauder.jpg" alt="Wired Hatters Ultimate Marauder"></td></tr>
    </tbody>
  </table>
</div>

> **Note:** Images are being added as they become available.
