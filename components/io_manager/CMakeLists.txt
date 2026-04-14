idf_component_register(
    SRCS "io_manager.c" "i2c_bus_lock.c" "i2c_shared.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_timer
)
