file(GLOB_RECURSE app_sources 
    "${CMAKE_SOURCE_DIR}/main/*.c"
    "${CMAKE_SOURCE_DIR}/main/core/esp_comm_manager.c"
    "${CMAKE_SOURCE_DIR}/main/core/universal_ir.c"
)
# Exclude all Flipper NFC parser sources from the glob so we can whitelist only
# the parsers that are actually wired into the compat layer.
list(FILTER app_sources EXCLUDE REGEX "managers/nfc/flipper_parsers/.*\\.c")

# Exclude BLE sources for targets without local BLE hardware.
# Use idf_build_get_property for reliable target detection in component CMake
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32s2")
    message(STATUS "Excluding BLE sources for ${idf_target} target (no local BLE hardware)")
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
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/myki.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/clipper.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/itso.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/opal.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/gallagher.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/gallagher_util.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/flipper_parsers/emv_card.c"
    "${CMAKE_SOURCE_DIR}/main/managers/nfc/emv.c"
)
# Exclude infrared view from glob; conditionally included below when IR is enabled
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/managers/views/infrared_view.c")
list(APPEND app_sources "${CMAKE_SOURCE_DIR}/main/managers/settings_sd_backup.c")
# Exclude i80_display.c from the glob as it's conditionally included later
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
# Exclude camera managers until camera support is enabled.
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/managers/motion_detector_manager.c")
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/managers/camera_stream_manager.c")
# OTA managers remain in every build. Their GHOSTESP_OTA_SUPPORTED guards
# compile lightweight stubs on boards without an OTA-capable flash layout.
# Add infrared view back when IR support is enabled
if(CONFIG_HAS_INFRARED)
    list(APPEND app_sources "${CMAKE_SOURCE_DIR}/main/managers/views/infrared_view.c")
endif()
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

# Screenless boards still compile the lockscreen, which references the pet mood
# sprites, so --gc-sections cannot drop them. Excluding the sources here saves
# ~111 KB, which is what keeps 4 MB-flash boards inside their app partition.
if(NOT CONFIG_WITH_SCREEN)
    list(FILTER app_sources EXCLUDE REGEX "managers/views/ghostchi_screen\\.c")
    list(FILTER app_sources EXCLUDE REGEX "vendor/images/ghostchi/.*\\.c")
endif()

set(required_components bt nvs_flash driver esp_driver_gpio esp_driver_i2c esp_driver_uart esp_driver_rmt esp_driver_i2s esp_driver_usb_serial_jtag esp_http_server mdns cjson esp_http_client mbedtls fatfs sdmmc wpa_supplicant lvgl lvgl_esp32_drivers freertos esp_lcd esp_mm esp_adc esp_timer esp_app_format io_manager esp_eth espcoredump esp-tls app_update esp_https_ota spi_flash bootloader_support)

# The P4 hosts Wi-Fi and Bluetooth through the onboard C6. The P4 runs the
# NimBLE host while ESP-Hosted supplies the C6 controller over VHCI.
if(idf_target STREQUAL "esp32p4")
    list(REMOVE_ITEM app_sources
         "${CMAKE_SOURCE_DIR}/main/managers/espnow_manager.c"
         "${CMAKE_SOURCE_DIR}/main/attacks/wifi/gtk_abuse.c")
    list(APPEND app_sources
         "${CMAKE_SOURCE_DIR}/main/managers/espnow_manager_stub.c"
         "${CMAKE_SOURCE_DIR}/main/attacks/wifi/gtk_abuse_stub.c")
    # Keep these target-gated rather than profile-gated. ESP-IDF may rebuild
    # the component requirements graph without sdkconfig values available.
    list(APPEND required_components esp_wifi esp_hosted esp_wifi_remote esp_lcd_ek79007 esp_lcd_touch_gt911 esp_mm esp_driver_ppa esp_video)
endif()

# Optionally add extra include directories
set(extra_includes "")
list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/M5GFX/src")
if(CONFIG_USE_IO_EXPANDER)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/io_manager")
endif()

if(CONFIG_NFC_PN532 OR CONFIG_NFC_ST25R3916)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp-idf-pn532/include")
endif()

if(CONFIG_NFC_ST25R3916)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp-idf-st25r3916/include")
endif()


set(sources ${app_sources})

