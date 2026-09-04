// cmd_system.c
// Reboot, crash, coredump, wardriver, AP enable, chip info, mirror, and apps commands.

#include "core/callbacks.h"
#include "core/chip_info.h"
#include "core/commands.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "core/screen_mirror.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/ap_manager.h"
#include "managers/gps_manager.h"
#include "managers/infrared_manager.h"
#include "managers/plugin_loader.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/nrf24_remote_manager.h"
#ifdef CONFIG_WITH_SCREEN
#include "managers/views/plugin_runner_view.h"
#endif
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/wpa3_compliance.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/pcap.h"
#include <esp_log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *log_level_name(esp_log_level_t level) {
    static const char *names[] = {"none", "error", "warn", "info", "debug", "verbose"};
    return (level <= ESP_LOG_VERBOSE) ? names[level] : "unknown";
}

void handle_log_level_cmd(int argc, char **argv) {
    if (argc == 1) {
        esp_log_level_t level = (esp_log_level_t)settings_get_log_level(&G_Settings);
        glog("Global log level: %s (%d)\n", log_level_name(level), level);
        return;
    }

    if (argc != 2) {
        glog("Usage: loglevel [none|error|warn|info|debug|verbose]\n");
        return;
    }

    static const char *names[] = {"none", "error", "warn", "info", "debug", "verbose"};
    esp_log_level_t level = ESP_LOG_NONE;
    bool found = false;
    for (int i = 0; i <= ESP_LOG_VERBOSE; i++) {
        if (strcasecmp(argv[1], names[i]) == 0) {
            level = (esp_log_level_t)i;
            found = true;
            break;
        }
    }
    if (!found) {
        glog("Unknown log level. Use none, error, warn, info, debug, or verbose.\n");
        return;
    }

    esp_log_level_set("*", level);
    settings_set_log_level(&G_Settings, level);
    settings_save(&G_Settings);
    glog("Global log level set to %s.\n", log_level_name(level));
}

void handle_fav_cmd(int argc, char **argv) {
    if (argc == 1) {
        uint8_t cnt = settings_get_favorites_count(&G_Settings);
        glog("Favorites (%u/%d):\n", cnt, FAVORITES_MAX);
        for (int i = 0; i < cnt; i++) {
            const char *n = settings_get_favorite(&G_Settings, i);
            glog("  %d: %s\n", i + 1, n ? n : "");
        }
        glog("Bypass PIN: %s\n", settings_get_favorites_bypass(&G_Settings) ? "on" : "off");
        glog("Usage: fav [list|add <name>|remove <name>|toggle <name>|clear|bypass <on|off|toggle>]\n");
        glog("  Menu: WiFi, BLE, GPS, Infrared, NFC, NRF24, SubGHz, BadUSB, GhostLink, Ethernet, Apps, Settings\n");
        glog("  Files: ir:/path/to/remote.ir  nfc:/path/to/tag.nfc  subghz:/path/to/file.sub  app:<plugin_id>\n");
        glog("  Ex: fav add WiFi   | fav add ir:/mnt/ghostesp/infrared/remotes/TV.ir\n");
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        uint8_t cnt = settings_get_favorites_count(&G_Settings);
        for (int i = 0; i < cnt; i++) glog("%d: %s\n", i+1, settings_get_favorite(&G_Settings, i));
        if (cnt==0) glog("(no favorites)\n");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        G_Settings.favorites_count = 0;
        memset(G_Settings.favorites, 0, sizeof(G_Settings.favorites));
        settings_persist_setting(SETTING_FAVORITES);
        glog("Favorites cleared.\n");
        return;
    }
    if (strcmp(argv[1], "bypass") == 0) {
        if (argc == 2) {
            glog("Bypass: %s\n", settings_get_favorites_bypass(&G_Settings) ? "on" : "off");
            return;
        }
        bool cur = settings_get_favorites_bypass(&G_Settings);
        bool nv = cur;
        if (strcmp(argv[2], "on") == 0) nv = true;
        else if (strcmp(argv[2], "off") == 0) nv = false;
        else if (strcmp(argv[2], "toggle") == 0) nv = !cur;
        else { glog("Usage: fav bypass [on|off|toggle]\n"); return; }
        settings_set_favorites_bypass(&G_Settings, nv);
        settings_persist_setting(SETTING_FAVORITES_BYPASS);
        glog("Bypass %s.\n", nv ? "enabled" : "disabled");
        return;
    }
    if (argc >= 3 && (strcmp(argv[1], "add")==0 || strcmp(argv[1], "remove")==0 || strcmp(argv[1], "toggle")==0)) {
        const char *name = argv[2];
        bool ok = false;
        if (strcmp(argv[1], "add")==0) ok = settings_add_favorite(&G_Settings, name);
        else if (strcmp(argv[1], "remove")==0) ok = settings_remove_favorite(&G_Settings, name);
        else ok = settings_toggle_favorite(&G_Settings, name);
        if (ok) {
            settings_persist_setting(SETTING_FAVORITES);
            glog("%s %s favorites.\n", name, strcmp(argv[1], "remove")==0 ? "removed from" : "toggled/added to");
        } else {
            glog("Failed (maybe full or not found). Count %u/%d\n", settings_get_favorites_count(&G_Settings), FAVORITES_MAX);
        }
        return;
    }
    glog("Usage: fav [list|add <name>|remove <name>|toggle <name>|clear|bypass <on|off|toggle>]\n");
}

