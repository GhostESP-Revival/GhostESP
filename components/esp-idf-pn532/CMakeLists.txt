set(pn532_sources "")

if(CONFIG_NFC_PN532 OR CONFIG_NFC_ST25R3916)
    list(APPEND pn532_sources
        src/pn532.c
        src/pn532_driver.c
        src/pn532_driver_i2c.c
    )
endif()

idf_component_register(
    SRCS ${pn532_sources}
    INCLUDE_DIRS
        include
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        io_manager
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
)
