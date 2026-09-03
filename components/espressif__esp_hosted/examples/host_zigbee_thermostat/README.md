| Supported Hosts | ESP32-P4 |
| --------------- | -------- |

| Supported Targets | Co-processors with Zigbee RCP Support |
|-------------------|---------------------------------------|

# Thermostat Example

This example demonstrates how to configure a Home Automation thermostat on a Zigbee Coordinator. Is has been modified from the [ESP Zigbee SDK Thermostat example](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/home_automation_devices/thermostat).

## Hardware Required

* An ESP32-P4 connected to a coprocessor with 802.15.4 support (e.g., ESP32-H2 or ESP32-C6) running this example. The ESP-Hosted Transport **and** UART should be connected between the two SOCs. See the [OpenThread and Zigbee Support](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/openthread_zigbee.md) documentation for more information.
* A second board running as a Zigbee end-device (see the [ESP Zigbee SDK temperature\_sensor](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/home_automation_devices/temperature_sensor) example)

## Configure the project

Before project configuration and build, make sure to set the correct chip target using `idf.py set-target TARGET` command.

## Erase the NVRAM

Before flash it to the board, it is recommended to erase NVRAM if user doesn't want to keep the previous examples or other projects stored info using `idf.py -p PORT erase-flash`

## Build and Flash

Build the project, flash it to the board, and start the monitor tool to view the serial output by running `idf.py -p PORT flash monitor`.

(To exit the serial monitor, type ``Ctrl-]``.)

## Application Functions

- When the program starts, the board, acting as a Zigbee Coordinator with the `Home Automation Thermostat` function, will form an open network within 180 seconds.

```text
I (419) main_task: Calling app_main()
[…]
I (1611) ESP_RADIO_SPINEL: spinel UART interface initialization completed
I (1611) ESP_RADIO_SPINEL: Spinel UART interface has been successfully enabled
I (1611) ESP_ZIGBEE_RADIO_SPINEL_UART: Spinel UART interface enable successfully
I(1621) OPENTHREAD:[I] P-SpinelDrive-: co-processor reset: RESET_POWER_ON
E(1631) OPENTHREAD:[C] P-SpinelDrive-: Software reset co-processor successfully
I (1691) THERMOSTAT: Initialize Zigbee stack
E (1691) gpio: gpio_install_isr_service(537): GPIO isr service already installed
I (1691) THERMOSTAT: Deferred driver initialization successful
I (1691) THERMOSTAT: Device started up in non factory-reset mode
I (1701) THERMOSTAT: Device reboot
I (2831) THERMOSTAT: Network(0x03e2) is open for 180 seconds
```

- If a Zigbee device with the `Home Automation Temperature Sensor` function joins the network, the board read its manufacturer code and model then add it to the binding table.

```text
I (11889) THERMOSTAT: Attempt to find HA temperature sensor device on address(0xd9bc)
I (12279) THERMOSTAT: Attempt to read manuf_code and model_id from device (0xd9bc, 0x0a)
I (12369) THERMOSTAT: ZCL Read Attribute Response message for endpoint(1) cluster(0x0000) client with status(0x00)
I (12369) THERMOSTAT: Model identifier: esp32h2
I (12369) THERMOSTAT: Manufacturer name: ESPRESSIF
I (12379) THERMOSTAT: Attempt to bind temperature sensor device (0xd9bc, 0x0a) to local
I (12389) THERMOSTAT: Attempt to subscribe temperature sensor device (0xd9bc, 0x0a) from local
I (12429) THERMOSTAT: Bound HA temperature sensor device (0x0000, 0x0a) to local successfully
```

- By clicking the `BOOT` button on this board, it will read the temperature value, temperature measurement range, and temperature tolerance from the remote board. Additionally, it will configure the remote temperature sensor to report the measured temperature value every 10 seconds or whenever there is a 2-degree change.

```text
I (25321) THERMOSTAT: Zigbee APP Signal: ZDO Device Update(type: 0x07)
I (25351) THERMOSTAT: New device commissioned or rejoined (short: 0x96cf)
I (25351) THERMOSTAT: Attempt to find HA temperature sensor device on address(0x96cf)
I (25411) THERMOSTAT: Attempt to read manuf_code and model_id from device (0x96cf, 0x0a)
I (25481) THERMOSTAT: ZCL Read Attribute Response message for endpoint(1) cluster(0x0000) client with status(0x00)
I (25481) THERMOSTAT: Model identifier: esp32h2
I (25491) THERMOSTAT: Manufacturer name: ESPRESSIF
I (25491) THERMOSTAT: Attempt to bind temperature sensor device (0x96cf, 0x0a) to local
I (25501) THERMOSTAT: Attempt to subscribe temperature sensor device (0x96cf, 0x0a) from local
I (25511) THERMOSTAT: Bound HA temperature sensor device (0x0000, 0x0a) to local successfully
I (25521) THERMOSTAT: Attempt to configure reporting for HA temperature sensor
I (25591) THERMOSTAT: Subscribed HA temperature sensor device (0x96cf, 0x0a) from local successfully
I (25641) THERMOSTAT: ZCL Report Config Response message for endpoint(1) cluster(0x0402) client with status(0x00)
I (25671) THERMOSTAT: ZCL Report Attribute message for endpoint(1) cluster(0x0402) client with status(0x00)
I (25671) THERMOSTAT: Temperature sensor measured value: -327.68 degrees Celsius
I (26741) THERMOSTAT: ZCL Report Attribute message for endpoint(1) cluster(0x0402) client with status(0x00)
I (26741) THERMOSTAT: Temperature sensor measured value: 29.60 degrees Celsius
I (36841) THERMOSTAT: ZCL Report Attribute message for endpoint(1) cluster(0x0402) client with status(0x00)
I (36841) THERMOSTAT: Temperature sensor measured value: 29.60 degrees Celsius
I (46911) THERMOSTAT: ZCL Report Attribute message for endpoint(1) cluster(0x0402) client with status(0x00)
I (46911) THERMOSTAT: Temperature sensor measured value: 29.60 degrees Celsius
I (56981) THERMOSTAT: ZCL Report Attribute message for endpoint(1) cluster(0x0402) client with status(0x00)
I (56981) THERMOSTAT: Temperature sensor measured value: 29.60 degrees Celsius
[…]
W (206631) THERMOSTAT: Read Temperature Measurement attribute ID: 0x0003 with status: 0x86
I (206631) THERMOSTAT: Max measured value: 80.00 degrees Celsius
I (206641) THERMOSTAT: Min measured value: -10.00 degrees Celsius
I (206641) THERMOSTAT: Measured value: 29.60 degrees Celsius
[…]
```
