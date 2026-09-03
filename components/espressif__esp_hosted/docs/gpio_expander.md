# GPIO Expander

The ESP-Hosted solution provides a feature to control the GPIOs of the slave (co-processor) from the host MCU. This acts as a virtual GPIO expander over the existing transport link (SPI, SDIO, or UART), saving hardware pins and complexity on the host board.

## Use Cases

*   Driving LEDs or status indicators on the slave board.
*   Reading button presses or sensor states connected to the slave.
*   Resetting or enabling/disabling peripherals connected to the slave.
*   Any application where the host needs simple digital I/O control over the slave.

## API

The GPIO control functionality is exposed through the `esp_hosted_cp_gpio.h` header file. The API is designed to be similar to the standard ESP-IDF GPIO driver.

Key functions include:
*   `esp_hosted_cp_gpio_config()`: Configure a GPIO's mode (input/output), pull-up/pull-down, and interrupt type.
*   `esp_hosted_cp_gpio_set_level()`: Set the logic level of an output GPIO.
*   `esp_hosted_cp_gpio_get_level()`: Read the logic level of an input GPIO.
*   `esp_hosted_cp_gpio_set_direction()`: Change the direction (input/output) of a GPIO.
*   ... and more.

## Enabling the Feature

To use this feature, it must be enabled on both the host and the slave firmware.

### Host Configuration

In the host's `menuconfig`, enable the following option:
```
Component config --->
    ESP-Hosted --->
        [*] Enable GPIO Expander feature on host
```
This corresponds to the `CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER=y` option.

### Slave Configuration

In the slave's `menuconfig`, enable the following option:
```
(Top) → Example Configuration
        [*] Enable GPIO Expander support (host can control slave GPIOs)
```
This corresponds to the `CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER=y` option.

## Safety Guard

The slave firmware includes a safety mechanism to prevent the host from accidentally or maliciously interfering with GPIOs that are critical for the ESP-Hosted transport link itself. Any RPC request from the host to control a pin used by the active SPI, SDIO, or UART interface will be rejected by the slave.

## Example

An example demonstrating the usage of this feature can be found in the `examples/host_gpio_expander` directory.
