idf_component_register(SRCS "main.c" "memory_debug.c"
	PRIV_REQUIRES esp_wifi nvs_flash
	INCLUDE_DIRS ".")