void handle_reboot(int argc, char **argv) {
    glog("Rebooting system...\n");
    esp_restart();
}


void handle_startwd(int argc, char **argv) {
    bool stop_flag = false;
    bool helper_mode = false;
    char helper_channels_csv[192] = {0};
    bool helper_channels_set = false;
    uint16_t helper_hop_ms = 0;
    bool helper_hop_set = false;
    bool helper_weighted = false;
    bool helper_weighted_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
        } else if (strcmp(argv[i], "--helper") == 0) {
            helper_mode = true;
        } else if (strcmp(argv[i], "--channels") == 0) {
            if (i + 1 >= argc) {
                glog("startwd: --channels requires a value\n");
                return;
            }
            strncpy(helper_channels_csv, argv[++i], sizeof(helper_channels_csv) - 1);
            helper_channels_set = true;
        } else if (strncmp(argv[i], "--channels=", 11) == 0) {
            strncpy(helper_channels_csv, argv[i] + 11, sizeof(helper_channels_csv) - 1);
            helper_channels_set = true;
        } else if (strcmp(argv[i], "--hop") == 0) {
            if (i + 1 >= argc) {
                glog("startwd: --hop requires a value (ms)\n");
                return;
            }
            helper_hop_ms = (uint16_t)atoi(argv[++i]);
            helper_hop_set = true;
        } else if (strncmp(argv[i], "--hop=", 6) == 0) {
            helper_hop_ms = (uint16_t)atoi(argv[i] + 6);
            helper_hop_set = true;
        } else if (strcmp(argv[i], "--weighted") == 0) {
            helper_weighted = true;
            helper_weighted_set = true;
        }
    }

    if (stop_flag) {
        bool stopping_helper = wardriving_is_helper_mode();
        if (helper_mode && !stopping_helper) {
            glog("Wardriving helper stop ignored: helper is not active.\n");
            return;
        }
        if (!helper_mode && !esp_comm_manager_is_remote_command()) {
            if (esp_comm_manager_is_connected()) {
                bool peer_stop_ok = esp_comm_manager_send_command("startwd", "-s --helper");
                glog(peer_stop_ok
                         ? "Wardrive helper stop sent to peer.\n"
                         : "Wardrive helper stop could not be sent to peer.\n");
            }
            wardriving_set_peer_assist(false);
        }

        stop_wardriving();
        wifi_manager_stop_monitor_mode();
        if (!stopping_helper) {
            if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
                csv_flush_buffer_to_file();
            }
            csv_file_close();
            gps_manager_deinit(&g_gpsManager);
            glog("Wardriving stopped.\n");
            status_display_show_status("Wardrive Stop");
        } else {
            (void)wardriving_set_helper_channels_from_csv(NULL);
            glog("Wardriving helper stopped.\n");
            status_display_show_status("WD Helper Stop");
        }
    } else {
        if (helper_mode) {
            if (csv_file_is_open() && !wardriving_is_helper_mode()) {
                glog("Wardriving helper refused: a primary CSV session is active.\n");
                return;
            }
            if (wardriving_is_helper_mode()) {
                glog("Wardriving helper is already running.\n");
                return;
            }
            if (helper_channels_set) {
                if (!wardriving_set_helper_channels_from_csv(helper_channels_csv)) {
                    glog("Wardriving helper: invalid channel list, using default helper channels.\n");
                }
            } else {
                (void)wardriving_set_helper_channels_from_csv(NULL);
            }

            if (helper_hop_set) {
                wardriving_set_helper_hop_ms(helper_hop_ms);
            }
            if (helper_weighted_set) {
                wardriving_set_helper_weighted_5g(helper_weighted);
            }

            bool helper_initialized_gps = false;
            if (!g_gpsManager.isinitilized) {
                gps_manager_init(&g_gpsManager);
                helper_initialized_gps = g_gpsManager.isinitilized;
            }

            wifi_manager_start_monitor_mode(wardriving_scan_callback);
            if (!start_wardriving_helper()) {
                stop_wardriving();
                wifi_manager_stop_monitor_mode();
                if (helper_initialized_gps) gps_manager_deinit(&g_gpsManager);
                glog("Wardriving helper failed to start.\n");
                status_display_show_status("WD Helper Fail");
                return;
            }
            glog("Wardriving helper started.\n");
            status_display_show_status("WD Helper Start");
            return;
        }

        if (csv_file_is_open()) {
            glog("Wardriving is already running.\n");
            return;
        }

        bool prefer_peer_only = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        prefer_peer_only = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) &&
                           !esp_comm_manager_is_remote_command() &&
                           esp_comm_manager_is_connected();
