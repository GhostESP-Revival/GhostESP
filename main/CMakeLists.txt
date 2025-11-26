file(GLOB_RECURSE app_sources 
    "${CMAKE_SOURCE_DIR}/main/*.c"
    "${CMAKE_SOURCE_DIR}/main/core/esp_comm_manager.c"
    "${CMAKE_SOURCE_DIR}/main/core/universal_ir.c"
)
# Exclude all Flipper NFC parser sources from the glob so we can whitelist only
# the parsers that are actually wired into the compat layer.
list(FILTER app_sources EXCLUDE REGEX "managers/nfc/flipper_parsers/.*\\.c")
# Explicitly add hooked-up Flipper parsers
list(APPEND app_sources
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/smartrider.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/aime.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/csc.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/washcity.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/metromoney.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/bip.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/charliecard.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/disney_infinity.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/hi.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/hid.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/hworld.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/kazan.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/microel.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/mizip.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/plantain.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/saflok.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/skylanders.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/social_moscow.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/troika.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/two_cities.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/umarsh.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/zolotaya_korona.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/zolotaya_korona_online.c"
)
# Explicitly include infrared view implementation
list(APPEND app_sources "${CMAKE_SOURCE_DIR}/main/managers/views/infrared_view.c")
# Exclude i80_display.c from the glob as it's conditionally included later
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
file(GLOB_RECURSE image_sources "${CMAKE_SOURCE_DIR}/main/vendor/images/*.c")
file(GLOB_RECURSE axs15231b_sources "${CMAKE_SOURCE_DIR}/components/axs15231b/*.c")


list(APPEND app_sources ${axs15231b_sources} ${image_sources})


set(required_components bt nvs_flash driver esp_http_server mdns json esp_http_client mbedtls fatfs sdmmc wpa_supplicant lvgl lvgl_esp32_drivers freertos esp_lcd esp_adc esp_timer io_manager)

# Optionally add extra include directories
set(extra_includes "")
if(CONFIG_USE_IO_EXPANDER)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/io_manager")
endif()


set(sources ${app_sources})



## nfc build control: only include pn532 component/sources when pn532 is enabled
if(CONFIG_HAS_NFC)
    if(CONFIG_NFC_PN532)
        list(APPEND required_components esp-idf-pn532)
        list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp-idf-pn532/include")
    endif()
endif()

if("${IDF_TARGET}" STREQUAL "esp32s3")
    # Add the M5GFX wrapper source file
    list(APPEND sources "vendor/m5/m5gfx_wrapper.cpp")
    # Add M5GFX to the required components
    list(APPEND required_components M5GFX)
    
    # Add I80 display driver for TDisplay S3
    if(CONFIG_USE_TDISPLAY_S3)
        list(APPEND sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
    endif()
endif()

# Only pull in IO expander manager when enabled so headers are exported (all targets)
if(CONFIG_USE_IO_EXPANDER)
    list(APPEND required_components io_manager)
endif()

# Add IEEE 802.15.4 component for Zigbee capture on ESP32-C5/C6
if("${IDF_TARGET}" STREQUAL "esp32c5" OR "${IDF_TARGET}" STREQUAL "esp32c6")
    list(APPEND required_components ieee802154)
    # Add Zigbee manager source
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/zigbee_manager.c")
endif()

if(CONFIG_HAS_NFC)
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" ${extra_includes}
                           REQUIRES ${required_components}
                           EMBED_TXTFILES "mf_classic_dict.nfc")
    # Signal to C code that the dictionary is embedded so it can reference linker symbols safely
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_MFC_DICT_EMBEDDED=1)
else()
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" ${extra_includes}
                           REQUIRES ${required_components})
endif()

# add compile definition for esp32s2 target to maintain legacy guards (needs COMPONENT_LIB)
if("${IDF_TARGET}" STREQUAL "esp32s2")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_IDF_TARGET_ESP32S2)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-incompatible-pointer-types -Wno-unused-local-typedefs)

