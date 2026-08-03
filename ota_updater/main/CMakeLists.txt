idf_component_register(
    SRCS "updater_main.c"
    REQUIRES app_update esp_http_client esp-tls esp_wifi nvs_flash mbedtls
)
