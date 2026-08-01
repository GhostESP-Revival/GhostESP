 // command.c

#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/commands.h"
#include "core/serial_manager.h"
#include "core/utils.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "managers/ap_manager.h"
#include "managers/ota_manager.h"
#include "sdkconfig.h"
#include "vendor/drivers/pcf8563.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#include "managers/ble_bridge_manager.h"
#include "attacks/ble/ble_spam.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/flipper_scan.h"
#include "host/ble_gap.h"
#endif
#include "managers/dial_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/error_popup.h"
#include "managers/settings_sd_backup.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/wpa3_compliance.h"
#include "managers/sd_card_manager.h"
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_driver.h"
#include "managers/microphone/mic_visualizer.h"
#endif
#include "vendor/pcap.h"
#include "vendor/printer.h"
#ifdef CONFIG_HAS_CAMERA
#include "managers/motion_detector_manager.h"
#include "managers/camera_stream_manager.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
#include "managers/zigbee_manager.h"
#endif
#ifdef CONFIG_HAS_TLV320DAC_I2S
#include "managers/audio_receiver_manager.h"
#endif
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/audio_stream_manager.h"
#endif
#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_comm_handler.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "managers/ethernet/eth_utils.h"
#include "managers/ethernet/eth_http.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

// Forward declaration - esp_netif_get_netif_impl is not in public API but exists internally
void* esp_netif_get_netif_impl(esp_netif_t *esp_netif);
#endif
#include <esp_timer.h>
#include <managers/gps_manager.h>
#include <managers/views/terminal_screen.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <vendor/dial_client.h>
#include "esp_wifi.h"
#include "core/glog.h"
#include "core/dns_server.h"

extern dns_server_handle_t dns_handle;
#include <time.h>
#include <dirent.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "core/ghostesp_version.h"
#include "core/chip_info.h"
#include "core/memory_debug.h"
#include <stddef.h>
#include <ctype.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <dirent.h>
#include "managers/infrared_manager.h"
#include "core/universal_ir.h"
#include "core/screen_mirror.h"
#include "managers/display_manager.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include "esp_partition.h"
#include "managers/aerial_detector_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/wigle_manager.h"
#include "managers/config_manager.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/plugin_manager.h"
#include "managers/plugin_loader.h"
#ifdef CONFIG_WITH_SCREEN
#include "managers/views/plugin_runner_view.h"
#endif


#include "attacks/wifi/dhcp_starvation.h"

static Command *command_list_head = NULL;
static Command *command_pool = NULL;
static Command *command_free_list = NULL;

#define COMMAND_REGISTRY_MAX 192
TaskHandle_t VisualizerHandle = NULL;
TaskHandle_t gps_info_task_handle = NULL;

// Storage for GPS info task stack and TCB to enable proper cleanup
StackType_t* gps_task_stack = NULL;
StaticTask_t* gps_task_tcb = NULL;

void command_init() {
    free(command_pool);
    command_pool = NULL;
    command_list_head = NULL;
    command_free_list = NULL;

#if defined(CONFIG_SPIRAM)
    command_pool = heap_caps_calloc(COMMAND_REGISTRY_MAX, sizeof(*command_pool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!command_pool) {
        command_pool = calloc(COMMAND_REGISTRY_MAX, sizeof(*command_pool));
    }
    if (!command_pool) {
        glog("Failed to allocate command registry (%u entries)\n", (unsigned)COMMAND_REGISTRY_MAX);
        return;
    }

    for (int i = 0; i < COMMAND_REGISTRY_MAX; i++) {
        command_pool[i].next = command_free_list;
        command_free_list = &command_pool[i];
    }
}

void register_command(const char *name, CommandFunction function) {
    // Check if the command already exists
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Command already registered
            return;
        }
        current = current->next;
    }

    if (!command_pool) {
        command_init();
    }

    Command *new_command = command_free_list;
    if (new_command == NULL) {
        glog("Failed to register command '%s': command registry full\n", name);
        return;
    }
    command_free_list = new_command->next;
    new_command->name = name;
    new_command->function = function;
    new_command->next = command_list_head;
    command_list_head = new_command;
}

