# Karma Attack Guide

The Karma attack is a powerful WiFi attack technique that responds to probe requests from nearby devices by creating fake access points with the SSIDs that devices are looking for. This guide explains how to use the karma functionality in GhostESP.

## What is Karma Attack?

Karma attack works by:
1. **Listening for probe requests** from nearby devices (phones, laptops, IoT devices)
2. **Learning SSIDs** that devices are searching for
3. **Creating fake access points** with those SSIDs
4. **Starting evil portals** to capture credentials or perform other attacks
5. **Rotating between SSIDs** to maximize effectiveness

## How to Use Karma

### Command Line Interface

#### Start Karma with Auto-Learning
```
karma start
```
This starts karma in learning mode - it will automatically discover and cache SSIDs from probe requests.

#### Start Karma with Custom SSIDs
```
karma start SSID1 SSID2 SSID3
```
Example: `karma start FreeWiFi Starbucks McDonald's`

#### Stop Karma Attack
```
karma stop
```

### Touch Interface (GUI)

1. Navigate to **WiFi** menu
2. Select **Attacks**
3. Choose from:
   - **"Start Karma Attack"** - Uses automatic SSID learning
   - **"Start Karma Attack (Custom SSIDs)"** - Enter specific SSIDs manually
   - **"Stop Karma Attack"** - Stops the attack

## How Karma Works

### Automatic SSID Learning
- Karma listens for probe requests on all WiFi channels
- When a device probes for a network, karma caches that SSID
- Cached SSIDs are used to create fake access points
- Maximum of 64 SSIDs can be cached

### SSID Rotation
- If multiple SSIDs are cached, karma rotates between them every 5 seconds
- This increases the chance of catching devices looking for different networks
- Each rotation creates a new fake AP with the current SSID

### Evil Portal Integration
- Each fake AP automatically starts an evil portal
- Users who connect see a captive portal page
- Credentials and other data can be captured

### Beacon Broadcasting
- Karma broadcasts beacon frames for all cached SSIDs every 500ms
- This makes the fake networks visible to nearby devices
- Devices scanning for WiFi will see these networks

## Testing Karma

### Safe Testing Environment
- **Use your own devices** for testing
- **Test in controlled environments** (your home, lab)
- **Never test on public networks** or other people's devices
- **Stop the attack** when testing is complete

### Generating Probe Requests
1. **Forget a known network** on your phone's WiFi settings
2. **Turn WiFi off and on** - your phone will probe for the forgotten network
3. **Walk around with WiFi enabled** - phones constantly probe for known networks
4. **Use IoT devices** that automatically probe for specific networks

### Verifying Karma is Working
Look for these log messages in the terminal:
```
[KARMA] Received probe request from STA XX:XX:XX:XX:XX:XX for SSID 'NetworkName'
Karma cached SSID: NetworkName
Karma rotating to SSID: NetworkName
[KARMA] Evil portal started for SSID: NetworkName
```

### Checking Fake APs
- Use another device to scan for WiFi networks
- You should see the SSIDs that karma is broadcasting
- They should appear as open networks (no password required)

## Advanced Usage

### Manual SSID Management
- Use the GUI option "Start Karma Attack (Custom SSIDs)"
- Enter SSIDs separated by commas: `FreeWiFi,Starbucks,McDonald's`
- This bypasses automatic learning and uses only specified SSIDs

### Monitoring Activity
- Watch the terminal output for real-time activity logs
- Monitor which devices are probing for which networks
- Track which SSIDs are being cached and used

### Integration with Other Attacks
- Karma works well with evil portal attacks
- Can be combined with beacon spam for maximum coverage
- Use alongside deauth attacks to force reconnections

## Troubleshooting

### Karma Not Detecting Probes
- Ensure WiFi is enabled on test devices
- Try moving closer to the ESP32
- Verify devices are actually probing (forget a known network)
- Check that karma is running: `karma start`

### Fake APs Not Appearing
- Check terminal for error messages
- Try restarting karma: `karma stop` then `karma start`
- Verify the ESP32 has sufficient power
- Check for WiFi channel conflicts

### No Devices Connecting
- Ensure evil portal is working correctly
- Check that fake APs are visible in WiFi scans
- Verify SSIDs match what devices are looking for
- Try different SSIDs or let karma learn automatically

## Legal and Ethical Considerations

### Important Legal Notice
- **Only test on your own devices** or with explicit permission
- **Use in controlled environments** only
- **Obtain proper authorization** before testing
- **Stop attacks immediately** when testing is complete

### Ethical Guidelines
- Use karma for educational purposes and authorized security testing
- Never use karma to attack networks you don't own
- Respect privacy and follow applicable laws
- Use responsibly and ethically

## Technical Details

### Supported Hardware
- All ESP32 variants supported by GhostESP
- Requires WiFi capability (not available on ESP32-S2)
- Works with or without SD card (evil portal functionality)

### Performance
- Can cache up to 64 SSIDs simultaneously
- Rotates between SSIDs every 5 seconds
- Broadcasts beacons every 500ms
- Minimal impact on device performance

### Memory Usage
- SSID cache uses minimal RAM
- Evil portal integration adds overhead
- Automatic cleanup when karma stops

---

For more information about GhostESP features, see the [Features](Features.md) and [Commands](Commands.md) guides.
