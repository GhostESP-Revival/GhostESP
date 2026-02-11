idf_component_register(
    SRCS "io_manager.c" "i2c_bus_lock.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/main"
    REQUIRES driver esp_timer lvgl
)
