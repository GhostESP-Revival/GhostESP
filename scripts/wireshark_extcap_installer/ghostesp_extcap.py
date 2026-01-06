#!/usr/bin/env python3
"""
GhostESP Wireshark Extcap Interface
Captures WiFi/BLE packets from GhostESP devices and feeds them to Wireshark
"""

import sys
import os
import subprocess
import argparse
import struct
import time
from datetime import datetime

# Check and install dependencies
def check_dependencies():
    try:
        import serial
        import serial.tools.list_ports
    except ImportError:
        print("Installing required dependency: pyserial", file=sys.stderr)
        try:
            subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial", "--user"])
            import serial
            import serial.tools.list_ports
        except subprocess.CalledProcessError:
            print("Failed to install pyserial. Please run: pip install pyserial", file=sys.stderr)
            sys.exit(1)
        except ImportError:
            print("Failed to import pyserial after installation", file=sys.stderr)
            sys.exit(1)

# Install dependencies on import
check_dependencies()

import serial
import serial.tools.list_ports

EXTCAP_VERSION = "1.0"
INTERFACE_NAME = "ghostesp"

PCAP_MAGIC_BYTES_LE = b"\xd4\xc3\xb2\xa1"

def extcap_config(interface):
    ports = serial.tools.list_ports.comports()
    default_port = "NONE"

    if ports:
        default_port = ports[0].device

    print(
        f"arg {{number=0}}{{call=--port}}{{display=Serial Port}}{{type=selector}}{{default={default_port}}}{{tooltip=Select GhostESP serial port}}"
    )

    if not ports:
        print(f"value {{arg=0}}{{value=NONE}}{{display=No serial ports found - Connect your GhostESP}}{{default=true}}")
    else:
        for i, port in enumerate(ports):
            default_flag = "{default=true}" if i == 0 else "{default=false}"
            print(f"value {{arg=0}}{{value={port.device}}}{{display={port.device} - {port.description}}}{default_flag}")

    print(f"arg {{number=1}}{{call=--baud}}{{display=Baud Rate}}{{type=integer}}{{default=115200}}{{tooltip=Serial baud rate}}")
    print(f"arg {{number=2}}{{call=--capture-type}}{{display=Capture Type}}{{type=selector}}{{default=wifi}}{{tooltip=Type of capture}}")
    print(f"value {{arg=2}}{{value=wifi}}{{display=WiFi}}")
    print(f"value {{arg=2}}{{value=ble}}{{display=Bluetooth LE}}")
    
    print(f"arg {{number=3}}{{call=--channel-lock}}{{display=Channel Lock}}{{type=selector}}{{default=auto}}{{tooltip=Lock capture to specific WiFi channel (WiFi only)}}")
    print(f"value {{arg=3}}{{value=auto}}{{display=Auto (channel hopping)}}")
    
    # Add channel options based on target device
    max_channel = 13  # Default for most ESP32 targets
    for channel in range(1, max_channel + 1):
        print(f"value {{arg=3}}{{value={channel}}}{{display=Channel {channel}}}")

def extcap_interfaces():
    print(f"extcap {{version={EXTCAP_VERSION}}}{{help=https://github.com/jaylikesbunda/Ghost_ESP}}")
    print(f"interface {{value={INTERFACE_NAME}}}{{display=GhostESP WiFi/BLE Capture}}")
    
    dlt_wifi = 127
    dlt_ble = 187
    
    print(f"dlt {{number={dlt_wifi}}}{{name=IEEE802_11_RADIO}}{{display=802.11 plus radiotap header}}")
    print(f"dlt {{number={dlt_ble}}}{{name=BLUETOOTH_HCI_H4}}{{display=Bluetooth HCI UART transport}}")

def extcap_dlts(interface):
    if interface == INTERFACE_NAME:
        print("dlt {number=127}{name=IEEE802_11_RADIO}{display=802.11 plus radiotap header}")
        print("dlt {number=187}{name=BLUETOOTH_HCI_H4}{display=Bluetooth HCI UART transport}")

def _read_exact(ser, size, deadline):
    buf = bytearray()
    while len(buf) < size and time.monotonic() < deadline:
        chunk = ser.read(size - len(buf))
        if chunk:
            buf.extend(chunk)
    if len(buf) != size:
        return None
    return bytes(buf)


def _is_plausible_packet_header(ts_sec, ts_usec, incl_len, orig_len, snaplen):
    if ts_usec >= 1000000:
        return False
    if incl_len == 0:
        return False
    if incl_len > snaplen or orig_len > snaplen:
        return False
    if orig_len < incl_len:
        return False
    return True


def _serial_read_some(ser):
    n = ser.in_waiting
    if n and n > 0:
        return ser.read(n)
    return ser.read(1)


