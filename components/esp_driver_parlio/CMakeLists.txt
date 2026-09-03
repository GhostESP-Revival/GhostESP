idf_build_get_property(target IDF_TARGET)

set(srcs)
set(public_include "include")
set(priv_requires "")
# Vendored snapshot only carries ESP32-C5 low-level files (the HAL target
# headers live in ../esp_hal_parlio/<target>/include). On any other target
# register an empty component so PARLIO-capable non-C5 targets (e.g. ESP32-C6)
# don't try to compile C5-only sources.
if(target STREQUAL "esp32c5")
if(CONFIG_SOC_PARLIO_SUPPORTED)
    list(APPEND srcs "src/parlio_common.c"
                     "src/parlio_tx.c"
                     "src/parlio_rx.c")
endif()

if(CONFIG_SOC_BITSCRAMBLER_SUPPORTED)
    list(APPEND srcs "src/parlio_bitscrambler.c")
endif()

    set(priv_requires esp_pm esp_driver_gpio esp_mm esp_driver_bitscrambler esp_driver_dma)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${public_include}
                       PRIV_REQUIRES "${priv_requires}"
                       REQUIRES esp_hal_parlio
                       LDFRAGMENTS "linker.lf"
                      )
