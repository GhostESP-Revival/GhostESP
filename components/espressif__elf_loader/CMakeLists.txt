
if(CONFIG_ELF_LOADER)
    set(srcs  "src/esp_elf_symbol.c"
              "src/esp_elf.c"
              "src/esp_elf_adapter.c")

    if(CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS)
        list(APPEND srcs "src/esp_all_symbol.c")
    endif()

    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        list(APPEND srcs "src/arch/esp_elf_xtensa.c")

        # ESP32-S2 need to set MMU to run ELF
        if(CONFIG_IDF_TARGET_ESP32S2 AND (CONFIG_ELF_LOADER_LOAD_PSRAM))
            list(APPEND srcs "src/soc/esp_elf_esp32s2.c")
        endif()
    elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
        list(APPEND srcs "src/arch/esp_elf_riscv.c")

        # ESP32-C5 flash-XIP: program app .text into a flash partition and map it
        # instruction-executable in place (breaks the internal-RAM exec ceiling).
        if(CONFIG_ELF_LOADER_C5_FLASH_XIP)
            list(APPEND srcs "src/soc/esp_elf_esp32c5.c")
        endif()
    endif()

    if(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT)
        list(APPEND srcs "src/dlso/dlfcn.c"
                         "src/dlso/dlmod.c")
    endif()

    set(include_dirs "include")
    set(ldfragments "linker.lf")
endif()

# esp_partition is required by the C5 flash-XIP backend (esp_elf_esp32c5.c).
# Listed unconditionally: component requirements are gathered in an early CMake
# expansion pass where CONFIG_* values are not reliably set, so a config-guarded
# PRIV_REQUIRES would be silently dropped. Always-requiring esp_partition is
# harmless for builds that don't compile the C5 backend.
set(priv_req spi_flash esp_mm esp_partition)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_REQUIRES ${priv_req}
                       LDFRAGMENTS ${ldfragments})

include(package_manager)
if(CONFIG_ELF_LOADER)
    cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
endif()