#endif

        gps_manager_set_peer_gps_preferred(false);
        gps_manager_init(&g_gpsManager);
        esp_err_t err = csv_file_open("wardriving");
        if (err != ESP_OK) {
            glog("Failed to open CSV for wardriving\n");
            status_display_show_status("CSV Open Fail");
            if (!prefer_peer_only) {
                gps_manager_deinit(&g_gpsManager);
            }
            return;
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        if (!start_wardriving()) {
            wifi_manager_stop_monitor_mode();
            csv_file_close();
            gps_manager_deinit(&g_gpsManager);
            glog("Failed to start wardriving observation queue.\n");
            status_display_show_status("Wardrive Start Fail");
            return;
        }

        bool peer_helper_ok = false;
        if (!esp_comm_manager_is_remote_command()) {
            if (esp_comm_manager_is_connected()) {
                char helper_command[256];
                char helper_plan_csv[192] = {0};
                uint16_t hop_ms = settings_get_wd_hop_helper_ms(&G_Settings);
                bool weighted = settings_get_wd_weighted_5g(&G_Settings);
                if (wardriving_get_helper_channel_plan_csv(helper_plan_csv, sizeof(helper_plan_csv))) {
                    snprintf(helper_command, sizeof(helper_command),
                             "startwd --helper --channels %s --hop %u%s",
                             helper_plan_csv, (unsigned)hop_ms, weighted ? " --weighted" : "");
                } else {
                    snprintf(helper_command, sizeof(helper_command), "startwd --helper --hop %u%s",
                             (unsigned)hop_ms, weighted ? " --weighted" : "");
                }
                wardriving_expect_peer_assist(true);
                peer_helper_ok = esp_comm_manager_send_command_line(helper_command);
                if (!peer_helper_ok) wardriving_expect_peer_assist(false);
                glog(peer_helper_ok
                          ? "Wardrive helper start sent; waiting for ready status.\n"
                          : "Wardrive helper not started on peer; continuing local only.\n");
            } else {
                glog("Wardrive helper unavailable: no GhostLink peer connected.\n");
            }
        }
        if (!peer_helper_ok) wardriving_set_peer_assist(false);

        if (prefer_peer_only && !peer_helper_ok) {
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_clear_peer_fix();
            if (!g_gpsManager.isinitilized) {
                gps_manager_init(&g_gpsManager);
            }
            glog("Wardriving: peer helper unavailable, falling back to local GPS.\n");
        }

        glog("Wardriving started.\n");
        status_display_show_status("Wardrive Start");
    }
}

