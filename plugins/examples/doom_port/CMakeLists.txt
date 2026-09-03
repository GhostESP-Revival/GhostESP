cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/espressif__elf_loader")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doom_port)

include(elf_loader)

file(GLOB DOOM_SOURCES "${CMAKE_CURRENT_LIST_DIR}/doomgeneric/doomgeneric/*.c")
list(FILTER DOOM_SOURCES EXCLUDE REGEX ".*/doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)\\.c$")
list(FILTER DOOM_SOURCES EXCLUDE REGEX ".*/i_(allegro.*|sdlsound|sdlmusic)\\.c$")
list(FILTER DOOM_SOURCES EXCLUDE REGEX ".*/w_file_stdc\\.c$")

set(ELF_DEFINITIONS DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
if(CONFIG_IDF_TARGET_ESP32C5)
    list(APPEND ELF_DEFINITIONS DOOMGENERIC_RGB565=1 DOOM_PORT_FAST=1)
    set(ELF_OPTIMIZATION -O3 -funroll-loops)
endif()
set(ELF_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/doomgeneric/doomgeneric")
set(ELF_LIBS ${DOOM_SOURCES})
project_so(doom_port)