void unregister_command(const char *name) {
    Command *current = command_list_head;
    Command *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Found the command to remove
            if (previous == NULL) {
                command_list_head = current->next;
            } else {
                previous->next = current->next;
            }
            current->name = NULL;
            current->function = NULL;
            current->next = command_free_list;
            command_free_list = current;
            return;
        }
        previous = current;
        current = current->next;
    }
}

CommandFunction find_command(const char *name) {
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcasecmp(current->name, name) == 0) {
            return current->function;
        }
        current = current->next;
    }
    return NULL;
}

const char *command_name_at(size_t index) {
    Command *current = command_list_head;
    while (current != NULL && index > 0) {
        current = current->next;
        index--;
    }
    return current ? current->name : NULL;
}







void register_commands() {
    command_init();
    register_command("echo", handle_echo_cmd);
    register_command("ifconfig", handle_ifconfig_cmd);
    register_command("ping", handle_ping_cmd);
    register_command("version", handle_version_cmd);
    register_command("uuid", handle_uuid_cmd);
    register_command("macaddr", handle_macaddr_cmd);
    register_command("uptime", handle_uptime_cmd);
    register_command("date", handle_time_cmd);
    register_command("whoami", handle_whoami_cmd);
    register_command("status", handle_status_cmd);
    register_command("clear", handle_clear_cmd);
    register_command("hostname", handle_hostname_cmd);
    register_command("color", handle_color_cmd);
    register_command("cli_color", handle_color_cmd);
    register_command("banner", handle_banner_cmd);
    register_command("alias", handle_alias_cmd);
    register_command("unalias", handle_unalias_cmd);
    register_command("history", handle_history_cmd);
    register_command("didyoumean", handle_didyoumean_cmd);
    register_command("ps", handle_ps_cmd);
    register_command("top", handle_ps_cmd);
    register_command("df", handle_df_cmd);
    register_command("tail", handle_tail_cmd);
    register_command("grep", handle_grep_cmd);
    register_command("source", handle_source_cmd);
    register_command("tee", handle_tee_cmd);
    register_command("env", handle_env_cmd);
    register_command("export", handle_export_cmd);
    register_command("watch", handle_watch_cmd);
    register_command("help", handle_help);
    register_command("mem", handle_mem_cmd);
#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)
    register_command("nfc", handle_nfc_cmd);
    register_command("nfctest", handle_nfctest_cmd);
#endif
    register_command("scanap", cmd_wifi_scan_start);
    register_command("scansta", handle_sta_scan);
    register_command("scanlocal", handle_ip_lookup);
    register_command("stopscan", cmd_wifi_scan_stop);
    register_command("attack", handle_attack_cmd);
    register_command("list", handle_list);
    register_command("beaconspam", handle_beaconspam);
    register_command("beaconadd", handle_beaconadd);
    register_command("beaconremove", handle_beaconremove);
    register_command("beaconclear", handle_beaconclear);
    register_command("beaconshow", handle_beaconshow);
    register_command("beaconspamlist", handle_beaconspamlist);
    register_command("stopspam", handle_stop_spam);
    register_command("stopdeauth", handle_stop_deauth);
    register_command("select", handle_select_cmd);
    register_command("capture", handle_capture_scan);
    register_command("startportal", handle_start_portal);
    register_command("disconnect", handle_wifi_disconnect);
    register_command("wifistatus", handle_wifi_status);
    register_command("autoreconnect", handle_wifi_autoreconnect_cmd);
    register_command("stopportal", stop_portal);
    register_command("sinkhole", handle_sinkhole_cmd);
    register_command("connect", handle_wifi_connection);
    register_command("dialconnect", handle_dial_command);
    register_command("powerprinter", handle_printer_command);
    register_command("tplinktest", handle_tp_link_test);
    register_command("stop", handle_stop_flipper);
    register_command("reboot", handle_reboot);
    register_command("startwd", handle_startwd);
    register_command("wdstream", handle_wdstream_cmd);
    register_command("gpsinfo", handle_gps_info);
    register_command("gpspin", handle_gps_pin);
    register_command("gpsbaud", handle_gps_baud);
    register_command("scanports", handle_scan_ports);
    register_command("scanarp", handle_scan_arp);
    register_command("scanssh", handle_scan_ssh);
    register_command("netbiosscan", handle_netbios_scan);
    register_command("httpbannerscan", handle_http_banner_scan);
    register_command("snmpprobe", handle_snmp_probe);
    register_command("enumscan", handle_enum_scan);
    register_command("congestion", handle_congestion_cmd);
    register_command("listenprobes", handle_listen_probes_cmd);
    register_command("settings", handle_settings_cmd);
    register_command("listportals", handle_listportals);
    register_command("evilportal", handle_evilportal);
    register_command("commdiscovery", handle_comm_discovery);
    register_command("commconnect", handle_comm_connect);
    register_command("commsend", handle_comm_send);
    register_command("commstatus", handle_comm_status);
    register_command("commdisconnect", handle_comm_disconnect);
    register_command("commsetpins", handle_comm_setpins);
#if GHOSTESP_OTA_SUPPORTED
    // Only registered on 8MB/16MB boards -- these handlers live in
    // peer_ota_manager.c, so registering them unconditionally would pull
    // that file's static buffers into every board's BSS for nothing.
    register_command("otarecv", handle_otarecv_cmd);
    register_command("otastatus", handle_otastatus_cmd);
    register_command("otaabort", handle_otaabort_cmd);
    register_command("otainfo", handle_otainfo_cmd);
#endif

#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blescan", handle_ble_scan_cmd);
    register_command("blebridge", ble_bridge_handle_command);
    register_command("blewardriving", handle_ble_wardriving);
    register_command("listairtags", handle_list_airtags_cmd);
    register_command("selectairtag", handle_select_airtag);
    register_command("spoofairtag", handle_spoof_airtag);
    register_command("stopspoof", handle_stop_spoof);
    register_command("chameleon", handle_chameleon_cmd);
#endif
#ifdef DEBUG
    register_command("crash", handle_crash);
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    register_command("coredump", handle_coredump_cmd);
#endif
    register_command("pineap", handle_pineap_detection);
    register_command("wpa3check", handle_wpa3_compliance);
    register_command("apcred", handle_apcred);
    register_command("apenable", handle_ap_enable_cmd);
    register_command("chipinfo", handle_chip_info_cmd);
    register_command("rgbmode", handle_rgb_mode);
    register_command("setrgbpins", handle_setrgb);
    register_command("setrgbcount", handle_setrgbcount);
    register_command("sd_config", handle_sd_config);
    register_command("sd_pins_mmc", handle_sd_pins_mmc);
    register_command("sd_pins_spi", handle_sd_pins_spi);
    register_command("sd_save_config", handle_sd_save_config);
    register_command("sd", handle_sd_cmd);
    register_command("scanall", handle_scanall);
    register_command("sweep", handle_sweep_cmd);
    register_command("timezone", handle_timezone_cmd);
    register_command("settime", handle_settime_cmd);
    register_command("time", handle_time_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("listflippers", handle_list_flippers_cmd);
    register_command("selectflipper", handle_select_flipper_cmd);
    register_command("listgatt", handle_list_gatt_cmd);
    register_command("selectgatt", handle_select_gatt_cmd);
    register_command("enumgatt", handle_enum_gatt_cmd);
    register_command("trackgatt", handle_track_gatt_cmd);
    register_command("listadv", handle_list_advertisers_cmd);
#endif
    register_command("trackap", handle_track_ap_cmd);
    register_command("tracksta", handle_track_sta_cmd);
    #ifdef CONFIG_WITH_STATUS_DISPLAY
    register_command("statusidle", handle_status_idle_cmd);
    #endif
    register_command("dhcpstarve", handle_dhcpstarve_cmd);
    register_command("saeflood", handle_sae_flood_cmd);
    register_command("stopsaeflood", handle_stop_sae_flood_cmd);
    register_command("saefloodhelp", handle_sae_flood_help_cmd);
#if CONFIG_IDF_TARGET_ESP32C5
    register_command("setcountry", handle_setcountry);
#endif
    register_command("webauth", handle_web_auth_cmd);
    register_command("webuiap", handle_webuiap_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blespam", handle_ble_spam_cmd);
#endif
    register_command("setrgbmode", handle_set_rgb_mode_cmd);
    register_command("karma", handle_karma_cmd);
    register_command("setneopixelbrightness", handle_set_neopixel_brightness_cmd);
    register_command("getneopixelbrightness", handle_get_neopixel_brightness_cmd);
#ifdef CONFIG_HAS_INFRARED
    register_command("ir", handle_ir_cmd);
#endif
    register_command("nrf24", handle_nrf24_cmd);
    register_command("audio", handle_audio_cmd);
    register_command("badusb", handle_badusb_cmd);
#if CONFIG_ENABLE_GHOSTSCRIPT
    register_command("script", handle_script_cmd);
#endif
#ifdef CONFIG_WITH_ETHERNET
    register_command("ethup", handle_eth_up_cmd);
    register_command("ethdown", handle_eth_down_cmd);
    register_command("ethinfo", handle_eth_info_cmd);
    register_command("ethfp", handle_eth_fingerprint_cmd);
    register_command("etharp", handle_eth_arp_cmd);
    register_command("ethports", handle_eth_ports_cmd);
    register_command("ethping", handle_eth_ping_cmd);
    register_command("ethdns", handle_eth_dns_cmd);
    register_command("ethtrace", handle_eth_trace_cmd);
    register_command("ethstats", handle_eth_stats_cmd);
    register_command("ethconfig", handle_eth_config_cmd);
    register_command("ethmac", handle_eth_mac_cmd);
    register_command("ethserv", handle_eth_serv_cmd);
    register_command("ethntp", handle_eth_ntp_cmd);
    register_command("ethhttp", handle_eth_http_cmd);
    register_command("ethpoison", handle_eth_poison_cmd);
#endif
    register_command("mirror", handle_mirror_cmd);
    register_command("rave", handle_rave_cmd);
    register_command("raveport", handle_raveport_cmd);
    register_command("input", handle_input_cmd);
    register_command("iobtn", handle_iobtn_cmd);
    register_command("identify", handle_identify_cmd);
#if CONFIG_IDF_TARGET_ESP32S3
    register_command("usbkbd", handle_usb_kbd_cmd);
#endif
    register_command("aerialscan", handle_aerial_scan_cmd);
    register_command("aeriallist", handle_aerial_list_cmd);
    register_command("aerialtrack", handle_aerial_track_cmd);
    register_command("aerialstop", handle_aerial_stop_cmd);
    register_command("aerialspoof", handle_aerial_spoof_cmd);
    register_command("aerialspoofstop", handle_aerial_spoof_stop_cmd);
    register_command("flockscan", handle_flock_scan_cmd);
    register_command("flocklist", handle_flock_list_cmd);
    register_command("flockstop", handle_flock_stop_cmd);
    register_command("wigle", handle_wigle_cmd);
#ifdef CONFIG_HAS_MIC
    register_command("mic_cal", handle_mic_cal_cmd);
#endif
#ifdef CONFIG_HAS_CAMERA
    register_command("motion", handle_motion_cmd);
    register_command("camerastream", handle_camerastream_cmd);
#endif
    register_command("loadconfig", handle_loadconfig_cmd);
    register_command("apps", handle_apps_cmd);
    register_command("subghz", handle_subghz_cmd);

    cmd_comm_register_callback();

    glog("Registered Commands\n");
}


