set(st25r3916_sources "")

if(CONFIG_NFC_ST25R3916)
    list(APPEND st25r3916_sources
        src/st25r3916.c
        src/st25r3916_iso14443a.c
        src/st25r3916_iso15693.c
        src/st25r3916_target.c
        src/st25r3916_mifare.c
        src/crypto1.c
        src/st25r3916_adapter.c
        hal/hb_nfc_bus.c
        hal/hb_nfc_spi.c
        hal/hb_nfc_i2c.c
    )
endif()

idf_component_register(
    SRCS ${st25r3916_sources}
    INCLUDE_DIRS
        include
        hal/include
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp-idf-pn532
        io_manager
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
)
