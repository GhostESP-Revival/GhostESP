| Supported Hosts | ESP32-P4 |
| --------------- | -------- |

| Supported Co-processors | ESP32-C5 | ESP32-C6 | ESP32-H2 |
|-------------------------|----------|----------|----------|

# OpenThread Command Line Example

This example demonstrates an [OpenThread CLI](https://github.com/openthread/openthread/blob/master/src/cli/README.md), with some additional features such as iperf.

## Example 1: OpenThread Host communicating with Openthread RCP over UART

The example runs on an ESP32-P4, connected to a ESP32-C6 DevKit as the co-processor via the GPIO Header. ESP-Hosted transport is SPI-FD (Full duplex) while UART is used for OpenThread communications.

### Setting up the hardware

This table shows the Hardware Connections between the ESP32-P4 and C6.

**SPI-FD Connection Between ESP32-P4 and ESP32-C6 co-processor**

|            | ESP32-P4 GPIO | ESP32-C6 GPIO |
|------------|--------------:|--------------:|
| MOSI       |             4 |             7 |
| MISO       |             5 |             2 |
| CLK        |            26 |             6 |
| CS         |             6 |            10 |
| Handshake  |            20 |             3 |
| Data Ready |            32 |             4 |
| C6 Reset   |             2 |           RST |

**UART Connection Between ESP32-P4 and ESP32-C6 co-processor**

|  ESP32-P4 GPIO | ESP32-C6 GPIO |
|---------------:|--------------:|
| (To RCP Tx) 24 |       (Tx) 21 |
| (To RCP Rx) 25 |       (Rx) 20 |

### Configure the project for ESP32-P4

On the command-line:

```bash
idf.py set-target esp32p4
idf.py menuconfig
```

Configure the project:

```
Component config
└── ESP-Hosted config
    ├── Configure GPIOs for Development Board ──> No development board
    ├── Transport layer ──> SPI Full-duplex
    ├── SPI Configuration ──> (Configure GPIOs)
    │   ⋮
    └── [*] Enable OpenThread Host Support
        └── OpenThread RCP Configuration
            ├── OpenThread Transport ──> UART
            └── (configure UART parameters)
```

### Configure the project for ESP32-C6

In the ESP-Hosted project `slave` directory:

Edit `sdkconfig.defaults.esp32c6` and enable the OpenThread section:

```
#
# OpenThread
#
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_RADIO=y
CONFIG_OPENTHREAD_DIAG=n
CONFIG_OPENTHREAD_COMMISSIONER=n
CONFIG_OPENTHREAD_JOINER=n
CONFIG_OPENTHREAD_BORDER_ROUTER=n
CONFIG_OPENTHREAD_CLI=n
CONFIG_OPENTHREAD_SRP_CLIENT=n
CONFIG_OPENTHREAD_DNS_CLIENT=n
CONFIG_OPENTHREAD_TASK_SIZE=3072
CONFIG_OPENTHREAD_CONSOLE_ENABLE=n
CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC=n

CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
# end of OpenThread
```

On the command-line:

```bash
idf.py set-target esp32c6
idf.py menuconfig
```

Configure the project:

```
Example Configuration
├── Bus Config in between Host and Co-processor
│   ├── Transport layer ──> SPI Full-duplex
│   └── SPI Full-duplex Configuration ──> (Configure GPIOs)
│   ⋮
└── [*] Enable OpenThread RCP (Radio Co-Processor)
    └── OpenThread RCP Configuration
        ├── OpenThread Transport ──> UART
        └── (configure UART parameters)
```

If you did not edit `sdkconfig.defaults.esp32c6` to enable OpenThread (above), modify these settings:

```
Component config
├── Wireless Coexistence
│   └── [*] Software controls WiFi/Bluetooth coexistence
└── OpenThread
    ├── [*] OpenThread
    ├── Thread Task Parameters
    │   └── (3072) Size of OpenThread task
    ├── Thread Console
    │   ├── [ ] Enable OpenThread console
    │   └── [ ] Enable Openthread Command-Line Interface
    ├── Thread Core Features
    │   ├── Thread device type ──> Radio Only Device
    │   ├── [ ] Enable Commissioner
    │   ├── [ ] Enable Joiner
    │   ├── [ ] Enable SRP Client
    │   ├── [ ] Enable DNS Client
    │   ├── [ ] Enable diag Client
    └── Thread Log
        └── [ ] Enable dynamic log level control
```

### Build, Flash, and Run

Build the projects and flash them to the appropriate boards, then run monitor tool to view serial output:

```bash
idf.py -p PORT build flash monitor
```

On the ESP32-P4 you'll get an OpenThread command line shell.

### Example Output

The `help` command will print all of the supported commands.
```text
esp32h2> ot help
I(7058) OPENTHREAD:[INFO]-CLI-----: execute command: help
bbr
bufferinfo
ccathreshold
channel
child
childip
childmax
childsupervision
childtimeout
coap
contextreusedelay
counters
dataset
delaytimermin
diag
discover
dns
domainname
eidcache
eui64
extaddr
extpanid
factoryreset
...
```

## Set Up Network

To run this example, one more board (for example, an ESP32-H2) with the ESP-IDF [ot_cli example](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_cli) is required..

On the ESP32-P4, run the following commands:

```test
esp32p4> ot factoryreset
... # the device will reboot

esp32p4> ot dataset init new
Done
esp32p4> ot dataset commit active
Done
esp32p4> ot ifconfig up
Done
esp32p4> ot thread start
Done

# After some seconds

esp32p4> ot state
leader
Done
```

Now the ESP32-P4 has formed a Thread network as a leader. Get some information which will be used in next steps:

```text
esp32p4> ot ipaddr
fdde:ad00:beef:0:0:ff:fe00:fc00
fdde:ad00:beef:0:0:ff:fe00:8000
fdde:ad00:beef:0:a7c6:6311:9c8c:271b
fe80:0:0:0:5c27:a723:7115:c8f8

# Get the Active Dataset
esp32p4> ot dataset active -x
0e080000000000010000000300001835060004001fffe00208fe7bb701f5f1125d0708fd75cbde7c6647bd0510b3914792d44f45b6c7d76eb9306eec94030f4f70656e5468726561642d35383332010258320410e35c581af5029b054fc904a24c2b27700c0402a0fff8
```

On the ESP32-H2, set the active dataset from leader, and start Thread interface:

```text
esp32h2> ot factoryreset
... # the device will reboot

esp32h2> ot dataset set active 0e080000000000010000000300001835060004001fffe00208fe7bb701f5f1125d0708fd75cbde7c6647bd0510b3914792d44f45b6c7d76eb9306eec94030f4f70656e5468726561642d35383332010258320410e35c581af5029b054fc904a24c2b27700c0402a0fff8
esp32h2> ot ifconfig up
Done
esp32h2> ot thread start
Done

# After some seconds

esp32h2> ot state
router  # child is also a valid state
Done
```
The second device has joined the Thread network as a router (or a child).

## Extension commands

You can refer to the [extension command](https://github.com/espressif/esp-thread-br/blob/main/components/esp_ot_cli_extension/README.md) about the extension commands.

The following examples are supported by `ot_cli`:

* TCP and UDP Example

## Using iPerf to measure bandwidth

iPerf is a tool used to obtain TCP or UDP throughput on the Thread network. To run iPerf, you need to have two Thread devices on the same network.

Refer to [the iperf-cmd component](https://components.espressif.com/components/espressif/iperf-cmd) for details on specific configurations.

### Typical usage on a thread network

> [!NOTE]
> The [ML-EID](https://openthread.io/guides/thread-primer/ipv6-addressing#unicast_address_types) address is used for iperf.

For measuring the TCP throughput, first get the ML-EID address, then create an iperf service on one node:

```text
> ot ipaddr mleid
fdde:ad00:beef:0:a7c6:6311:9c8c:271b
Done

> iperf -V -s -t 20 -i 3 -p 5001 -f k
Done
```

Then create an iperf client connecting to the service on another node.

```text
> iperf -V -c fdde:ad00:beef:0:a7c6:6311:9c8c:271b -t 20 -i 1 -p 5001 -l 85 -f k
Done
[ ID] Interval		Transfer	Bandwidth
[  1]  0.0- 1.0 sec	3.15 KBytes	25.16 Kbits/sec
[  1]  1.0- 2.0 sec	2.89 KBytes	23.12 Kbits/sec
[  1]  2.0- 3.0 sec	2.98 KBytes	23.80 Kbits/sec
...
[  1]  9.0-10.0 sec	2.55 KBytes	20.40 Kbits/sec
[  1]  0.0-10.0 sec	27.80 KBytes	22.24 Kbits/sec
```
