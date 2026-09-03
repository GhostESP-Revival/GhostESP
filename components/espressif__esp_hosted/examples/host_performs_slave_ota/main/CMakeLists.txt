# Simple component selection based on Kconfig
set(COMPONENT_SRCS "main.c")
set(COMPONENT_REQUIRES esp_hosted console esp_timer)

idf_component_register(SRCS ${COMPONENT_SRCS}
                    INCLUDE_DIRS ".")
