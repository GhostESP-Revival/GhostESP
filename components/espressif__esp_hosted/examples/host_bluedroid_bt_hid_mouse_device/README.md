| Supported Hosts | ESP32 | ESP32-P Series | ESP32-H Series | ESP32-C Series | ESP32-S Series | Any other MCU hosts |
| --------------- | ----- | -------------- | -------------- | -------------- | -------------- | ------------------- |

| Supported Co-Processors | ESP32 |
| ----------------------- | ----- |

# Bluetooth HID Device example

This example aims to show how to implement a Bluetooth HID device using the APIs provided by Classic Bluetooth HID profile.

This example simulates a Bluetooth HID mouse device that periodically sends report to remote Bluetooth HID host after connection. The report indicates a horizontally moving pointer and can be observed on the display on the HID host side. If you want to build an HID device, this can be your first example to look at.

## How to use example

This example has been modified to work with ESP-Hosted. The original ESP-IDF example is at [ https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/bt_hid_mouse_device/ ].

### Hardware Required

* This example is able to run on the ESP32-P4 Dev Board, acting as the BT Host, connected to a ESP32 co-processor via the GPIO header, using SPI FD (full duplex) as Hosted HCI transport. The ESP32 acts as the BT Controller. The following GPIO settings were used:

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

* This example is supposed to connect to a Classic Bluetooth HID Host device, e.g. laptop or tablet.

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

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

The following log will be shown on the IDF monitor console:

```
I (499) main_task: Calling app_main()
I (509) transport: Attempt connection with slave: retry[0]
I (509) transport: Reset slave using GPIO[2]
I (509) os_wrapper_esp: GPIO [2] configured
I (509) gpio: GPIO[2]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (1699) transport: Received INIT event from ESP32 peripheral
I (1699) transport: EVENT: 12
I (1699) transport: EVENT: 11
I (1699) transport: capabilities: 0xf8
I (1709) transport: Features supported are:
I (1709) transport:        - HCI over SPI
I (1709) transport:        - BT/BLE dual mode
I (1719) transport: EVENT: 13
I (1719) transport: ESP board type is : 0

I (1719) transport: Base transport is set-up

I (1729) transport: Slave chip Id[12]
I (1729) vhci_drv: Host BT Support: Enabled
I (1729) vhci_drv:      BT Transport Type: VHCI
I (1739) spi: Received INIT event
I (2799) app_main: setting device name
I (2799) esp_bt_gap_cb: event: 10
I (2799) app_main: setting cod major, peripheral
I (4799) app_main: register hid device callback
I (4799) app_main: starting hid device
I (4799) esp_bt_hidd_cb: setting hid parameters
I (4799) esp_bt_gap_cb: event: 10
I (4799) esp_bt_hidd_cb: setting hid parameters success!
I (4799) esp_bt_hidd_cb: setting to connectable, discoverable
I (4809) app_main: Own address:[10:97:bd:d5:8a:62]
I (4809) app_main: exiting
```

The messages show the successful initialization of Bluetooth stack and HID application. ESP32-P4 will become discoverable with the Bluetooth device name as "HID Mouse Example", by nearby Bluetooth HID Host device.

Connect to ESP32-P4 on the HID Host side, then finish bonding. After that the HID connection will be established. IDF monitor console will continue to print messages like:

```
W (21229) BT_HCI: hcif conn complete: hdl 0x80, st 0x0
I (21229) esp_bt_gap_cb: event: 16
I (21859) esp_bt_gap_cb: authentication success: XXXXXXXX
I (21859) esp_bt_gap_cb: 64 49 7d d0 fd 99
I (21889) esp_bt_gap_cb: event: 21
W (22299) BT_HIDD: hidd_l2cif_config_cfm: config failed, retry
W (22319) BT_APPL: new conn_srvc id:20, app_id:1
I (22319) esp_bt_hidd_cb: connected to 64:49:7d:d0:fd:99
I (22319) mouse_move_task: starting
I (22319) esp_bt_hidd_cb: making self non-discoverable and non-connectable.
W (22329) BT_HCI: hci cmd send: sniff: hdl 0x80, intv(10 18)
I (22339) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
W (22349) BT_HCI: hcif mode change: hdl 0x80, mode 2, intv 18, status 0x0
I (22349) esp_bt_gap_cb: ESP_BT_GAP_MODE_CHG_EVT mode:2
I (22369) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
I (22419) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
I (22469) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
I (22519) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
I (22569) esp_bt_hidd_cb: ESP_HIDD_SEND_REPORT_EVT id:0x00, type:1
```

ESP32-P4 will generate and send HID mouse reports periodically. On the screen of HID Host, the cursor will move horizontally from left to right and then right to left, and so on so forth.

## Example Breakdown

### Initial settings for Bluetooth HID device profile

Bluetooth HID device requires the specific major and minor device type in the Class of Device (CoD), the following lines of source code performs the configuration of CoD:

```
void app_main(void) {
    ...
    ESP_LOGI(TAG, "setting cod major, peripheral");
    esp_bt_cod_t cod;
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    ...
}
```

Bluetooth HID device profile requires the information of service name, provide, device subclass, report descriptor for SDP server, as well as L2CAP QoS configurations from the application. Following lines in function `app_main` initialize these information fields:

