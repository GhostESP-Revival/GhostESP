# ESP-Hosted Slave OTA Example

| Supported Targets | ESP32-P4 | ESP32-H2 |
| ----------------- | -------- | -------- |

## What This Example Does

This example demonstrates **ESP-Hosted Transport OTA** - updating slave firmware **through the existing ESP-Hosted connection** (SDIO/SPI/UART) without any additional hardware or physical access.

**Key Features:**
- **No extra hardware needed** - uses your existing ESP-Hosted transport
- **Three OTA methods** - choose what works best for your deployment
- **Version checking** - prevents unnecessary updates

## ESP-Hosted Slave Firmware Update Approaches

There are a lot of ways that you can flash the slave firmware.
To limit possibilities, **two main approaches** could be understood.

##### 1. ESP-Hosted Slave OTA (Current Example focus this)
- **Hardware**: None (reuses existing SDIO/SPI/UART serial channel from ESP-Hosted)
- **Use case**: Production updates, remote deployment
- **Access**: Over-the-air OR through Host partition (Uses RPC calls to slave)

##### 2. Direct UART Flashing
- **Hardware**: ESP-Prog or UART connection
- **Use case**: Initial slave FW setup, firmware recovery, development
- **Access**: Requires physical access to device


**This example focuses entirely on Method 1**

---

## Slave OTA Process Flow

```mermaid
---
title: ESP-Hosted Co-processor (Slave) OTA Process
---
sequenceDiagram
    participant Source as Firmware Source
    participant Host as Host MCU
    participant Slave as Slave ESP32

    Note over Host,Slave: Pre-OTA<br/> Version compatibility check (optional)<br/> <Host version != Current Slave version>

    Host->>Host: 1. Read firmware header from FW image
    Host->>Host: 2. Validate FW image<br/>(magic number, header)
    Host->>Host: 3. Compare <FW image version != Current slave version>

    alt Slave needs update
        Host->>Slave: 4. Send OTA_BEGIN command
        Slave->>Slave: Prepare OTA partition

        loop Transfer firmware in chunks
            Source->>Host: 5. Read firmware chunk
            Host->>Slave: 6. Send OTA_WRITE (chunk)
            Slave->>Slave: Write to flash partition
        end

        Host->>Slave: 7. Send OTA_END command
        Slave->>Slave: 8. Verify firmware integrity<br/>(CRC, signature)

        Host->>Host: 9. Check slave FW version<br/>for activate support

        alt Slave FW >= v2.6.0
            Host->>Slave: 10. Send OTA_ACTIVATE command
            Slave->>Slave: Mark new firmware as active
            Note over Slave: ✅ Slave OTA Done
        else Older slave FW
            Note over Host: ✅ Slave OTA Done (activate API not required)
        end

        Note over Host: Restart Host, to avoid sync issues

    else Slave already up-to-date
        Note over Host: ✅ Slave OTA not required<br/>(firmware versions match/compatible)
    end
```

## ESP-Hosted Slave OTA APIs

The ESP-Hosted slave OTA functionality is built around **4 core APIs** that handle the complete OTA process. These APIs are transport-agnostic and can work with any firmware source (LittleFS, Partition, HTTPS, SPIFFS, etc.).

### ESP-Hosted-MCU Slave OTA APIs Used

#### 1. `esp_hosted_slave_ota_begin()`
```c
esp_err_t esp_hosted_slave_ota_begin(void);
```
- **Purpose**: Initializes the OTA process on the slave
- **Arguments**: None
- **Returns**: `ESP_OK` on success, error code on failure
- **What it does**:
  - Prepares slave for firmware reception
  - Allocates OTA buffers
  - Sets up the OTA partition on slave

#### 2. `esp_hosted_slave_ota_write()`
```c
esp_err_t esp_hosted_slave_ota_write(const void *data, size_t size);
```
- **Purpose**: Sends firmware data chunks to the slave
- **Arguments**:
  - `data`: Pointer to firmware data chunk
  - `size`: Size of the data chunk (typically 1400-1500 bytes)
