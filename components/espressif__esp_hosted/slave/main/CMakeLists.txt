if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/common")
	set(common_dir "${CMAKE_CURRENT_SOURCE_DIR}/common")
else()
	set(common_dir "../../common")
endif()

set(COMPONENT_SRCS
	"${common_dir}/proto/esp_hosted_rpc.pb-c.c"
	"${common_dir}/utils/esp_hosted_cli.c"
	"protocomm_pserial.c"
	"esp_hosted_coprocessor.c"
	"stats.c"
	"host_power_save.c"
	"slave_light_sleep.c"
	"nw_split_router.c"
	"slave_transport_gpio_pin_guard.c"
	"slave_gpio_expander.c"
	"slave_ext_coex.c"
	"slave_control.c"
	"slave_util.c"
	"../../../../firmware/crowpanel_p4/ghost_raw_radio_slave.c"
)

if(CONFIG_ESP_HOSTED_USE_MEMPOOL)
	list(APPEND COMPONENT_SRCS
		"${common_dir}/mempool/mempool_ll.c"
		"${common_dir}/mempool/mempool.c"
	)
endif()

if(CONFIG_SOC_BT_SUPPORTED)
	list(APPEND COMPONENT_SRCS "slave_bt.c")
endif()

if(CONFIG_ESP_HOSTED_CP_WIFI)
	list(APPEND COMPONENT_SRCS "slave_wifi_std.c")
endif()

if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
	list(APPEND COMPONENT_SRCS "slave_wifi_enterprise.c")
endif()

if (CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
	list(APPEND COMPONENT_SRCS "slave_network_split.c")
endif()

if (CONFIG_ESP_HOSTED_OT_RCP_ENABLED)
	list(APPEND COMPONENT_SRCS "slave_openthread.c")
endif()

set(COMPONENT_ADD_INCLUDEDIRS
	"."
	"${common_dir}"
	"${common_dir}/log"
	"${common_dir}/proto"
	"${common_dir}/rpc"
	"${common_dir}/transport"
	"${common_dir}/mempool/include"
	"../../../../firmware/crowpanel_p4"
	"../../../../include/managers"
)

if(CONFIG_SOC_BT_SUPPORTED)
# Select BT UART code based on IDF Target
if(CONFIG_IDF_TARGET_ESP32)
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32.c)
elseif(CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32S3)
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32c3_s3.c)
else()
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32xx.c)
endif()
endif()

if(CONFIG_ESP_SDIO_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS sdio_slave_api.c)
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS spi_slave_api.c)
elseif(CONFIG_ESP_SPI_HD_MODE)
	list(APPEND COMPONENT_SRCS spi_hd_slave_api.c)
else(CONFIG_ESP_UART_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS uart_slave_api.c)
endif()

if(CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER)
	list(APPEND COMPONENT_SRCS example_peer_data_transfer.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_LIGHT_SLEEP)
	list(APPEND COMPONENT_SRCS example_light_sleep.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT)
	list(APPEND COMPONENT_SRCS example_mqtt_client.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT)
	list(APPEND COMPONENT_SRCS example_http_client.c)
endif()

# cli
list(APPEND COMPONENT_ADD_INCLUDEDIRS "${common_dir}/utils")
list(APPEND COMPONENT_SRCS "${common_dir}/utils/esp_hosted_cli.c")

register_component()

# Add directory of protocomm_priv.h to include paths
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# Add linker options to wrap esp_wifi_init function
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")

if(DEFINED ENV{ESP_HOSTED_CI_PEDANTIC})
	target_compile_options(${COMPONENT_LIB} PRIVATE
		-Werror -Werror=deprecated-declarations -Werror=unused-variable
		-Werror=unused-but-set-variable -Werror=unused-function
		$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
endif()
