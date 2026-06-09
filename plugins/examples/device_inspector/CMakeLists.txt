cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/espressif__elf_loader")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ghostesp_device_inspector)

include(elf_loader)
project_so(ghostesp_device_inspector)
