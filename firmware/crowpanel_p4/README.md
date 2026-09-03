# CrowPanel P4 C6 Firmware

`network_adapter.bin` is the ESP32-C6 ESP-Hosted slave image used by the
CrowPanel P4 host. It must be built from the same ESP-Hosted component version
as the P4 host and is flashed into the `slave_fw` P4 data partition.

The bundled image is GhostESP ESP-Hosted 2.12.16 for ESP32-C6. P4 host pin mappings are
selected by the GhostESP board profile; the same C6 image is used by v1.1 and
newer CrowPanel revisions.

This local revision adds read-only STA diagnostics to P4 `ifconfig`. It reports
the C6 radio MAC, host-frame delivery counts, Wi-Fi TX results, and DHCP RX/TX
metadata/checksums. Wi-Fi TX success means driver acceptance, not RF delivery.
The diagnostic reply uses a fifth C6 custom-message handler. Packet callbacks
only update bounded counters/snapshots; reporting happens on request outside
the Wi-Fi receive task. No network settings or packet contents are changed.

After a full P4 flash (not `app-flash`), allow the automatic C6 update/reboot
and confirm that the reported C6 version is 2.12.16. Attempt STA connection, wait
about 20 seconds, then run `ifconfig` and collect the `P4_STA` lines. Counters
are cumulative since C6 boot and DHCP RX totals also include broadcasts for
other clients; compare transaction IDs and client MACs before interpreting
an OFFER/ACK as a response to this device.