- **Returns**: `ESP_OK` on success, error code on failure
- **What it does**:
  - Transmits firmware data over ESP-Hosted transport (SDIO/SPI/UART)
  - Slave writes data to its OTA partition
  - Can be called multiple times for large firmware

#### 3. `esp_hosted_slave_ota_end()`
```c
esp_err_t esp_hosted_slave_ota_end(void);
```
- **Purpose**: Finalizes the OTA process
- **Arguments**: None
- **Returns**: `ESP_OK` on success, error code on failure
- **What it does**:
  - Validates the complete firmware image on slave
  - Calculates and verifies checksums
  - Marks the new firmware as valid but not active

#### 4. `esp_hosted_slave_ota_activate()`
```c
esp_err_t esp_hosted_slave_ota_activate(void);
```
- **Purpose**: Activates the newly flashed firmware
- **Arguments**: None
- **Returns**: `ESP_OK` on success, error code on failure
- **What it does**:
  - Switches slave's boot partition to the new firmware
  - Triggers slave reboot with new firmware
  - **Note**: After this call, slave will restart with new firmware

### Additional API used

#### 5. `esp_hosted_get_coprocessor_fwversion()`
```c
esp_err_t esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *version);
```
- **Purpose**: Gets the currently running slave firmware version
- **Arguments**:
  - `version`: Pointer to structure to store version information
- **Returns**: `ESP_OK` on success, error code on failure
- **Structure**:
  ```c
  typedef struct {
      uint32_t major1;    // Major version
      uint32_t minor1;    // Minor version
      uint32_t patch1;    // Patch version
  } esp_hosted_coprocessor_fwver_t;
  ```

### How This Example Works

This example demonstrates the **complete OTA workflow**:

1. **Version Check** (optional): Uses `esp_hosted_get_coprocessor_fwversion()` to get current slave version
2. **Begin OTA**: Calls `esp_hosted_slave_ota_begin()` to initialize
3. **Transfer Firmware**:
   - Reads firmware from source (LittleFS file/Partition/HTTPS download)
   - Calls `esp_hosted_slave_ota_write()` repeatedly with chunks
4. **Finalize**: Calls `esp_hosted_slave_ota_end()` to validate firmware
5. **Activate**: Calls `esp_hosted_slave_ota_activate()` to switch to new firmware

### Firmware Source Flexibility

**Important**: These APIs are **not limited to the three methods shown**. You can use:
- **LittleFS** (this example)
- **Dedicated Partition** (this example)
- **HTTPS Download** (this example)
- **SPIFFS filesystem**
- **SD Card files**
- **NVS storage**
- **Custom network protocols**
- **Any other firmware source**

The **key requirement** is that you can read the firmware data and feed it to `esp_hosted_slave_ota_write()` in chunks.

---

## ESP-Hosted Slave OTA Methods

Choose the method that best fits your deployment:

| Method | Remarks  |
|--------|----------|
| Slave OTA using LittleFS  | Local deployment. One of Host partition formatted as LittleFS and pushed the slave fw as file |
| Slave OTA using Host Partition | Local deployment. Slave Fw flashed in one of Host partition directly |
| Slave OTA Using HTTPS | Needs internet access. Can reuse HTTPS server in the deployment |

`Ease of Use`: LittleFS > HTTPS > Partition

Pre-requisites:
1. Please ensure the slave application firmware binary is located in the designated path specified below to prevent 'File not Found' build errors.
2. For HTTPS-based updates, verify that the required certificates are properly installed to avoid connection establishment failures.

---

## Project Structure