```
void app_main(void) {
    ...
    // Initialize HID SDP information and L2CAP parameters.
    // to be used in the call of `esp_bt_hid_device_register_app` after profile initialization finishes
    do {
        s_local_param.app_param.name = "Mouse";
        s_local_param.app_param.description = "Mouse Example";
        s_local_param.app_param.provider = "ESP32";
        s_local_param.app_param.subclass = ESP_HID_CLASS_MIC;
        s_local_param.app_param.desc_list = hid_mouse_descriptor;
        s_local_param.app_param.desc_list_len = hid_mouse_descriptor_len;

        memset(&s_local_param.both_qos, 0, sizeof(esp_hidd_qos_param_t)); // don't set the qos parameters
    } while (0);

    // Report Protocol Mode is the default mode, according to Bluetooth HID specification
    s_local_param.protocol_mode = ESP_HIDD_REPORT_MODE;

    ESP_LOGI(TAG, "register hid device callback");
    esp_bt_hid_device_register_callback(esp_bt_hidd_cb);

    ESP_LOGI(TAG, "starting hid device");
    esp_bt_hid_device_init();
    ...
}
```

The information is set to global struct `s_local_param` and will be used upon successful profile initialization, i.e. reception of `ESP_HIDD_INIT_EVT` which is generated after the call of `esp_bt_hid_device_init()`:

```
void esp_bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    ...
    switch (event) {
    case ESP_HIDD_INIT_EVT:
        if (param->init.status == ESP_HIDD_SUCCESS) {
            ESP_LOGI(TAG, "setting hid parameters");
            esp_bt_hid_device_register_app(&s_local_param.app_param, &s_local_param.both_qos, &s_local_param.both_qos);
        } else {
            ESP_LOGE(TAG, "init hidd failed!");
        }
        break;
    ...
    }
    ...
}
```

### Determination of HID Report Mode

There are two HID report modes: Report Protocol Mode and Boot Protocol Mode. The former is the default mode. The two report modes differ in the report contents and format. The example supports both of the two modes.

Report Mode requires report descriptor to describe the usage and format of the reports. For Bluetooth HID device, the report descriptor shall be provided in the SDP server, which can be discovered and used by remote HID Host.

Boot Mode only supports keyboards and mice, with pre-defined report formats. Therefore it does not require a report descriptor parser on the remote HID Host. It is originally used to simplify the design of PC BIOSs.

The following code lines set Report Protocol Mode as the default Report Mode:

```
void app_main(void) {
    ...
    // Report Protocol Mode is the default mode, according to Bluetooth HID specification
    s_local_param.protocol_mode = ESP_HIDD_REPORT_MODE;
    ...
}
```

Report Mode can be chosen by remote HID Host through the SET_PROTOCOL request:

```
void esp_bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    ...
    switch (event) {
    ...
    case ESP_HIDD_SET_PROTOCOL_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_SET_PROTOCOL_EVT");
        if (param->set_protocol.protocol_mode == ESP_HIDD_BOOT_MODE) {
            ESP_LOGI(TAG, "  - boot protocol");
            xSemaphoreTake(s_local_param.mouse_mutex, portMAX_DELAY);
            s_local_param.x_dir = -1;
            xSemaphoreGive(s_local_param.mouse_mutex);
        } else if (param->set_protocol.protocol_mode == ESP_HIDD_REPORT_MODE) {
            ESP_LOGI(TAG, "  - report protocol");
        }
        xSemaphoreTake(s_local_param.mouse_mutex, portMAX_DELAY);
        s_local_param.protocol_mode = param->set_protocol.protocol_mode;
        xSemaphoreGive(s_local_param.mouse_mutex);
        break;
    ....
    }
    ....
}
```

### Report generation

The example simulates a mouse by creating a FreeRTOS task that periodically generates and sends the HID mouse report:

```
// move the mouse left and right
void mouse_move_task(void* pvParameters)
{
    const char* TAG = "mouse_move_task";

    ESP_LOGI(TAG, "starting");
    for(;;) {
        s_local_param.x_dir = 1;
        int8_t step = 10;
        for (int i = 0; i < 2; i++) {
            xSemaphoreTake(s_local_param.mouse_mutex, portMAX_DELAY);
            s_local_param.x_dir *= -1;
            xSemaphoreGive(s_local_param.mouse_mutex);
            for (int j = 0; j < 100; j++) {
                send_mouse_report(0, s_local_param.x_dir * step, 0, 0);
                vTaskDelay(50 / portTICK_PERIOD_MS);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
```

Function `send_mouse_report` is used to pack the information into a mouse HID report and sends it to HID Host, according to the Report Mode applied:

```
// send the buttons, change in x, and change in y
void send_mouse_report(uint8_t buttons, char dx, char dy, char wheel)
{
    uint8_t report_id;
    uint16_t report_size;
    xSemaphoreTake(s_local_param.mouse_mutex, portMAX_DELAY);
    if (s_local_param.protocol_mode == ESP_HIDD_REPORT_MODE) {
        report_id = 0;
        report_size = REPORT_PROTOCOL_MOUSE_REPORT_SIZE;
        s_local_param.buffer[0] = buttons;
        s_local_param.buffer[1] = dx;
        s_local_param.buffer[2] = dy;
        s_local_param.buffer[3] = wheel;
    } else {
        // Boot Mode
        report_id = ESP_HIDD_BOOT_REPORT_ID_MOUSE;
        report_size = ESP_HIDD_BOOT_REPORT_SIZE_MOUSE - 1;
        s_local_param.buffer[0] = buttons;
        s_local_param.buffer[1] = dx;
        s_local_param.buffer[2] = dy;
    }
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, report_id, report_size, s_local_param.buffer);
    xSemaphoreGive(s_local_param.mouse_mutex);
}
```
