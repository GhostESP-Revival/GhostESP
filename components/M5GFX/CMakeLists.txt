if(NOT CONFIG_WITH_SCREEN AND NOT CONFIG_WITH_STATUS_DISPLAY)
    message(STATUS "Explicitly excluding M5GFX component because no display is enabled")
    return()
endif()

if("${IDF_TARGET}" STREQUAL "esp32c5" OR "${IDF_TARGET}" STREQUAL "esp32c6" OR "${IDF_TARGET}" STREQUAL "esp32s2" OR "${IDF_TARGET}" STREQUAL "esp32c3")
    message(STATUS "Explicitly excluding M5GFX component for ${IDF_TARGET}")
    return()
endif()

file(GLOB SRCS
     src/*.cpp
     src/lgfx/Fonts/efont/*.c
     src/lgfx/Fonts/IPA/*.c
     src/lgfx/utility/*.c
     src/lgfx/v1/*.cpp
     src/lgfx/v1/misc/*.cpp
     src/lgfx/v1/panel/*.cpp
     src/lgfx/v1/platforms/esp32/*.cpp
     src/lgfx/v1/platforms/esp32c3/*.cpp
     src/lgfx/v1/platforms/esp32s3/*.cpp
      src/lgfx/v1/platforms/esp32p4/*.cpp
      src/lgfx/v1/touch/*.cpp
      )

if (IDF_VERSION_MAJOR GREATER_EQUAL 5)
    set(COMPONENT_REQUIRES nvs_flash efuse driver esp_timer esp_lcd esp_mm esp_driver_ledc esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_dma)
elseif ((IDF_VERSION_MAJOR EQUAL 4) AND (IDF_VERSION_MINOR GREATER 3) OR IDF_VERSION_MAJOR GREATER 4)
    set(COMPONENT_REQUIRES nvs_flash efuse esp_lcd)
else()
    set(COMPONENT_REQUIRES nvs_flash efuse)
endif()

### If you use arduino-esp32 components, please activate next comment line.
# list(APPEND COMPONENT_REQUIRES arduino-esp32)


message(STATUS "M5GFX use components = ${COMPONENT_REQUIRES}")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS src
    REQUIRES ${COMPONENT_REQUIRES}
)