# The e-paper Reader decodes JPEG pages directly into its LVGL framebuffer.
# Keep TJpgDec out of the other targets so their existing image pipelines and
# binary sizes remain unchanged.
if(idf_target STREQUAL "esp32s3" AND CONFIG_CROWPANEL_EPAPER_42)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/utility/lgfx_tjpgd.c")
    # ESP32-S3 has a ROM JPEG decoder. Use it for the reader instead of the
    # slower software TJpgDec path while retaining the software fallback in
    # the reader source for non-ROM configurations.
    if(CONFIG_ESP_ROM_HAS_JPEG_DECODE)
        list(APPEND sources "${CMAKE_SOURCE_DIR}/components/esp32-camera/conversions/esp_jpg_decode.c")
        list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/esp32-camera/conversions/include")
    endif()
    set_source_files_properties("${CMAKE_SOURCE_DIR}/components/M5GFX/src/lgfx/utility/lgfx_miniz.c"
        PROPERTIES COMPILE_DEFINITIONS "LGFX_MINIZ_ENABLE_ARCHIVE=1")
endif()



## nfc build control: main still compiles NFC sources when NFC is enabled,
## so export pn532 headers whenever NFC support is on
if(CONFIG_NFC_PN532 OR CONFIG_NFC_ST25R3916)
    list(APPEND required_components esp-idf-pn532)
endif()

if(CONFIG_NFC_ST25R3916)
    list(APPEND required_components esp-idf-st25r3916)
endif()

if(idf_target STREQUAL "esp32s3" AND (CONFIG_USE_CARDPUTER OR CONFIG_USE_CARDPUTER_ADV OR CONFIG_IS_ATOMS3R))
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

if(CONFIG_ENABLE_GHOSTSCRIPT)
    list(APPEND required_components lua)
    list(APPEND extra_includes "${CMAKE_SOURCE_DIR}/components/lua/src")
else()
    list(FILTER app_sources EXCLUDE REGEX "managers/ghostscript_.*\\.c")
    list(FILTER app_sources EXCLUDE REGEX "managers/views/ghostscript_.*\\.c")
    list(FILTER sources EXCLUDE REGEX "managers/ghostscript_.*\\.c")
    list(FILTER sources EXCLUDE REGEX "managers/views/ghostscript_.*\\.c")
    # Core subsystems emit optional GhostScript events on every target.
    # Keep those calls linkable when the runtime is disabled for a board.
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/ghostscript_runtime_stub.c")
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
    list(APPEND required_components esp_tinyusb)
endif()

# BadBLE uses the shared transport-neutral DuckyScript parser and the IDF HID
# component.  Keep this separate from BadUSB so non-S3 BLE targets do not pull
# in TinyUSB.
if(CONFIG_HAS_BADBLE)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/badble_manager.c")
    list(APPEND required_components esp_hid)
    # IDF 6's component-requirements subprocess can retain the esp_hid
    # library dependency without propagating its public include directory to
    # sources added through this component's generated source list.
    list(APPEND extra_includes "${IDF_PATH}/components/esp_hid/include")
elseif(NOT idf_target STREQUAL "esp32s2" AND NOT idf_target STREQUAL "esp32p4")
    # IDF v6.0.2 quirk: the component requirements graph is built by a
    # sub-process that re-runs this CMakeLists WITHOUT sdkconfig values, so
    # config-gated REQUIRES appends (CONFIG_HAS_BADBLE, CONFIG_HAS_BADUSB,
    # CONFIG_HAS_CAMERA, ...) never reach the link graph. Gate on idf_target
    # instead - it IS visible there. Every non-S2 target in this project has
    # Bluetooth (ESP32/C3/C5/C6/S3), so esp_hid is always linked. It compiles
    # to an effectively empty library where BadBLE is disabled by Kconfig.
    list(APPEND required_components esp_hid)
endif()

if(CONFIG_HAS_BADUSB OR CONFIG_HAS_BADBLE)
    list(APPEND sources "${CMAKE_SOURCE_DIR}/main/managers/hid_script_parser.c")
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

set(embed_files_list "core/ouis.bin")

