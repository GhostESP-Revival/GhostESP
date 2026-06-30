# esp-idf-st25r3916

GhostESP's ESP-IDF driver for the **ST25R3916 / ST25R3916B** NFC reader IC.
It supports **SPI and I2C**, and plugs into the same high-level NFC flow we use
for PN532 so the app code can switch between frontends without a big rewrite.

## Notes

- License is GPL-3.0-or-later, same as GhostESP.
- The ST datasheet was used for the register map, direct commands, FIFO behavior,
  IRQs, and the basic bring-up sequence.
- Flipper Zero / Momentum-Firmware was useful reference material for the real-world
  ST25R3916 register values around NFC-A listener mode, load modulation, field
  thresholds, and emulation behavior.
- This component is our driver for GhostESP. It does not pull in ST RFAL or vendor
  middleware.

## Layout

```
include/
  st25r3916_hw_config.h   bus-agnostic hardware config (SPI or I2C selector)
  st25r3916_reg.h         register/command/bit definitions (datasheet)
  st25r3916.h             core driver API (bring-up, field, FIFO, IRQ)
hal/
  include/hb_nfc_bus.h    bus dispatch (vtable over SPI/I2C)
  include/hb_nfc_spi.h    SPI transport
  include/hb_nfc_i2c.h    I2C transport
  hb_nfc_bus.c            selects + dispatches to the active transport
  hb_nfc_spi.c            SPI framing (DS Table 11 operation modes)
  hb_nfc_i2c.c            I2C framing (shared/locked bus via io_manager)
src/
  st25r3916.c             core driver
  st25r3916_target.c      NFC-A passive target / card emulation
  st25r3916_iso14443a.c   ISO14443-A poller activation/transceive
  st25r3916_mifare.c      MIFARE Classic software auth (Crypto1)
  crypto1.c               Crypto1 helper
  st25r3916_adapter.c     pn532_io_t frontend adapter
```

ISO14443-A activation/transceive, MIFARE Crypto1, and the pn532-compatible
frontend adapter are layered on top of the core chip driver.

The chip is exposed to GhostESP through the existing `pn532_io_t` frontend
vtable, so the high-level NFC code (`main/managers/nfc/*`) drives either a PN532
or an ST25R3916 unchanged.
