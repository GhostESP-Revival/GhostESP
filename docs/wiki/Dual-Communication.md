# GhostLink (Dual Communication)

Connect two ESP32 devices with wires to control one from the other. Since ESP32s only have one radio, this allows you to have full control and functionality remotely while one still hosts the AP continously for control.

## How It Works

- Wire two ESP32s together (UART connection)
- They auto-discover and connect on boot
- Send any GhostESP command to the other device
- No manual connection needed in most cases

## Wiring

Connect the devices with 3 wires:

- **TX of Device A** → **RX of Device B** (GPIO 6 → GPIO 7 by default, 17 → 16 on base ESP32 models)
- **RX of Device A** → **TX of Device B** (GPIO 7 → GPIO 6 by default, 16 → 17 on base ESP32 models)  
- **GND** → **GND**

Both devices need Power.

## Basic Usage

Once wired and powered on, devices automatically find each other. Use these commands:

- `commstatus` - Check if connected
- `commsend <command>` - Send any command to the other device
- `commdisconnect` - Disconnect if needed

### Examples

```bash
# Check connection
commstatus

# Send commands to the other device
commsend scanap
commsend attack -d
commsend beaconspam -r
commsend capture -probe
```

## Web Interface

Use the **GhostLink** tab for:
- Connection status with visual indicator
- Quick command buttons organized by category
- Terminal to send custom commands
- Pin configuration if you need different GPIO pins

## Changing Pins

Default pins are TX: GPIO 6, RX: GPIO 7 on base ESP32 models they are 17 and 16. To change them:

```bash
commsetpins NUM NUM
```

Pin changes are saved and you can't change them while connected.

## Manual Connection (if needed)

If auto-connection fails, you can manually connect:

```bash
commdiscovery          # Check discovery status
commconnect ESP_A1B2C3 # Connect to specific device
```

Device names are auto-generated like `ESP_A1B2C3` based on MAC address.

## Troubleshooting

**Not connecting?**
- Check wiring (TX→RX, RX→TX, GND connected)
- Make sure both devices are powered, and reboot them simultaneously
- Wait 30 seconds for discovery
- Run `commstatus` to check

**Commands not working?**
- Verify connection with `commstatus`
- Use exact GhostESP command syntax

## Use Cases

- **Hidden device control**: Hide 2 ESP32s and control them remotely with full functionality through the WebUI!
- **Coordinated attacks**: Both devices attack different targets simultaneously

## Technical Details

- **Protocol**: Custom UART (initialized at 115,200 baud) 
- **Auto-discovery**: Every 3 seconds
- **Master/slave**: Device with "larger" name becomes master but you can send and receive commands from both devices
- **Checksum**: CRC-8 is preferred and negotiated between peers with a legacy XOR fallback
- **Auto-reconnect / ping**: The manager sends periodic pings and expects pongs; on link timeout the connection is considered lost and discovery restarts automatically
- **Physical access required**: Needs wired connections

## Notes for developers

- `MAX_CMD_LEN` is 32; packet payload size is constrained by `COMM_PACKET_SIZE` (64 bytes). Responses include an optional `[seq|flags]` header to help assemble lines reliably on the receiving side. 