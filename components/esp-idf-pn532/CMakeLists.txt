if(CONFIG_HAS_NFC)
idf_component_register(
    SRCS
        src/pn532.c
        src/pn532_driver.c
        src/pn532_driver_i2c.c
    INCLUDE_DIRS
        include
    REQUIRES
        driver io_manager
)
endif()


