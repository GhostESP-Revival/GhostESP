if(CONFIG_ESP_HOSTED_ENABLED)
	message(STATUS "Using Hosted Wi-Fi")
	set(FG_root_dir ".")
	set(host_dir "${FG_root_dir}/host")

	set(srcs
		"${host_dir}/api/src/esp_wifi_weak.c"
		"${host_dir}/api/src/esp_hosted_api.c"
		"${host_dir}/api/src/esp_hosted_transport_config.c"
		"${host_dir}/api/src/esp_hosted_ota_api.c"
		"${host_dir}/drivers/transport/transport_drv.c"
		"${host_dir}/drivers/transport/transport_util.c"
		"${host_dir}/drivers/serial/serial_ll_if.c"
		"${host_dir}/utils/stats.c"
		"${host_dir}/drivers/serial/serial_drv.c")

	# only these directories are public. Others are private
	set(pub_include
		"${host_dir}"
		"${host_dir}/api/include")

	set(priv_include
		"${host_dir}/drivers/transport"
		"${host_dir}/drivers/transport/spi"
		"${host_dir}/drivers/transport/sdio"
		"${host_dir}/drivers/serial"
		"${host_dir}/utils"
		"${host_dir}/api/priv")

	# rpc files - wrap -> slaveif -> core
	set(rpc_dir "${host_dir}/drivers/rpc")
	set(rpc_core_dir "${rpc_dir}/core")
	set(rpc_slaveif_dir "${rpc_dir}/slaveif")
	set(rpc_wrap_dir "${rpc_dir}/wrap")

	list(APPEND srcs
		"${rpc_core_dir}/rpc_core.c"
		"${rpc_core_dir}/rpc_req.c"
		"${rpc_core_dir}/rpc_rsp.c"
		"${rpc_core_dir}/rpc_evt.c"
		"${rpc_core_dir}/rpc_utils.c"
		"${rpc_slaveif_dir}/rpc_slave_if.c"
		"${rpc_wrap_dir}/rpc_wrap.c")

	list(APPEND priv_include
		"${rpc_core_dir}"
		"${rpc_slaveif_dir}"
		"${rpc_wrap_dir}")

	# virtual serial
	set(virt_serial_dir "${host_dir}/drivers/virtual_serial_if")
	list(APPEND srcs  "${virt_serial_dir}/serial_if.c")
	list(APPEND priv_include "${virt_serial_dir}")

	# slave and host common files
	set(common_dir "${FG_root_dir}/common")

	list(APPEND srcs
		"${common_dir}/protobuf-c/protobuf-c/protobuf-c.c"
		"${common_dir}/proto/esp_hosted_rpc.pb-c.c" )

	list(APPEND priv_include
		"${common_dir}"
		"${common_dir}/log"
		"${common_dir}/rpc"
		"${common_dir}/transport"
		"${common_dir}/protobuf-c"
		"${common_dir}/proto" )

	# mempool
	if (CONFIG_ESP_HOSTED_USE_MEMPOOL)
		list(APPEND srcs "${common_dir}/mempool/mempool_ll.c"
			"${common_dir}/mempool/mempool.c")
	endif()
	list(APPEND priv_include "${common_dir}/mempool/include" )

	# cli
	list(APPEND srcs "${common_dir}/utils/esp_hosted_cli.c")
	list(APPEND priv_include "${common_dir}/utils")

	# bt (NimBLE)
	### TODO config for HCI over UART
	list(APPEND priv_include "${host_dir}/drivers/bt")

	if(CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI OR CONFIG_ESP_HOSTED_BLUEDROID_HCI_VHCI)
		list(APPEND srcs "${host_dir}/drivers/bt/vhci_drv.c")
	else()
		list(APPEND srcs "${host_dir}/drivers/bt/hci_stub_drv.c")
	endif()

	# power save
	list(APPEND priv_include "${host_dir}/drivers/power_save")
	list(APPEND srcs "${host_dir}/drivers/power_save/power_save_drv.c")

	# transport files
	if(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/drivers/transport/sdio/sdio_drv.c")
	elseif(CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/drivers/transport/spi_hd/spi_hd_drv.c")
	elseif(CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/drivers/transport/spi/spi_drv.c")
	elseif(CONFIG_ESP_HOSTED_UART_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/drivers/transport/uart/uart_drv.c")
	endif()

	# port files
	list(APPEND priv_include "${host_dir}/port/esp/freertos/include")

	list(APPEND srcs
		"${host_dir}/port/esp/freertos/src/port_esp_hosted_host_init.c"
		"${host_dir}/port/esp/freertos/src/port_esp_hosted_host_os.c"
		#"${host_dir}/port/esp/freertos/src/port_esp_hosted_host_ota.c"
		"${host_dir}/port/esp/freertos/src/port_esp_hosted_host_transport_defaults.c"
	)

	if(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/port/esp/freertos/src/port_esp_hosted_host_sdio.c")
	elseif(CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/port/esp/freertos/src/port_esp_hosted_host_spi_hd.c")
	elseif(CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/port/esp/freertos/src/port_esp_hosted_host_spi.c")
	elseif(CONFIG_ESP_HOSTED_UART_HOST_INTERFACE)
		list(APPEND srcs "${host_dir}/port/esp/freertos/src/port_esp_hosted_host_uart.c")
	endif()

	# OpenThread utility functions
	if(CONFIG_ESP_HOSTED_HOST_OT_ENABLE)
		list(APPEND srcs "${host_dir}/port/esp/freertos/src/port_esp_hosted_host_ot_util.c")
	endif()
endif()

if(EXISTS "${IDF_PATH}/components/esp_driver_sdio")
    message(STATUS "Using new driver components (IDF >= 5.0)")
    list(APPEND driver_requires esp_driver_sdmmc esp_driver_spi esp_driver_uart esp_driver_gpio)
else()
    message(STATUS "Using legacy driver component (IDF <= 4.4)")
    list(APPEND driver_requires driver)
endif()

### to support ESP-Hosted events, make esp_event a public requirement
list(APPEND driver_requires esp_event)

idf_component_register(SRCS ${srcs}
	PRIV_REQUIRES soc esp_netif esp_timer driver esp_wifi bt esp_http_client console wpa_supplicant openthread
	REQUIRES ${driver_requires}
	INCLUDE_DIRS ${pub_include}
	PRIV_INCLUDE_DIRS ${priv_include})

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)

if(DEFINED ENV{ESP_HOSTED_CI_PEDANTIC})
	target_compile_options(${COMPONENT_LIB} PRIVATE
		-Werror -Werror=deprecated-declarations -Werror=unused-variable
		-Werror=unused-but-set-variable -Werror=unused-function
		$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
endif()

if(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
	idf_component_optional_requires(PRIVATE sdmmc)
endif()

# Required if using ESP-IDF without commit 6b6065de509b5de39e4655fd425bf96f43b365f7:
# fix(driver_spi): fix p4 cache auto writeback during spi(dma) rx
# if(CONFIG_IDF_TARGET_ESP32P4 AND (CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE OR CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE))
# 	# used to workaround SPI transfer issue
# 	idf_component_optional_requires(PRIVATE esp_mm)
# endif()


if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    idf_component_get_property(lwip lwip COMPONENT_LIB)
    if(TARGET ${lwip})
    	# Use generator expressions to only apply to non-INTERFACE targets
    	get_target_property(lwip_type ${lwip} TYPE)
    	if(NOT lwip_type STREQUAL "INTERFACE_LIBRARY")
			#target_include_directories(${lwip} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
			#target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_SOURCE_DIR}/common/esp_hosted_lwip_src_port_hook.h\"")

            # Some IDF release/v5.4 commits fail to attach hook file for udp.c, so mandate attach for every file in lwip build
        	message(STATUS "********** Configuring LWIP for network split port configs **********")
            target_compile_options(${lwip} PRIVATE "-include" "${CMAKE_CURRENT_SOURCE_DIR}/common/esp_hosted_lwip_src_port_hook.h")
    	endif()
    endif()
endif()
