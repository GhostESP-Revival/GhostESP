---
title: "SD Card"
description: "Using SD card storage with GhostESP for file management, captures, and configuration."
weight: 40
---

GhostESP uses SD cards to store captures, logs, and files. Learn how to manage files and configure pins.

## Storage Structure

When initialized, GhostESP creates the following directory structure:

```
/mnt/ghostesp/
├── debug/          # Debug logs
├── pcaps/          # Packet captures
├── scans/          # Scan results
├── gps/            # GPS/wardriving logs
├── games/          # Game saves
├── evil_portal/
│   └── portals/    # Custom portal HTML files
├── infrared/
│   ├── remotes/    # Learned IR signals
│   └── universals/ # Universal IR databases
└── nfc/            # NFC card data (if enabled)
```

## Web Serial Interface

For the easiest file management experience, use the **Web Serial Interface** at:

**[ghostesp.net/serial](https://ghostesp.net/serial)**

This browser-based tool connects directly to your GhostESP via USB and provides:

- **File Browser** — Browse, upload, and download files using the SD CLI
- **Serial Console** — Full terminal access to all CLI commands
- **Screen Mirror** — View your device's display in real-time

> **Note**: Requires a Chromium-based browser (Chrome, Edge, Brave) with Web Serial API support.

## WebUI File Manager

Access the SD card through the WebUI's **SD Card** tab:

1. Connect to **GhostNet** and open `192.168.4.1` or `ghostesp.local`
2. Navigate to the **SD Card** tab
3. Browse the `/mnt/ghostesp/` directory structure

### Capabilities

- **Navigate** — Click folders to browse
- **Download** — Download PCAP captures, logs, and other files
- **Upload** — Add custom portal HTML files or IR remotes
- **Delete** — Remove files to free space

> **Tip**: For large file transfers (>1MB), use a USB card reader for faster speeds.

## CLI Commands

The `sd` command allows scripting and direct file access.

### Status

```
sd status
```

Output format:
```
SD:STATUS:mounted=true
SD:STATUS:type=physical
SD:STATUS:name=SD32G
SD:STATUS:capacity_mb=30436
SD:STATUS:used_pct=12
SD:STATUS:free_mb=26784
```

### List Files

```
sd list [path]
```

Lists files and directories with indices for quick reference:

```
SD:LIST:/mnt/ghostesp
SD:DIR:[0] pcaps
SD:DIR:[1] scans
SD:FILE:[2] config.txt 1024
SD:FILE:[3] log.csv 8192
SD:OK:listed 4 entries
```

Use indices in subsequent commands:

```
sd info 2         # Get info for config.txt
sd cat 3 1000     # Read first 1000 bytes of log.csv
```

### File Info

```
sd info <index|path>
```

Shows file or directory details:

```
SD:INFO:path=/mnt/ghostesp/pcaps/capture.pcap
SD:INFO:type=file
SD:INFO:size=524288
SD:OK
```

### Read File

```
sd read <index|path> [offset] [length]
```

Reads file contents with optional offset and length for chunked downloads. No size limit:

```
SD:READ:BEGIN:/mnt/ghostesp/capture.pcap
SD:READ:SIZE:1048576
SD:READ:OFFSET:0
SD:READ:LENGTH:65536
... binary file contents ...
SD:READ:END:bytes=65536
SD:OK
```

Example chunked download:
```
sd read myfile.bin 0 65536       # First 64KB
sd read myfile.bin 65536 65536   # Next 64KB
sd read myfile.bin 131072 65536  # Next 64KB...
```

### File Size

```
sd size <index|path>
```

Quick file size check (useful before downloads):

```
SD:SIZE:1048576
SD:OK
```

### Write File

```
sd write <path> <base64data>
```

Creates or overwrites a file with base64-decoded data:

```
sd write myfile.txt SGVsbG8gV29ybGQh
SD:WRITE:bytes=12
SD:OK:created:/mnt/ghostesp/myfile.txt
```

### Append to File

```
sd append <path> <base64data>
```

Appends base64-decoded data to an existing file:

```
sd append myfile.txt IG1vcmUgZGF0YQ==
SD:APPEND:bytes=10
SD:OK:appended:/mnt/ghostesp/myfile.txt
```

> **Note:** For large file uploads via WebUI, use the HTTP API which handles binary data directly. The CLI uses base64 encoding due to serial protocol limitations.

### Create Directory

```
sd mkdir <path>
```

Creates a new directory:

```
sd mkdir mydata
SD:OK:created:/mnt/ghostesp/mydata
```

### Delete

```
sd rm <index|path>
```

Removes a file or empty directory:

```
sd rm 2
SD:OK:removed:/mnt/ghostesp/config.txt
```

### Tree View

```
sd tree [path] [depth]
```

Recursive directory listing (default depth: 2, max: 10):

```
SD:TREE:/mnt/ghostesp
[D] pcaps/
  [F] capture_001.pcap (524288)
  [F] capture_002.pcap (131072)
[D] scans/
[D] infrared/
  [D] remotes/
SD:OK:tree 5 items
```

## Pin Configuration

SD card pins can be reconfigured at runtime without recompiling.

### SPI Mode

Most boards use SPI mode. Configure pins with:

```
sd_pins_spi <CS> <CLK> <MISO> <MOSI>
```

Example:
```
sd_pins_spi 5 18 19 23
sd_save_config
```

### SDMMC Mode

Some boards support faster SDMMC (1-bit or 4-bit):

```
sd_pins_mmc <CLK> <CMD> <D0> <D1> <D2> <D3>
```

Example (4-bit):
```
sd_pins_mmc 39 38 40 41 42 2
sd_save_config
```

### View Current Config

```
sd_config
```

Shows current pin assignments for both modes.

### Save Configuration

```
sd_save_config
```

Persists pin settings to NVS. Changes apply on next boot.

## Virtual Storage

Devices without SD slots (like S3TWatch) use internal flash as virtual storage (limited to ~4MB).

Check storage type:
```
sd status
```

Output:
```
SD:STATUS:mounted=true
SD:STATUS:type=virtual
```

## Machine-Parsable Output

All `sd` command responses use a consistent format for scripting:

| Prefix | Meaning |
|--------|---------|
| `SD:OK:...` | Success with optional message |
| `SD:ERR:...` | Error with reason |
| `SD:STATUS:key=value` | Status key-value pair |
| `SD:LIST:path` | Directory listing header |
| `SD:DIR:[n] name` | Directory entry with index |
| `SD:FILE:[n] name size` | File entry with index and size |
| `SD:INFO:key=value` | File info key-value pair |
| `SD:CAT:BEGIN:path` | File content start marker |
| `SD:CAT:END:bytes=n` | File content end marker |
| `SD:TREE:path` | Tree listing header |
| `SD:EMPTY` | Empty directory |

## Troubleshooting

### SD Card Not Detected

- Verify the card is formatted as FAT32
- Check pin configuration matches your hardware
- Ensure the card is fully inserted
- Try a different SD card

### Mount Failures

```
sd status
SD:STATUS:mounted=false
```

- Run `sd_config` to verify pin settings
- Some boards require specific pin configurations at compile time

### Slow Transfers

- Use a Class 10 or higher SD card
- For large files, use a USB card reader instead of WebUI
- Reduce concurrent operations

## Next Steps

- [WebUI Guide]({{< relref "webui-guide.md" >}}) — File manager and settings
- [Command Line Reference]({{< relref "command-line-reference.md" >}}) — Full CLI documentation
- [Infrared]({{< relref "../infrared/_index.md" >}}) — Store IR remotes on SD card
