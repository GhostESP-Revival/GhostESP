set(COMPONENT_SRC "src/audio_decoder_reg.c" "src/audio_encoder_reg.c" "src/simple_decoder_reg.c")
set(COMPONENT_INCLUDE "include" 
    "include/decoder"
    "include/decoder/impl"
    "include/encoder"
    "include/encoder/impl"
    "include/simple_dec"
)

idf_component_register(
    INCLUDE_DIRS ${COMPONENT_INCLUDE}
    SRCS ${COMPONENT_SRC}
)

get_filename_component(BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR} NAME)
add_prebuilt_library(esp_acodec "${CMAKE_CURRENT_SOURCE_DIR}/lib/${CONFIG_IDF_TARGET}/libesp_audio_codec.a"
                     PRIV_REQUIRES ${BASE_DIR})
add_prebuilt_library(esp_sdec "${CMAKE_CURRENT_SOURCE_DIR}/lib/${CONFIG_IDF_TARGET}/libesp_audio_simple_dec.a"
                     PRIV_REQUIRES ${BASE_DIR})
set(TARGET_LIB_NAME esp_sdec esp_acodec)

target_link_libraries(${COMPONENT_LIB}  PRIVATE "-L ${CMAKE_CURRENT_SOURCE_DIR}/lib/${CONFIG_IDF_TARGET}")
target_link_libraries(${COMPONENT_LIB}  PRIVATE ${TARGET_LIB_NAME})
