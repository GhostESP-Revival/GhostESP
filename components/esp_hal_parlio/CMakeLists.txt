idf_build_get_property(target IDF_TARGET)

set(srcs)
set(includes "include")

if(CONFIG_SOC_PARLIO_SUPPORTED)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${target}/parlio_periph.c")
        list(APPEND srcs "${target}/parlio_periph.c")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/parlio_hal.c")
        list(APPEND srcs "parlio_hal.c")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${target}/include")
        list(APPEND includes "${target}/include")
    endif()
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes}
                       REQUIRES soc hal)
