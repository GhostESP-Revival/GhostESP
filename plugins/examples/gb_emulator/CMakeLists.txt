cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/espressif__elf_loader")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gb_emulator)

include(elf_loader)

# The elf_loader project_so() macro recursively globs every .c file under the
# "main" component, so all sources (app + vendored gnuboy) live under main/
# and ELF_LIBS stays empty. ELF_LIBS is reserved for sources outside main/.
set(ELF_DEFINITIONS ENABLE_LCD=1)
if(CONFIG_IDF_TARGET_ESP32C5)
    set(ELF_OPTIMIZATION -O3 -funroll-loops)
else()
    set(ELF_OPTIMIZATION -O2)
endif()

set(ELF_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/main"
    "${CMAKE_CURRENT_LIST_DIR}/main/gnuboy"
)
project_so(gb_emulator)
