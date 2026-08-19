cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project({{APP_SYMBOL}})

include(elf_loader)
project_so({{APP_SYMBOL}})
