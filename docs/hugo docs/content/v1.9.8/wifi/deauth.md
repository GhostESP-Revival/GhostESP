---
title: "Deauthentication Attacks"
description: "Force devices to disconnect from a Wi-Fi network."
weight: 25
---

Deauthentication (deauth) attacks force devices to disconnect from a Wi-Fi network. This is useful for testing network resilience or capturing authentication handshakes when devices reconnect.

> **Note**: Only perform deauth attacks on networks you own or have explicit permission to test. Disrupting networks without authorization is illegal.

## How it works

Wi-Fi networks use management frames to coordinate connections. Two of these frames can terminate a connection:

- **Deauthentication frame**: Ends the authentication relationship
- **Disassociation frame**: Ends the association relationship

GhostESP sends both frame types. Since these frames are part of the original Wi-Fi standard (802.11), most devices have no way to verify if the message is legitimate.

### Broadcast and targeted attacks

The attack sends frames in two ways:

1. **Broadcast** (`FF:FF:FF:FF:FF:FF`): Hits all devices on the network without needing to know who's connected
2. **Targeted**: If you've scanned for stations (`scansta`), GhostESP automatically includes any discovered clients associated with the selected AP

### Bidirectional frames

For targeted stations, GhostESP sends frames in both directions:

- **AP → Station**: Appears to come from the access point, telling the device to disconnect
- **Station → AP**: Appears to come from the device, telling the AP the device is leaving

This makes the attack more reliable since both sides think the connection ended.

## Frequency band limitations

### 2.4 GHz only (most boards)

Most ESP32 boards (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6) only support **2.4 GHz Wi-Fi**. This means:

- You can only deauth networks on channels 1–14
- 5 GHz networks are invisible and unaffected
- Many modern routers use both bands—devices on the 5 GHz band will stay connected

### 5 GHz support (ESP32-C5)

The **ESP32-C5** supports both 2.4 GHz and 5 GHz bands, allowing you to:

- Scan and attack networks on all Wi-Fi channels
- Target devices on 5 GHz networks

If your target network uses 5 GHz, you need an ESP32-C5 based device.

## Protected Management Frames (PMF)

**PMF** (also called MFP or 802.11w) is a security feature that cryptographically signs management frames. When enabled, devices can verify that deauth frames actually came from the real access point.

### Networks immune to deauth

- **WPA3 networks**: PMF is mandatory. Deauth attacks will not work.
- **WPA2/WPA3 mixed mode**: Devices using WPA3 are protected; WPA2 devices may still be vulnerable.
- **WPA2 with PMF enabled**: Some enterprise networks enable PMF on WPA2.

### How to check

When you scan networks on an ESP32-C5 or ESP32-C6, GhostESP shows the security type and PMF status:

```
SSID: MyNetwork
Band: 2.4GHz
Security: WPA3
PMF: Required
```

If you see `PMF: Required`, deauth attacks will have no effect on that network.

## Prerequisites

- GhostESP device
- Target network on a supported frequency band (2.4 GHz for most boards, or 5 GHz with ESP32-C5)
- **Country code set** in device settings (see Troubleshooting if you get errors)

## Launching a deauth attack

### On-device UI

1. Open **Menu → WiFi → Scanning** and wait for the scan to complete.
2. Select your target with **Select AP**.
3. Open **Menu → WiFi → Attacks → Deauth**.
4. The device will continuously send deauth frames to all clients on that network.
5. To stop, navigate to **Menu → WiFi → Attacks → Stop Attack** or run `stopdeauth`.

### Command line

1. Scan for networks:
   ```
   scanap
   ```
2. List results:
   ```
   list -a
   ```
3. Select your target (replace `1` with the network number):
   ```
   select -a 1
   ```
4. Start the attack:
   ```
   attack -d
   ```
5. Stop the attack:
   ```
   stopdeauth
   ```

### Targeting multiple networks

You can select multiple access points at once:

```
select -a 1,3,5
attack -d
```

GhostESP groups APs by channel to minimize channel-switching overhead. APs on the same channel are attacked together before moving to the next channel.

## Troubleshooting

### ESP_ERR_INVALID_ARG when deauthing

This error occurs when attempting to deauth a network on a channel not allowed by your current country code setting. By default, GhostESP uses "World Safe" mode (`01`) which only permits channels 1–11.

If your target network is on channel 12, 13, or 14, you must set the correct country code first.

**Fix via on-device UI**:

1. Open **Menu → Settings → WiFi Country**.
2. Select your region.
3. Restart the device.

**Fix via CLI** (ESP32-C5 only):

```
setcountry <CC>
```

Replace `<CC>` with one of the supported country codes:

| Code | Region | Code | Region | Code | Region |
|------|--------|------|--------|------|--------|
| `01` | World Safe | `AT` | Austria | `AU` | Australia |
| `BE` | Belgium | `BG` | Bulgaria | `BR` | Brazil |
| `CA` | Canada | `CH` | Switzerland | `CN` | China |
| `CY` | Cyprus | `CZ` | Czechia | `DE` | Germany |
| `DK` | Denmark | `EE` | Estonia | `ES` | Spain |
| `FI` | Finland | `FR` | France | `GB` | United Kingdom |
| `GR` | Greece | `HK` | Hong Kong | `HR` | Croatia |
| `HU` | Hungary | `IE` | Ireland | `IN` | India |
| `IS` | Iceland | `IT` | Italy | `JP` | Japan |
| `KR` | South Korea | `LI` | Liechtenstein | `LT` | Lithuania |
| `LU` | Luxembourg | `LV` | Latvia | `MT` | Malta |
| `MX` | Mexico | `NL` | Netherlands | `NO` | Norway |
| `NZ` | New Zealand | `PL` | Poland | `PT` | Portugal |
| `RO` | Romania | `SE` | Sweden | `SI` | Slovenia |
| `SK` | Slovakia | `TW` | Taiwan | `US` | United States |

**Channel availability by region**:
- **US, Canada** (`US`, `CA`): Channels 1–11
- **Europe, Australia** (`GB`, `DE`, `AU`, etc.): Channels 1–13
- **Japan** (`JP`): Channels 1–14

### Attack has no effect

- **Check the band**: If the network is on 5 GHz and you're using a 2.4 GHz-only board, the attack won't work.
- **Check for PMF**: WPA3 networks and some WPA2 networks with PMF enabled are immune.
- **Multiple APs**: Some networks have multiple access points. You may need to target each one separately.
- **Distance**: Get closer to the access point for a stronger signal.

### Devices reconnect immediately

Devices reconnect immediately after being kicked. If they keep getting back on:

- Make sure you're only targeting one or two networks
- Move closer to the access point
- Check for other interference

### Cannot see 5 GHz networks

Your board only supports 2.4 GHz. Use an ESP32-C5 based device to access 5 GHz networks.

## Related attacks

- **EAPOL Logoff** (`attack -e`): Sends logoff frames instead of deauth frames. Works similarly but uses a different frame type.
- **SAE Flood** (`attack -s`): Floods WPA3 networks with authentication requests. Only available on ESP32-C5/C6.
