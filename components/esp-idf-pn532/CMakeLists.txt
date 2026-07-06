if(CONFIG_NFC_PN532 OR CONFIG_NFC_ST25R3916)
idf_component_register(
    SRCS
        src/pn532.c
        src/pn532_driver.c
        src/pn532_driver_i2c.c
    INCLUDE_DIRS
        include
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        io_manager
)
else()
idf_component_register()
endif()