```
host_performs_slave_ota/
├── components/
│   ├── ota_partition/           # Slave OTA using Host Partition method
│   │   └── slave_fw_bin/        # Put slave .bin files here
│   ├── ota_littlefs/           # Slave OTA using LittleFS method
│   │   └── slave_fw_bin/        # Put slave .bin files here
│   └── ota_https/              # Slave OTA Using HTTPS method
│       ├── certs/              # SSL certificates
│       └── test_server/        # Local HTTPS server
├── partitions.csv              # Universal partition table
└── main/                       # Main application
```

## Host Partition Table
```
nvs,      data, nvs,      0x9000,   16K,
otadata,  data, ota,      0xd000,   8K,
phy_init, data, phy,      0xf000,   4K,
ota_0,    app,  ota_0,    0x10000,  2M,
ota_1,    app,  ota_1,    0x210000, 2M,
storage,  data, littlefs, 0x410000, 0x1E0000,    # Used by Slave OTA using LittleFS
slave_fw, data, 0x40,     0x5F0000, 0x200000,    # Used by Slave OTA using Host Partition
```

---

## Method 1: Slave OTA using LittleFS

**Best for**: General use, dynamic updates, multiple firmware storage

### How it works:
- Slave firmware stored in LittleFS filesystem
- Build system creates filesystem image with firmware
- OTA mounts filesystem and reads firmware file

### Setup Steps:

