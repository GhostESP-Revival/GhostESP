#pragma once

void cmd_wifi_scan_start(int argc, char **argv);
void cmd_wifi_scan_stop(int argc, char **argv);
void cmd_wifi_scan_results(int argc, char **argv);
void handle_list(int argc, char **argv);
void handle_beaconspam(int argc, char **argv);
void handle_stop_spam(int argc, char **argv);
void handle_sta_scan(int argc, char **argv);
void handle_attack_cmd(int argc, char **argv);
void handle_sae_flood_cmd(int argc, char **argv);
void handle_stop_sae_flood_cmd(int argc, char **argv);
void handle_sae_flood_help_cmd(int argc, char **argv);
void handle_stop_deauth(int argc, char **argv);
void handle_select_cmd(int argc, char **argv);
void handle_ip_lookup(int argc, char **argv);
void handle_capture_scan(int argc, char **argv);
void handle_apcred(int argc, char **argv);
void handle_ap_enable_cmd(int argc, char **argv);
void handle_congestion_cmd(int argc, char **argv);
void handle_scanall(int argc, char **argv);
void handle_listen_probes_cmd(int argc, char **argv);
void handle_beaconadd(int argc, char **argv);
void handle_beaconremove(int argc, char **argv);
void handle_beaconclear(int argc, char **argv);
void handle_beaconshow(int argc, char **argv);
void handle_beaconspamlist(int argc, char **argv);
void handle_dhcpstarve_cmd(int argc, char **argv);
#if CONFIG_IDF_TARGET_ESP32C5
void handle_setcountry(int argc, char **argv);
#endif
void handle_wifi_disconnect(int argc, char **argv);