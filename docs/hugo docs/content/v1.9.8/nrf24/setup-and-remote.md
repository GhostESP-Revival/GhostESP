---
title: "Setup and Remote Mode"
description: "Configure NRF24 hardware pins and GhostLink remote analyzer control"
weight: 20
---

This page covers local NRF24 wiring/configuration and remote control mode over GhostLink.

## Local NRF24 setup

Enable the analyzer in project config:

- `CONFIG_HAS_NRF24`
- `CONFIG_NRF24_SPI_HOST` (`2` or `3`)
- `CONFIG_NRF24_SPI_MOSI_PIN`
- `CONFIG_NRF24_SPI_MISO_PIN`
- `CONFIG_NRF24_SPI_SCK_PIN`
- `CONFIG_NRF24_CSN_PIN`
- `CONFIG_NRF24_CE_PIN`
- `CONFIG_NRF24_SPI_CLOCK_HZ`

Analyzer behavior tuning:

- `CONFIG_NRF24_ANALYZER_SAMPLES_PER_CHANNEL`
- `CONFIG_NRF24_ANALYZER_SETTLE_US`
- `CONFIG_NRF24_ANALYZER_CHANNELS_PER_TICK`

Pin defaults depend on your selected board config. In `menuconfig`, these options are under **Ghost ESP Options -> Misc Options -> NRF24 Options**.

## GhostLink remote mode

Remote mode is for display/controller builds that do not have local NRF24 hardware.

- Enable `CONFIG_HAS_NRF24_REMOTE` on the controller/display build.
- Pair to a peer device that is built with `CONFIG_HAS_NRF24` and physically connected to an NRF24 module.
- Start the analyzer from the controller UI; commands are forwarded over GhostLink.

For pairing and transport setup, see [Dual Communication (GhostLink)]({{< relref "../getting-started/dual-communication.md" >}}).

## Troubleshooting

- **`NRF24 analyzer failed to start`**: check wiring, power, and SPI pin mapping.
- **`built without CONFIG_HAS_NRF24`**: rebuild firmware with local NRF24 support.
- **No remote stream updates**: verify GhostLink connection and that the peer analyzer actually started.
