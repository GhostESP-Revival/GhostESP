file(GLOB_RECURSE app_sources 
    "${CMAKE_SOURCE_DIR}/main/*.c"
    "${CMAKE_SOURCE_DIR}/main/core/esp_comm_manager.c"
)
# Explicitly include infrared view implementation
list(APPEND app_sources "${CMAKE_SOURCE_DIR}/main/managers/views/infrared_view.c")
# Exclude i80_display.c from the glob as it's conditionally included later
list(REMOVE_ITEM app_sources "${CMAKE_SOURCE_DIR}/main/vendor/i80_display.c")
file(GLOB_RECURSE image_sources "${CMAKE_SOURCE_DIR}/main/vendor/images/*.c")
file(GLOB_RECURSE axs15231b_sources "${CMAKE_SOURCE_DIR}/components/axs15231b/*.c")


list(APPEND app_sources ${axs15231b_sources} ${image_sources})


set(required_components bt nvs_flash driver esp_http_server mdns json esp_http_client mbedtls fatfs sdmmc wpa_supplicant lvgl lvgl_esp32_drivers freertos esp_lcd esp_adc)


set(sources ${app_sources})



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

idf_component_register(SRCS ${sources}
                       INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include"
                       REQUIRES ${required_components})

# add compile definition for esp32s2 target to maintain legacy guards (needs COMPONENT_LIB)
if("${IDF_TARGET}" STREQUAL "esp32s2")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_IDF_TARGET_ESP32S2)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-incompatible-pointer-types -Wno-unused-local-typedefs)

