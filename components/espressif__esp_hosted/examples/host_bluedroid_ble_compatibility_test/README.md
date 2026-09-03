| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 | ESP32-C Series | ESP32-S Series |
| ----------------------- | ----- | -------------- | -------------- |

# ESP-IDF BLE Compatibility Test Example

This example is to test the Bluetooth compatibility and mobile phones.

## How to Use Example

This example has been modified to work with ESP-Hosted. The original ESP-IDF example is at [ https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/ble/ble_compatibility_test ].

This example is able to run on the ESP32-P4 Dev Board, acting as the BT Host, connected to a ESP32 co-processor via the GPIO header, using SPI FD (full duplex) as Hosted HCI transport. The ESP32-P4 acts as the BT Controller. The following GPIO settings were used:

| SPI Function | ESP32 GPIO | ESP32-P4 GPIO |
| :---         |       ---: |          ---: |
| MOSI         |         13 |             4 |
| MISO         |         12 |             5 |
| CLK          |         14 |            26 |
| CS           |         15 |             6 |
| Handshake    |         26 |            20 |
| Data Ready   |          4 |            36 |
| Reset        |         -1 |             2 |

> [!NOTE]
> SPI Mode 2 was used on both the ESP32-P4 and ESP32.

Users are free to choose which supported ESP-Hosted transport to use. See the [main ESP-Hosted README](https://github.com/espressif/esp-hosted-mcu/blob/main/README.md#6-decide-the-communication-bus-in-between-host-and-slave) for a list of supported transports.

Before project configuration and build, be sure to set the correct chip target using:

```bash
idf.py set-target <chip_name>
```

### Test Scenario

* ESP32-P4-Function-EV-Board connected to a ESP32 via the GPIO header
* [Test case](ble_compatibility_test_case.md)
* Test APK: LightBlue V2.0.5

### Configure the project

On the ESP32-P4 Dev Board, run `idf.py menuconfig`.

* Check and enable Classic Bluetooth and Classic BT HID Device under `Component config --> Bluetooth --> Bluedroid Options`
* Ensure that `Component config --> Bluetooth --> Controller` is `Disabled`.
* Under `Component config --> ESP-Hosted config`:
  * Configure ESP-Hosted to use `SPI Full-duplex` as the transport
  * Set the Slave chipset used as `ESP32`
  * Check and enable `Bluetooth Support`
  * Configure the GPIOs used for SPI FD on both the ESP32-P4 and ESP32, following the table above

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://idf.espressif.com/) for full steps to configure and use ESP-IDF to build projects.
