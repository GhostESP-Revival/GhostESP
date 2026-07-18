set(QR_ENCODER "${CMAKE_CURRENT_LIST_DIR}/../../../../components/M5GFX/src/lgfx/utility/lgfx_qrcode.c")

idf_component_register(
    SRCS "qr_generator.c" "${QR_ENCODER}"
    INCLUDE_DIRS "../../../sdk" "../../../../components/M5GFX/src/lgfx/utility"
)