void handle_crash(int argc, char **argv) {
    glog("Triggering crash for coredump test...\n");
    (void)argc;
    (void)argv;
    /* Intentional null pointer write to trigger panic; coredump will be saved to flash. */
    int *ptr = NULL;
    *ptr = 42;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/* Read coredump partition and print summary or stream base64 for host decode. */
void handle_coredump_cmd(int argc, char **argv) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
    if (part == NULL) {
        glog("No coredump partition found. Check partition table.\n");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "erase") == 0) {
        /* Use partition erase on all targets (esp_core_dump_image_erase can fail on plain ESP32). */
        size_t esz = part->erase_size;
        size_t to_erase = (part->size / esz) * esz;
        if (to_erase == 0) {
            to_erase = esz;
        }
        esp_err_t err = esp_partition_erase_range(part, 0, to_erase);
        if (err == ESP_OK) {
            glog("Coredump partition erased.\n");
        } else {
            glog("Failed to erase coredump: %s\n", esp_err_to_name(err));
        }
        return;
    }

    const int do_dump = (argc > 1 && strcmp(argv[1], "dump") == 0);

    if (do_dump) {
        /* Stream partition as base64 so user can save and run: idf.py coredump-info -c <file> */
        glog("=== COREDUMP BASE64 START ===\n");
        glog("Save the lines below to a file (e.g. coredump.b64), then run:\n");
        glog("  idf.py coredump-info -c coredump.b64\n");
        glog("(Omit the start/end marker lines from the file.)\n");
        uint8_t buf[768]; /* multiple of 3 for base64 */
        char b64[1032];
        size_t offset = 0;
        while (offset < part->size) {
            size_t chunk = (part->size - offset) > sizeof(buf) ? sizeof(buf) : (part->size - offset);
            if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
                glog("\nRead error at offset %u\n", (unsigned)offset);
                break;
            }
            size_t written = 0;
            int ret = mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &written, buf, chunk);
            if (ret != 0) {
                glog("\nBase64 encode error\n");
                break;
            }
            b64[written] = '\0';
            /* glog caps each call at 511 bytes; emit 4-aligned pieces so no
             * base64 chars are truncated. Newlines between pieces are fine for
             * host-side base64 decoding. */
            size_t piece_len = 496; /* multiple of 4, < 511 */
            size_t pos = 0;
            while (pos < written) {
                size_t n = written - pos;
                if (n > piece_len) n = piece_len;
                glog("%.*s\n", (int)n, b64 + pos);
                pos += n;
            }
            offset += chunk;
        }
        glog("\n=== COREDUMP BASE64 END ===\n");
        return;
    }

    /* Summary: partition info and whether it contains valid coredump data.
     * ESP-IDF may write a small header (e.g. checksum) before the ELF, so scan
     * the first 128 bytes for ELF magic (0x7f 'E' 'L' 'F') instead of only offset 0. */
    uint8_t head[128];
    size_t head_len = part->size < sizeof(head) ? (size_t)part->size : sizeof(head);
    if (esp_partition_read(part, 0, head, head_len) != ESP_OK) {
        glog("Failed to read coredump partition.\n");
        return;
    }
    int elf_offset = -1;
    for (size_t i = 0; i + 4 <= head_len; i++) {
        if (head[i] == 0x7f && head[i + 1] == 'E' && head[i + 2] == 'L' && head[i + 3] == 'F') {
            elf_offset = (int)i;
            break;
        }
    }
    const int is_elf = (elf_offset >= 0);

    glog("Coredump partition: %s, size %u bytes\n", part->label, (unsigned)part->size);

    /* Use ESP-IDF API to parse and print panic reason from coredump in flash */
    {
        char panic_reason[256];
        esp_err_t err = esp_core_dump_get_panic_reason(panic_reason, sizeof(panic_reason));
        if (err == ESP_OK && panic_reason[0] != '\0') {
            glog("Panic reason: %s\n", panic_reason);
        } else if (err == ESP_ERR_NOT_FOUND) {
            /* Do not call esp_core_dump_get_summary() here: it uses too much stack and can
             * cause Stack protection fault in SerialTask (same task as CLI). */
            glog("Panic reason: (not available on device; run idf.py coredump-info on host)\n");
        } else {
            glog("Panic reason: (error %s)\n", esp_err_to_name(err));
        }
    }

    if (is_elf) {
        glog("Coredump data: present (ELF format");
        if (elf_offset > 0) {
            glog(", ELF at offset %d", elf_offset);
        }
        glog(").\n");
        glog("For full backtrace run on host: idf.py coredump-info\n");
    } else {
        /* Check for empty (all 0xff) or binary format */
        int empty = 1;
        for (size_t i = 0; i < head_len && empty; i++) {
            if (head[i] != 0xff) {
                empty = 0;
            }
        }
        if (empty != 0) {
            glog("Coredump data: partition empty (no crash recorded yet).\n");
        } else {
            glog("Coredump data: present (binary format).\n");
            glog("For full backtrace run on host: idf.py coredump-info\n");
        }
    }
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */




void handle_wpa3_compliance(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wpa3_compliance_check_selected();
    status_display_show_status("WPA3 Check");
}

