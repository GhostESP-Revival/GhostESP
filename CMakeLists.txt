cmake_minimum_required(VERSION 3.16.0)

# Set IDF_TARGET from environment variable if defined
if(DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET $ENV{IDF_TARGET})
    message(STATUS "Setting IDF_TARGET to ${IDF_TARGET} from environment variable")
endif()

# Partition table settings
add_link_options("-Wl,-z,muldefs")
add_compile_options(-Wno-error=unused-label)

if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

# ESP-IDF 6.1 can discard an explicit Wi-Fi cache TX buffer count when it
# differs from the Kconfig default, leaving the generated macro empty. Align
# the defaults before project.cmake evaluates Kconfig so local idf.py builds
# behave like the GitHub matrix build.
if(DEFINED PYTHON)
    set(GHOSTESP_PYTHON "${PYTHON}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(GHOSTESP_PYTHON "${Python3_EXECUTABLE}")
endif()
if(IS_ABSOLUTE "${SDKCONFIG_DEFAULTS}")
    set(GHOSTESP_SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS}")
else()
    set(GHOSTESP_SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/${SDKCONFIG_DEFAULTS}")
endif()
execute_process(
    COMMAND "${GHOSTESP_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/scripts/prepare_idf_wifi_kconfig.py"
            --idf-path "$ENV{IDF_PATH}" --config "${GHOSTESP_SDKCONFIG_DEFAULTS}"
    RESULT_VARIABLE GHOSTESP_KCONFIG_RESULT
    OUTPUT_VARIABLE GHOSTESP_KCONFIG_OUTPUT
    ERROR_VARIABLE GHOSTESP_KCONFIG_ERROR
)
if(NOT GHOSTESP_KCONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to align ESP-IDF Wi-Fi Kconfig defaults:\n${GHOSTESP_KCONFIG_ERROR}")
elseif(GHOSTESP_KCONFIG_OUTPUT)
    string(STRIP "${GHOSTESP_KCONFIG_OUTPUT}" GHOSTESP_KCONFIG_OUTPUT)
    message(STATUS "GhostESP: ${GHOSTESP_KCONFIG_OUTPUT}")
endif()

add_compile_definitions(HOLD_LIMIT=1000) # For joystick hold duration
add_compile_definitions(LED_ORDER=0)     # 2 for RGB, 0 for GRB
add_compile_definitions(DNS_SERVER_MAX_ITEMS=1)
add_compile_definitions(MAX_WPS_NETWORKS=15)

# define esp32s2 target macro globally for legacy guards
if("${IDF_TARGET}" STREQUAL "esp32s2")
    add_compile_definitions(CONFIG_IDF_TARGET_ESP32S2)
endif()

# Get git commit hash and branch at build time
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_COMMIT_HASH)
        set(GIT_COMMIT_HASH "unknown")
    endif()
    
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_BRANCH)
        set(GIT_BRANCH "unknown")
    endif()
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
endif()

# Add git commit hash and branch as compile definitions
add_compile_definitions(GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")
add_compile_definitions(GIT_BRANCH="${GIT_BRANCH}")

# Monotonic build number used by the OTA manifest for version comparisons
# (esp_app_desc_t's git-describe-derived version isn't guaranteed monotonic).
# Prefer CI's auto-incrementing run number, since the build job's checkout is
# shallow and `git rev-list --count` would otherwise always report 1.
if(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(GHOSTESP_BUILD_NUMBER $ENV{GITHUB_RUN_NUMBER})
elseif(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GHOSTESP_BUILD_NUMBER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GHOSTESP_BUILD_NUMBER)
        set(GHOSTESP_BUILD_NUMBER "0")
    endif()
else()
    set(GHOSTESP_BUILD_NUMBER "0")
endif()
add_compile_definitions(GHOSTESP_BUILD_NUMBER=${GHOSTESP_BUILD_NUMBER})

# Include project settings
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Ghost_ESP_IDF)
include(cmake/crowpanel_rgb_factory.cmake)