1. **Build slave firmware:**
   Please build the slave firmware referring the [slave example](https://components.espressif.com/components/espressif/esp_hosted/examples/slave).

2. **Copy slave firmware:**
   ```bash
   cp slave/build/network_adapter.bin examples/host_performs_slave_ota/components/ota_littlefs/slave_fw_bin/
   ```

3. **Configure (optional - already default):**
   ```bash
   idf.py menuconfig
   # ESP-Hosted Slave OTA Configuration → OTA Method → LittleFS OTA
   ```

4. **Build and flash:**
   ```bash
   idf.py -p <host_serial_port> build flash monitor
   ```

### What happens:
- Build system creates LittleFS image containing slave firmware
- LittleFS image flashed to `storage` partition
- At runtime, OTA mounts filesystem, finds firmware, and compares versions
- Only updates if versions differ

---

## Method 2: Slave OTA using Host Partition

**Best for**: Production, fastest updates, most reliable

### How it works:
- Slave firmware pre-flashed to dedicated partition
- OTA reads directly from partition (no filesystem overhead)
- Version comparison prevents unnecessary updates

### Setup Steps:

1. **Build slave firmware:**
   Please build the slave firmware referring the [slave example](https://components.espressif.com/components/espressif/esp_hosted/examples/slave).

2. **Copy slave firmware:**
   ```bash
   cp slave/build/network_adapter.bin examples/host_performs_slave_ota/components/ota_partition/slave_fw_bin/
   ```

3. **Configure:**
   ```bash
   idf.py menuconfig
   # ESP-Hosted Slave OTA Configuration → OTA Method → Partition OTA
   # (Default) Set partition label to: slave_fw
   ```

4. **Build and flash:**
   ```bash
   idf.py -p <host_serial_port> build flash monitor
   ```

### What happens:
- Build system detects slave firmware and shows notice
- During flash, slave firmware automatically flashed to `slave_fw` partition
- At runtime, OTA reads firmware from partition and compares versions
- Only updates if versions differ

---

## Method 3: Slave OTA Using HTTPS

**Best for**: Remote updates, internet deployment

### How it works:
- Downloads slave firmware from HTTPS server
- Verifies image header while downloading
- Supports both self-signed and CA certificates

### HTTPS Certificate Handling

The Slave OTA Using HTTPS supports two certificate modes:

#### Production Mode
For real servers with CA-signed certificates, disable self signed testing.
```
ESP-Hosted Slave OTA Configuration
└── OTA Method
        └── HTTPS OTA Config
             └── Use self-signed certificate (Testing Only)   ---> ❌ DISABLE
```

**How it works:**
- Uses ESP-IDF's built-in CA certificate bundle (`esp_crt_bundle_attach`)
- Automatically validates certificates from major CAs
- No additional certificate files needed
- Production-ready security

#### Testing Mode (Self-Signed Certificates)
**For local testing with self-signed certificates**

1. **Enable self-signed certificates:**
```
ESP-Hosted Slave OTA Configuration
└── OTA Method
        └── HTTPS OTA Config
             └── Use self-signed certificate (Testing Only)   --->  ENABLE
```

2. **Generate SSL certificates:**
   ```bash
   cd examples/host_performs_slave_ota/components/ota_https/test_server
   ./create_self_signed_certs.sh
   ```

3. **Start HTTPS server:**
   ```bash
   cd examples/host_performs_slave_ota/components/ota_https/test_server
   python3 create_https_server.py
   ```

4. **Copy slave firmware to server:**
   Please build the slave firmware referring the [slave example](https://components.espressif.com/components/espressif/esp_hosted/examples/slave).
   ```bash
   cp slave/build/network_adapter.bin examples/host_performs_slave_ota/components/ota_https/test_server/
   ```

5. **Configure WiFi and URL:**
   ```bash
   idf.py menuconfig
   # ESP-Hosted Slave OTA Configuration → OTA Method → HTTPS OTA
   # Set HTTPS OTA URL to: https://<YOUR_IP>:8443/network_adapter.bin
   # Set WiFi SSID and password
   ```

6. **Build and flash:**
   ```bash
   idf.py -p <host_serial_port> build flash monitor
   ```

### What happens:
- ESP32 connects to WiFi
- Downloads firmware over HTTPS with certificate verification
- Verifies image header during download
- Compares versions and only updates if different

---

## Configuration Reference

### Menuconfig Options:
```
ESP-Hosted Slave OTA Configuration
├── OTA Method
│   ├── HTTPS OTA                                            (default)
│   │   │
│   │   ├── Wi-Fi Config
│   │   │    ├── SSID                                        <--change-->
│   │   │    └── Password                                    <--change-->
│   │   │
│   │   └── HTTPS OTA Config
│   │        ├── HTTPS OTA URL (https://...)                 <--change-->
│   │        └── Use self-signed certificate (Testing Only)  <--ENABLE-to-test-using-self-certs>
│   │             └── Skip certificate Common Name check     <--ENABLE-to-test-using-self-certs>
│   │
│   ├── LittleFS OTA
│   │   └── Delete OTA file from LittleFS once finished     (y)
│   │
│   └── Partition OTA
│       └── Partition Label (for Partition Slave OTA)       (slave_fw)
│
├── Host-Slave version compatibility check                   (y)
│
└── Skip OTA if slave firmware versions match               (y)
```

---

## Expected Output

##### Successful OTA:
```
I (1234) host_performs_slave_ota: ESP-Hosted initialized successfully
I (1235) host_performs_slave_ota: Using XXXX OTA method
I (1240) ota_XXXX: Current slave firmware version: 2.5.12
I (1242) ota_XXXX: New slave firmware version: 2.6.0
I (1245) ota_XXXX: Version differs - proceeding with OTA
I (5678) ota_XXXX: XXXX OTA completed successfully
I (5680) host_performs_slave_ota: OTA completed successfully
```

##### Same Version (Skip OTA):
```
I (1240) ota_XXXX: Current slave firmware version: 2.6.0
I (1242) ota_XXXX: New slave firmware version: 2.6.0
W (1245) ota_XXXX: Versions match. Skipping OTA.
I (1246) host_performs_slave_ota: OTA not required
```

---

## Troubleshooting

### Build Issues:
- **"No .bin files found"**: Copy slave firmware to appropriate `slave_fw_bin/` directory
- **"Partition not found"**: Check `partitions.csv` includes required partitions

### Runtime Issues:
- **"Failed to initialize ESP-Hosted"**: Check hardware connections
- **"OTA failed"**: Check version format mismatch, ensure slave firmware uses semantic versioning
- **WiFi issues (HTTPS)**: Verify network credentials and connectivity
- **SSL errors (HTTPS)**: Ensure certificate CN matches server IP address

### Version Issues:
- Slave firmware now uses semantic versioning (e.g., `2.5.12`) instead of git hashes
- Host compares versions and skips OTA if versions match
- Check slave firmware version with `esp_hosted_get_coprocessor_fwversion()`

## Limitation
- Older slave firmwares (< `2.15.12`) exposed git commit as `PROJECT_VER`, instead of `X.Y.Z` Hosted slave firmware version.
  So, Comparison of `Slave firmware image version` Vs `Slave current FW version` will always fail. So OTA will be triggered.

---

## Direct UART Flashing

> [!NOTE]
>
>  This section is for reference only. The main focus of this example is ESP-Hosted Transport OTA above.

### Slave Flashing Using ESP-Prog

> [!NOTE]
> ESP-Prog is only required if you want to flash firmware to the
> slave using the standard ESP Tools.

In following section, ESP32-P4-Function EV Board is considered as example, to showcase, how ESP-Prog is to be connected. Any ESP can be programmed with ESP-Prog, including Host as well.

The image below shows the board with an ESP-Prog connected to the header to communicate with the on-board ESP32-C6..

<img src="../../docs/images/esp32-p4-function-ev-board-esp-prog.jpg" alt="ESP32-P4-Function-EV-Board with ESP-Prog Connected to ESP32-C6" width="800" />

*ESP32-P4-Function-EV-Board with ESP-Prog Connected to ESP32-C6*

If you need to update the ESP-Hosted slave firmware on the on-board ESP32-C6 module using ESP-Prog, follow these steps:

1. Check out the ESP-Hosted slave example project:

```
idf.py create-project-from-example "espressif/esp_hosted:slave"
```

2. Set the target and start `Menuconfig`:

```sh
idf.py set-target esp32c6
idf.py menuconfig
```

3. Navigate and ensure SDIO is enabled. By default it should already be enabled.
   ```
   Example Configuration
   └── Bus Config in between Host and Co-processor
       └── Transport layer
           └── Select "SDIO"
   ```

4. Build the firmware:

```sh
idf.py build
```

5. Connect the Program Header on the ESP-Prog to the `PROG_C6` header
   on the board. The connections are as follows:

| ESP-Prog | PROG_C6 | Notes          |
| ---      | ---     | ---            |
| ESP\_EN  | EN      |                |
| ESP\_TXD | TXD     |                |
| ESP\_RXD | RXD     |                |
| VDD      | -       | Do not connect |
| GND      | GND     |                |
| ESP\_IO0 | IO0     |                |


6. Flashing the firmware

The on-board ESP32-P4 controls the reset signal for the ESP32-C6. To
prevent the P4 interfering with the C6 while flashing (by asserting
the C6 Reset signal during the firmware download), set the P4 into
Bootloader mode before flashing the firmware to the C6:

###### Manual Way
    1. hold down the `BOOT` button on the board
    2. press and release the `RST` button on the board
    3. release the `BOOT` button

###### Script Way

```sh
esptool.py -p <host_serial_port> --before default_reset --after no_reset run
```

You can now flash the firmware to the C6 (and monitor the console
output):

```sh
idf.py -p <Serial Port> flash monitor
```

### Flashing Slave FW From Host MCU Using direct UART
You can connect above ESP32-C6 (slave) GPIOs directly on ESP32-P4 (Host) GPIOs and use [ESP-Serial-Flasher](https://github.com/espressif/esp-serial-flasher) to flash the Slave firmware from Host.

**Hardware Required**: Host MCU UART connection (dedicated UART needed, cannot use ESP-Hosted bus)

**Steps**:

1. Connect host UART to slave flashing pins
2. Put host in bootloader mode (same as explained above)
3. Use esp-serial-flasher library to flash over UART