void handle_pineap_detection(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        status_display_show_status("PineAP Stop");
        return;
    }
    // Open PCAP file for logging detections
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        glog("Warning: Failed to open PCAP file for logging\n");
        status_display_show_status("PCAP Warn");
    }

    // Start PineAP detection with channel hopping
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);

    glog("Monitoring for Pineapples\n");
    status_display_show_status("PineAP Watch");
}





// Forward declaration for the new print function
void handle_setcountry(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: setcountry <CC>\n");
        status_display_show_status("Country Usage");
        return;
    }

    char cc_upper[4];
    if (!str_copy_upper(cc_upper, sizeof(cc_upper), argv[1])) {
        glog("failed to set country: invalid country code\n");
        status_display_show_status("Country Fail");
        return;
    }

    esp_err_t err = esp_wifi_set_country_code(cc_upper, true);
    if (err == ESP_OK) {
        glog("country set to %s\n", cc_upper);
        status_display_show_status("Country Set");
    } else {
        glog("failed to set country: %s\n", esp_err_to_name(err));
        status_display_show_status("Country Fail");
    }
}


void handle_ap_enable_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: apenable <on|off>\n");
        glog("Example: apenable on\n");
        glog("         apenable off\n");
        status_display_show_status("APEnable Use");
        return;
    }
    
    bool enable = false;
    if (strcmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        glog("Invalid argument. Use 'on' or 'off'\n");
        status_display_show_status("APEnable Bad");
        return;
    }
    
    settings_set_ap_enabled(&G_Settings, enable);
    settings_persist_setting(SETTING_AP_ENABLED);
    
    glog("Access Point %s. Restart required to take effect.\n", enable ? "enabled" : "disabled");
    status_display_show_status(enable ? "AP Enabled" : "AP Disabled");
}

void handle_chip_info_cmd(int argc, char **argv) {
    vTaskDelay(pdMS_TO_TICKS(50));

    glog("[CHIPINFO_START]\n");
    glog("Chip Information:\n");

    // The shared helper produces device info rows followed by a contiguous
    // run of "Enabled Features" rows (each with value "Yes"). The CLI
    // prints the two sections with different formatting, so we split at the
    // first "Yes" row to keep the original output layout.
    enum { CHIP_INFO_MAX_ROWS = 64 };
    chip_info_line_t *rows = (chip_info_line_t *)calloc(CHIP_INFO_MAX_ROWS, sizeof(chip_info_line_t));
    if (rows) {
        int total = chip_info_collect_lines(rows, CHIP_INFO_MAX_ROWS);

        int feature_start = total;
        for (int i = 0; i < total; i++) {
            if (strcmp(rows[i].value, "Yes") == 0) {
                feature_start = i;
                break;
            }
        }

        for (int i = 0; i < feature_start; i++) {
            glog("  %s: %s\n", rows[i].label, rows[i].value);
        }

        if (feature_start < total) {
            glog("\n  Enabled Features:\n");
            for (int i = feature_start; i < total; i++) {
                glog("    %s\n", rows[i].label);
            }
        }

        free(rows);
    }

    glog("[CHIPINFO_END]\n");

    status_display_show_status("Chip Info");
}


void handle_mirror_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: mirror <on|off|refresh|status>\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        screen_mirror_set_enabled(true);
        glog("Screen mirror enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        screen_mirror_set_enabled(false);
        glog("Screen mirror disabled\n");
    } else if (strcmp(argv[1], "refresh") == 0) {
        screen_mirror_refresh();
    } else if (strcmp(argv[1], "status") == 0) {
        glog("Screen mirror: %s\n", screen_mirror_is_enabled() ? "on" : "off");
    } else {
        glog("Usage: mirror <on|off|refresh|status>\n");
    }
}