# Banshee C5 self-OTA disabled for v2.1.2 - was bad UX and flash overflow (see partitions_banshee_c5_factory.csv).
# Previous updater embed block disabled:
# if(CONFIG_BUILD_CONFIG_TEMPLATE STREQUAL "somethingsomething")
if(FALSE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(banshee_c5_updater_build_dir "${CMAKE_SOURCE_DIR}/ota_updater/build_banshee_c5_updater_auto")
    set(banshee_c5_updater_sdkconfig_defaults "${CMAKE_SOURCE_DIR}/ota_updater/sdkconfig.defaults")
    file(REMOVE_RECURSE "${banshee_c5_updater_build_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "IDF_TARGET=esp32c5"
                "${Python3_EXECUTABLE}" "$ENV{IDF_PATH}/tools/idf.py"
                -C "${CMAKE_SOURCE_DIR}/ota_updater"
                -B "${banshee_c5_updater_build_dir}"
                -DSDKCONFIG=${banshee_c5_updater_build_dir}/sdkconfig
                -DSDKCONFIG_DEFAULTS=${banshee_c5_updater_sdkconfig_defaults}
                build
        RESULT_VARIABLE banshee_c5_updater_build_result
        OUTPUT_VARIABLE banshee_c5_updater_build_output
        ERROR_VARIABLE banshee_c5_updater_build_error
    )
    if(NOT banshee_c5_updater_build_result EQUAL 0)
        message(FATAL_ERROR "Failed to build Banshee C5 updater app:\n${banshee_c5_updater_build_output}\n${banshee_c5_updater_build_error}")
    endif()

    set(banshee_c5_updater_built_bin "${banshee_c5_updater_build_dir}/Ghost_ESP_Banshee_C5_Updater.bin")
    if(NOT EXISTS "${banshee_c5_updater_built_bin}")
        message(FATAL_ERROR "Banshee C5 updater build did not produce ${banshee_c5_updater_built_bin}")
    endif()
    set(banshee_c5_updater_bin "${CMAKE_SOURCE_DIR}/ota_updater/banshee_c5_updater.bin")
    configure_file("${banshee_c5_updater_built_bin}" "${banshee_c5_updater_bin}" COPYONLY)
    list(APPEND embed_files_list "${banshee_c5_updater_bin}")
endif()

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

if(CONFIG_HAS_BADBLE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_gatts_stop")
    # On ESP32-P4, esp_hid is placed before main in IDF's static link line.
    # Pull in its NimBLE HID implementation explicitly so references emitted
    # by badble_manager.c are not missed by archive ordering.
    idf_component_get_property(esp_hid_lib esp_hid COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
                          "-Wl,--whole-archive" ${esp_hid_lib} "-Wl,--no-whole-archive")
endif()

# add compile definition for esp32s2 target to maintain legacy guards (needs COMPONENT_LIB)
if(idf_target STREQUAL "esp32s2")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_IDF_TARGET_ESP32S2)
endif()

if(idf_target STREQUAL "esp32p4")
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-error=stringop-truncation
        -Wno-error=format-truncation)
    set(crowpanel_c6_firmware "${CMAKE_SOURCE_DIR}/firmware/crowpanel_p4/network_adapter.bin")
    if(NOT EXISTS "${crowpanel_c6_firmware}")
        message(FATAL_ERROR "Missing bundled CrowPanel C6 firmware: ${crowpanel_c6_firmware}")
    endif()
    esptool_py_flash_to_partition(flash slave_fw "${crowpanel_c6_firmware}")
    # The slave image is an ESP32-C6 application stored as P4 partition data.
    # esptool otherwise rejects it while validating the P4 flash transaction.
    # Update the esptool component property as well as the target property:
    # the final flash target is regenerated later from FLASH_SUB_ARGS.
    idf_component_get_property(crowpanel_flash_args esptool_py FLASH_SUB_ARGS)
    list(APPEND crowpanel_flash_args "--force")
    idf_component_set_property(esptool_py FLASH_SUB_ARGS "${crowpanel_flash_args}")
    set_property(TARGET flash PROPERTY SUB_ARGS "${crowpanel_flash_args}")
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-incompatible-pointer-types -Wno-unused-local-typedefs)

# Disabled for v2.1.2 - Banshee C5 OTA removed
# if(CONFIG_BUILD_CONFIG_TEMPLATE STREQUAL "somethingsomething")
#     target_compile_definitions(${COMPONENT_LIB} PRIVATE GHOSTESP_BANSHEE_C5_UPDATER_EMBEDDED=1)
# endif()

idf_build_get_property(idf_path IDF_PATH)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${idf_path}/components/wpa_supplicant/src"
    "${idf_path}/components/wpa_supplicant/esp_supplicant/src"
)
