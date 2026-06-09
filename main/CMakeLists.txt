file(GLOB_RECURSE app_sources 
    "${CMAKE_SOURCE_DIR}/main/*.c"
    "${CMAKE_SOURCE_DIR}/main/core/esp_comm_manager.c"
    "${CMAKE_SOURCE_DIR}/main/core/universal_ir.c"
)
# Exclude all Flipper NFC parser sources from the glob so we can whitelist only
# the parsers that are actually wired into the compat layer.
list(FILTER app_sources EXCLUDE REGEX "managers/nfc/flipper_parsers/.*\\.c")

# Exclude BLE sources for ESP32-S2 (no BLE hardware support)
# Use idf_build_get_property for reliable target detection in component CMake
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32s2")
    message(STATUS "Excluding BLE sources for ESP32-S2 target (no BLE hardware)")
    list(FILTER app_sources EXCLUDE REGEX "attacks/ble/.*\\.c")
    list(FILTER app_sources EXCLUDE REGEX "scans/ble/.*\\.c")
    list(FILTER app_sources EXCLUDE REGEX "managers/ble_manager\\.c")
endif()
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
list(APPEND app_sources "${CMAKE_SOURCE_DIR}/main/managers/settings_sd_backup.c")
# Exclude i80_display.c from the glob as it's conditionally included later
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
# Exclude camera managers until camera support is enabled.
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/managers/motion_detector_manager.c")
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/managers/camera_stream_manager.c")
file(GLOB_RECURSE image_sources "${CMAKE_SOURCE_DIR}/main/vendor/images/*.c")
list(APPEND image_sources "${CMAKE_SOURCE_DIR}/main/vendor/images/lock.c")
list(REMOVE_DUPLICATES image_sources)
file(GLOB_RECURSE axs15231b_sources "${CMAKE_SOURCE_DIR}/components/axs15231b/*.c")
file(GLOB m5gfx_sources
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/Fonts/efont/*.c"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/utility/*.c"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/v1/*.cpp"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/v1/misc/*.cpp"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/v1/panel/*.cpp"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/v1/platforms/esp32/*.cpp"
    "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/v1/touch/*.cpp"
)
set(lgfx_miniz_source "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/utility/lgfx_miniz.c")
list(REMOVE_ITEM m5gfx_sources "${lgfx_miniz_source}")
list(APPEND app_sources ${axs15231b_sources} ${image_sources} "${lgfx_miniz_source}")


set(required_components bt nvs_flash driver esp_driver_gpio esp_driver_i2c esp_driver_uart esp_driver_rmt esp_driver_i2s esp_driver_usb_serial_jtag esp_http_server mdns cjson esp_http_client mbedtls fatfs sdmmc wpa_supplicant lvgl lvgl_esp32_drivers freertos esp_lcd esp_adc esp_timer esp_app_format io_manager esp_eth espcoredump esp-tls)

# Optionally add extra include directories
set(extra_includes "")
list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/M5GFX/src")
if(CONFIG_USE_IO_EXPANDER)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/io_manager")
endif()

if(CONFIG_HAS_NFC)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp-idf-pn532/include")
endif()


set(sources ${app_sources})



## nfc build control: main still compiles NFC sources when NFC is enabled,
## so export pn532 headers whenever NFC support is on
if(CONFIG_HAS_NFC)
    list(APPEND required_components esp-idf-pn532)
endif()

if(idf_target STREQUAL "esp32s3" AND (CONFIG_USE_CARDPUTER OR CONFIG_USE_CARDPUTER_ADV))
    list(APPEND sources "vendor/m5/m5gfx_wrapper.cpp")
    list(APPEND sources ${m5gfx_sources})
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/M5GFX/src")
endif()

if(idf_target STREQUAL "esp32s3" AND CONFIG_USE_USB_KEYBOARD)
    list(APPEND required_components usb usb_host_hid)
endif()

# Add I80 display driver for TDisplay S3
if(idf_target STREQUAL "esp32s3" AND CONFIG_USE_TDISPLAY_S3)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
endif()

# Only pull in IO expander manager when enabled so headers are exported (all targets)
if(CONFIG_USE_IO_EXPANDER)
    list(APPEND required_components io_manager)
endif()

if(CONFIG_ENABLE_NATIVE_SD_APPS)
    list(APPEND required_components elf_loader)
endif()

# Add IEEE 802.15.4 component for Zigbee capture on ESP32-C5/C6
if(idf_target STREQUAL "esp32c5" OR idf_target STREQUAL "esp32c6")
    list(APPEND required_components ieee802154)
    # Add Zigbee manager source
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/zigbee_manager.c")
endif()

# Add Ethernet component when Ethernet is enabled
if(CONFIG_WITH_ETHERNET)
    list(APPEND required_components esp_eth)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/ethernet_manager.c")
endif()

# Add BadUSB manager and HID script parser when BadUSB is enabled (S3 only)
if(CONFIG_HAS_BADUSB)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/badusb_manager.c")
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/hid_script_parser.c")
    list(APPEND required_components esp_tinyusb)
endif()

# Add motion detector manager when camera is enabled (S3 only)
if(CONFIG_HAS_CAMERA)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/motion_detector_manager.c")
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/camera_stream_manager.c")
    list(APPEND required_components esp32-camera)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp32-camera/driver/include")
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp32-camera/conversions/include")
endif()

# Add TLV320DAC3100 driver component (code guarded by CONFIG_HAS_TLV320DAC_I2C)
list(APPEND required_components tlv320dac3100)

# Add audio codec component for audio receiver headers/libraries.
list(APPEND required_components esp_audio_codec)

set(embed_files_list "core/ouis.json")

if(CONFIG_HAS_NFC)
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" ${extra_includes}
                           REQUIRES ${required_components}
                           EMBED_TXTFILES "mf_classic_dict.nfc"
                           EMBED_FILES ${embed_files_list})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_MFC_DICT_EMBEDDED=1)
else()
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" ${extra_includes}
                           REQUIRES ${required_components}
                           EMBED_FILES ${embed_files_list})
endif()

# add compile definition for esp32s2 target to maintain legacy guards (needs COMPONENT_LIB)
if(idf_target STREQUAL "esp32s2")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_IDF_TARGET_ESP32S2)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-incompatible-pointer-types -Wno-unused-local-typedefs)

idf_build_get_property(idf_path IDF_PATH)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${idf_path}/components/wpa_supplicant/src"
    "${idf_path}/components/wpa_supplicant/esp_supplicant/src"
)