void handle_apps_cmd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        glog("apps list\n");
        glog("apps reload\n");
        glog("apps info <id>\n");
        glog("apps run <id>\n");
        glog("apps reset <id>\n");
        glog("apps stop\n");
        return;
    }

    if (strcmp(argv[1], "reload") == 0) {
        int count = plugin_manager_reload();
        if (count < 0) {
            glog("apps reload failed: %s\n", plugin_manager_last_error());
            return;
        }
        glog("apps reloaded: %d found\n", count);
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (plugin_manager_count() == 0) plugin_manager_reload();
        int count = plugin_manager_count();
        glog("SD apps: %d\n", count);
        for (int i = 0; i < count; ++i) {
            const plugin_app_manifest_t *app = plugin_manager_get(i);
            if (!app) continue;
            glog("  %s - %s v%s [%s]%s\n", app->id, app->name, app->version[0] ? app->version : "?",
                 app->target[0] ? app->target : "any",
                 app->launch_failure_count > 0 ? " [failures]" : "");
        }
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            glog("Usage: apps info <id>\n");
            return;
        }
        if (plugin_manager_count() == 0) plugin_manager_reload();
        const plugin_app_manifest_t *app = plugin_manager_find(argv[2]);
        if (!app) {
            glog("app not found: %s\n", argv[2]);
            return;
        }
        glog("id: %s\nname: %s\nversion: %s\nauthor: %s\ntarget: %s\nentry: %s\napi: %u\nfailures: %lu\n",
             app->id, app->name, app->version, app->author, app->target, app->entry,
              (unsigned)app->api_version,
              (unsigned long)app->launch_failure_count);
        return;
    }

    if (strcmp(argv[1], "reset") == 0 || strcmp(argv[1], "unquarantine") == 0) {
        if (argc < 3) {
            glog("Usage: apps reset <id>\n");
            return;
        }
        if (!plugin_manager_reset_app_state(argv[2])) {
            glog("apps reset failed: %s\n", plugin_manager_last_error());
            return;
        }
        plugin_manager_reload();
        glog("app state reset: %s\n", argv[2]);
        return;
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            glog("Usage: apps run <id>\n");
            return;
        }
        if (plugin_manager_count() == 0) plugin_manager_reload();
#ifdef CONFIG_WITH_SCREEN
        plugin_runner_set_app(argv[2]);
        display_manager_switch_view(&plugin_runner_view);
        glog("launching app in UI: %s\n", argv[2]);
#else
        plugin_loaded_app_t *loaded = NULL;
        esp_err_t err = plugin_loader_load(argv[2], &loaded);
        if (err != ESP_OK) {
            glog("apps run failed: %s\n", plugin_loader_last_error());
            return;
        }
        plugin_loader_start(loaded);
        glog("app started: %s\n", argv[2]);
#endif
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        plugin_loader_unload(plugin_loader_current());
        glog("app stopped\n");
        return;
    }

    glog("unknown apps command: %s\n", argv[1]);
}

void handle_devices_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    glog("Enabled devices:\n");

#ifdef CONFIG_HAS_INFRARED
    {
        int32_t tx_override = settings_get_ir_tx_pin(&G_Settings);
        glog("  Infrared TX: GPIO%d (%s)\n", (int)infrared_get_tx_pin(),
             tx_override >= 0 ? "override" : "default");
    }
#endif
#ifdef CONFIG_HAS_INFRARED_RX
    {
        int32_t rx_override = settings_get_ir_rx_pin(&G_Settings);
        glog("  Infrared RX: GPIO%d (%s) [%s]\n", (int)infrared_get_rx_pin(),
             rx_override >= 0 ? "override" : "default",
             infrared_manager_rx_is_initialized() ? "listening" : "idle");
    }
#endif
    {
        int32_t gps_pin = settings_get_gps_rx_pin(&G_Settings);
        if (gps_pin > 0) {
            glog("  GPS: RX GPIO%ld [%s]\n", (long)gps_pin,
                 g_gpsManager.isinitilized ? "running" : "idle");
        } else {
            glog("  GPS: default pin [%s]\n",
                 g_gpsManager.isinitilized ? "running" : "idle");
        }
    }
    {
        if (rgb_manager.is_separate_pins) {
            glog("  RGB: R GPIO%d G GPIO%d B GPIO%d\n",
                 (int)rgb_manager.red_pin, (int)rgb_manager.green_pin, (int)rgb_manager.blue_pin);
        } else {
            glog("  RGB: data GPIO%d, %d LEDs\n", (int)rgb_manager.pin, rgb_manager.num_leds);
        }
    }
    {
        const char *state = sd_card_manager.is_initialized ? "mounted" : "not mounted";
        if (sd_card_manager.spi_cs_pin > 0) {
            glog("  SD Card: SPI (CS GPIO%d) [%s]\n", sd_card_manager.spi_cs_pin, state);
        } else {
            glog("  SD Card: MMC [%s]\n", state);
        }
    }
#ifdef CONFIG_HAS_SUBGHZ
    glog("  SubGHz: [%s]\n", subghz_remote_manager_is_ready() ? "ready" : "idle");
#endif
#ifdef CONFIG_HAS_NRF24
    glog("  NRF24: [%s]\n", nrf24_remote_manager_is_running() ? "running" : "idle");
#endif
}
