# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Add the wrap flag for esp_wifi_init
add_compile_options(-Wl,--wrap=esp_wifi_init)

# Read version from header file before project() call
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/main/esp_hosted_coprocessor_fw_ver.h" HEADER_CONTENTS)
set(PROJECT_VERSION_REGEX "#define[ \t]+PROJECT_VERSION_MAJOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_MINOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_PATCH_1[ \t]+([0-9]+)")
string(REGEX MATCH "${PROJECT_VERSION_REGEX}" PROJECT_VERSION_MATCH "${HEADER_CONTENTS}")

if(PROJECT_VERSION_MATCH)
    set(PROJECT_VERSION_MAJOR_1 "${CMAKE_MATCH_1}")
    set(PROJECT_VERSION_MINOR_1 "${CMAKE_MATCH_2}")
    set(PROJECT_VERSION_PATCH_1 "${CMAKE_MATCH_3}")
    # Set PROJECT_VER before project() call to override git versioning
    set(PROJECT_VER "${PROJECT_VERSION_MAJOR_1}.${PROJECT_VERSION_MINOR_1}.${PROJECT_VERSION_PATCH_1}")
    message(STATUS "Setting PROJECT_VER to: ${PROJECT_VER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXCLUDE_COMPONENTS lwip esp_netif)

set(COMPONENTS esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm openthread)

#list(APPEND COMPONENTS esp_gdbstub) # in case gdbstub needed
project(network_adapter)
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)

if(EXISTS "${PROJECT_DIR}/../common")
    set(common_dir "${PROJECT_DIR}/../common")
    message(STATUS "common: ${common_dir}")
elseif(EXISTS "${PROJECT_DIR}/main/common")
    set(common_dir "${PROJECT_DIR}/main/common")
    message(STATUS "common: ${common_dir}")
else()
    message(FATAL_ERROR "Could not find 'common' directory in either '${PROJECT_DIR}/../common' or '${PROJECT_DIR}/main/common'")
endif()


if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    if(EXISTS "${common_dir}/esp_hosted_lwip_src_port_hook.h")
        idf_component_get_property(lwip lwip COMPONENT_LIB)
        if(TARGET ${lwip})
        	# Use generator expressions to only apply to non-INTERFACE targets
        	get_target_property(lwip_type ${lwip} TYPE)
        	if(NOT lwip_type STREQUAL "INTERFACE_LIBRARY")
        		#target_include_directories(${lwip} PRIVATE "${common_dir}")
        		#target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"${common_dir}/esp_hosted_lwip_src_port_hook.h\"")
                # Some IDF release/v5.4 commits fail to attach hook file for udp.c, so mandate attach for every file in lwip build
        			message(STATUS "********** Configuring LWIP for network split port configs **********")
                    target_compile_options(${lwip} PRIVATE "-include" "${common_dir}/esp_hosted_lwip_src_port_hook.h")
        	endif()
        endif()
    endif()
endif()

### Display FW Version (already read before project() call)
message(*************************************************************************************)
message("                    Building ESP-Hosted-MCU FW :: ${PROJECT_VER} ")
message(*************************************************************************************)
