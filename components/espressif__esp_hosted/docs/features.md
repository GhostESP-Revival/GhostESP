# Features Implemented in ESP-Hosted-MCU

This page documents the features implemented by ESP-Hosted-MCU.

## Wi-Fi

1. **Station** (scan, connect to an AP)
2. **SoftAP** (configure, start, stop)
3. **SoftAP+Station**
4. (Optional) [**Network Split**](feature_network_split.md) for splitting network traffic support between the co-processor and the host
5. (Optional) **iTWT** (individual Target Wake Time) Wi-Fi 6 (802.11ax) feature that lets a station negotiate its own wake/sleep schedule with an iTWT supporting AP to reduce power consumption
   - only works with co-processors that support iTWT like the ESP32-C6 and ESP32-C5
6. (Optional) **Wi-Fi Enterprise** security mode for enhanced security in business environments
7. (Optional) **Wi-Fi Easy Connect (DPP)** to securely onboard devices by scanning a QR code (displayed by the host) without entering a Wi-Fi password
   - requires a smartphone that supports Wi-Fi Easy Connect to scan the QR code and start the onboarding operation
   - APIs added:
	 - `esp_supp_dpp_init`
	 - `esp_supp_dpp_deinit`
	 - `esp_supp_dpp_bootstrap_gen`
	 - `esp_supp_dpp_start_listen`
	 - `esp_supp_dpp_stop_listen`

## Bluetooth

1. **ESP-Hosted / UART HCI** support for ESP-Nimble and ESP-Bluedroid. See [Bluetooth Design](bluetooth_design.md) for more information

## OpenThread

1. **ESP-Hosted / Dedicated UART** support for ESP OpenThread / Zigbee. See [OpenThread / Zigbee Support](openthread_zigbee.md) for more information

## Miscellaneous

1. (Optional) [**Host Power Save**](feature_host_power_save.md) to allow Host to go to sleep and be waken by co-processor
2. (Optional) [**GPIO Expander**](gpio_expander.md) to allow the host to control the GPIOs of the slave.
