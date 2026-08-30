| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 |
| ----------------------- | ----- |

ESP-IDF ESP-Hosted Hosted HCI Host
==================================

This is a Bluetooth Host using ESP-Hosted as HCI IO to the BT Controller.

## Example Layout

This example is modified based on [bt_discovery](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/bt_discovery), and all modifications are listed below:

- Removed all dependencies on controller from `main.c`.

```
#include "esp_bt.h"

...

ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
    ESP_LOGE(GAP_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
    return;
}

if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
    ESP_LOGE(GAP_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
    return;
}
```

- Add support for ESP-Hosted HCI interface: `esp_hosted_bt.h`.

- Open HCI interface in `main.c`.

```
#include "esp_hosted_bt.h"

...

/* initialize TRANSPORT first */
hosted_hci_bluedroid_open();

/* get HCI driver operations */
esp_bluedroid_hci_driver_operations_t operations = {
    .send = hosted_hci_bluedroid_uart_send,
    .check_send_available = hosted_hci_bluedroid_check_send_available,
    .register_host_callback = hosted_hci_bluedroid_register_host_callback,
};
esp_bluedroid_attach_hci_driver(&operations);
```

## How to use example

### Hardware Required

This example runs on the ESP32-P4 Dev Board connected to a ESP32 via the GPIO header, using SPI FD (full duplex) as Hosted HCI transport. The following GPIO settings were used:

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

For standard HCI, configure the co-processor Bluetooth Controller to use UART as the HCI transport, then select appropriate GPIOs on the ESP32-P4 to configure as a UART. In this mode, ESP-Hosted is not involved in transporting the HCI data.

See the ESP-IDF [UART HCI Host example](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/bluedroid_host_only/bluedroid_host_only_uart) on how to set-up UART for the Bluetooth Host.

### Configure the project

First, set the host target to ESP32-P4:

```
idf.py set-target esp32p4
```

For the ESP32 co-processor, run `idf.py menuconfig` and configure `Example Configuration` for SPI Full-duplex with the correct SPI mode and GPIOs.

For the ESP32-P4 host, run `idf.py menuconfig` and under `Component config ---> ESP-Hosted config`:

* set the transport to be `SPI Full-duplex` with the correct SPI modem GPIOs (see above table) and SPI Clock frequency (10 MHz max).
* set the Slave chipset used as `ESP32`.
* set `Bluetooth Support ---> Enable Hosted Bluedroid Bluetooth support` to enable Bluedroid support. Leave the HCI type as `VHCI`.

### Build and Flash

After setting the host target and configuring the project, build and flash the co-processor and host projects, then run monitor tool to view serial output on both the ESP32 and ESP32-P4:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)