def find_pcap_global_header(ser, timeout_s=10.0):
    deadline = time.monotonic() + timeout_s
    window = bytearray()

    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue

        window.extend(b)
        if len(window) > 4:
            del window[:-4]

        if bytes(window) != PCAP_MAGIC_BYTES_LE:
            continue

        rest = _read_exact(ser, 20, deadline)
        if rest is None:
            return None

        header = PCAP_MAGIC_BYTES_LE + rest
        magic, version_major, version_minor, thiszone, sigfigs, snaplen, network = struct.unpack(
            "<IHHiIII", header
        )

        if magic != 0xA1B2C3D4:
            continue

        return network, header

    return None

def capture(interface, port, baud, capture_type, channel_lock, fifo):
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as e:
        sys.stderr.write(f"Failed to open serial port {port}: {e}\n")
        return 1
    
    sys.stderr.write(f"Connected to {port} at {baud} baud\n")

    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    
    if capture_type == "ble":
        command = "capture -wiresharkble\n"
    else:
        # Build command with channel lock if specified
        if channel_lock != "auto":
            command = f"capture -wireshark -c {channel_lock}\n"
        else:
            command = "capture -wireshark\n"
    
    ser.write(command.encode())
    ser.flush()
    
    sys.stderr.write(f"Starting {capture_type} capture")
    if capture_type == "wifi" and channel_lock != "auto":
        sys.stderr.write(f" on channel {channel_lock}")
    sys.stderr.write("...\n")
    
    sys.stderr.write("Waiting for PCAP header...\n")

    found = find_pcap_global_header(ser, timeout_s=10.0)
    if found is None:
        sys.stderr.write("Failed to read PCAP header\n")
        return 1

    dlt, header_bytes = found
    sys.stderr.write(f"PCAP stream started (DLT={dlt})\n")
    
    with open(fifo, 'wb', buffering=0) as out:
        out.write(header_bytes)

        snaplen = 65535
        try:
            _magic, _vmaj, _vmin, _tz, _sig, snaplen, _net = struct.unpack("<IHHiIII", header_bytes)
        except struct.error:
            snaplen = 65535

        try:
            buf = bytearray()
            while True:
                chunk = _serial_read_some(ser)
                if chunk:
                    buf.extend(chunk)

                while len(buf) >= 16:
                    ts_sec, ts_usec, incl_len, orig_len = struct.unpack_from("<IIII", buf, 0)
                    if not _is_plausible_packet_header(ts_sec, ts_usec, incl_len, orig_len, snaplen):
                        del buf[0]
                        continue

                    needed = 16 + incl_len
                    if len(buf) < needed:
                        break

                    out.write(buf[:16])
                    out.write(buf[16:needed])
                    del buf[:needed]

        except KeyboardInterrupt:
            sys.stderr.write("\nCapture stopped by user\n")
        finally:
            ser.write(b"capture -stop\n")
            ser.flush()
            ser.close()
    
    return 0

def main():
    parser = argparse.ArgumentParser(description="GhostESP Wireshark Extcap")
    
    parser.add_argument("--extcap-interfaces", action="store_true", help="List interfaces")
    parser.add_argument("--extcap-interface", help="Interface name")
    parser.add_argument("--extcap-dlts", action="store_true", help="List DLTs")
    parser.add_argument("--extcap-config", action="store_true", help="List configuration options")
    parser.add_argument("--extcap-version", action="store_true", help="Show version")
    parser.add_argument("--capture", action="store_true", help="Start capture")
    parser.add_argument("--fifo", help="Output FIFO")
    parser.add_argument("--port", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--capture-type", default="wifi", help="Capture type")
    parser.add_argument("--channel-lock", default="auto", help="Channel lock setting")
    
    args = parser.parse_args()
    
    if args.extcap_interfaces:
        extcap_interfaces()
        return 0
    
    if args.extcap_dlts:
        extcap_dlts(args.extcap_interface)
        return 0
    
    if args.extcap_config:
        extcap_config(args.extcap_interface)
        return 0
    
    if args.extcap_version:
        print(f"extcap {{version={EXTCAP_VERSION}}}")
        return 0
    
    if args.capture:
        if not args.fifo:
            sys.stderr.write("ERROR: Missing --fifo argument\n")
            sys.stderr.write("This is an internal Wireshark error.\n")
            return 1
        if not args.port or args.port == "NONE":
            sys.stderr.write("ERROR: No serial port selected\n")
            sys.stderr.write("To configure the GhostESP interface:\n")
            sys.stderr.write("1. Right-click 'GhostESP WiFi/BLE Capture' in Wireshark\n")
            sys.stderr.write("2. Select 'Capture Options' or 'Configure'\n")
            sys.stderr.write("3. Choose your GhostESP COM port from the dropdown\n")
            sys.stderr.write("4. Click Start to begin capturing\n")
            return 1
        
        return capture(args.extcap_interface, args.port, args.baud, args.capture_type, args.channel_lock, args.fifo)
    
    parser.print_help()
    return 0

if __name__ == "__main__":
    sys.exit(main())
