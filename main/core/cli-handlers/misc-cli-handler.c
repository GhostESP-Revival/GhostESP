#include "core/cli-handlers/misc-cli-handler.h"
#include "core/callbacks.h"
#include "managers/gps_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/sd_card_manager.h"
#include "managers/wifi_manager.h"
#include "managers/settings_manager.h"
#include "vendor/pcap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

extern GPSManager g_gpsManager;
extern FSettings G_Settings;

// Help command
void handle_help(int argc, char **argv) {

    const char *category = (argc > 1) ? argv[1] : "all";

    if (strcmp(category, "wifi") == 0) {
        printf("\nWi-Fi Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nWi-Fi Commands:\n\n");
        printf("scanap\n    Start a Wi-Fi access point (AP) scan.\n    Usage: scanap [seconds]\n\n");
        printf("scansta\n    Start scanning for Wi-Fi stations.\n    Usage: scansta\n\n");
        printf("attack\n    Launch Wi-Fi attacks (deauth, EAPOL, SAE flood).\n    Usage: attack -d|-e|-s\n\n");
        printf("list\n    List Wi-Fi scan results or stations.\n    Usage: list -a|-s\n\n");
        printf("beaconspam\n    Start beacon spam.\n    Usage: beaconspam [option]\n\n");
        printf("stopspam\n    Stop beacon spam.\n    Usage: stopspam\n\n");
        printf("stopdeauth\n    Stop deauth/EAPOL/SAE attacks.\n    Usage: stopdeauth\n\n");
        printf("select\n    Select APs or stations.\n    Usage: select -a|-s <index>\n\n");
        printf("apcred\n    Change/reset AP credentials.\n    Usage: apcred <ssid> <password> | apcred -r\n\n");
        printf("apenable\n    Enable/disable AP.\n    Usage: apenable <on|off>\n\n");
        printf("scanall\n    Combined AP/STA scan.\n    Usage: scanall [seconds]\n\n");
        printf("congestion\n    Show Wi-Fi channel congestion.\n    Usage: congestion\n\n");
        TERMINAL_VIEW_ADD_TEXT("scanap, scansta, attack, list, beaconspam, stopspam, stopdeauth, select, apcred, apenable, scanall, congestion\n");
        return;
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(category, "ble") == 0) {
        printf("\nBLE Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nBLE Commands:\n\n");
        printf("blescan\n    Start BLE scan.\n    Usage: blescan [option]\n\n");
        printf("blespam\n    Start BLE spam.\n    Usage: blespam [option]\n\n");
        printf("blewardriving\n    Start BLE wardriving.\n    Usage: blewardriving [-s]\n\n");
        printf("list -airtags\n    List AirTags.\n    Usage: list -airtags\n\n");
        printf("select -airtag <index>\n    Select AirTag by index.\n\n");
        TERMINAL_VIEW_ADD_TEXT("blescan, blespam, blewardriving, list -airtags, select -airtag\n");
        return;
    }
#endif

    if (strcmp(category, "misc") == 0) {
        printf("\nMiscellaneous Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nMiscellaneous Commands:\n\n");
        printf("help\n    Show help menu.\n    Usage: help [category]\n\n");
        printf("chipinfo\n    Show chip info.\n    Usage: chipinfo\n\n");
        printf("timezone\n    Set timezone.\n    Usage: timezone <TZ_STRING>\n\n");
        printf("webauth\n    Enable/disable web authentication.\n    Usage: webauth <on|off>\n\n");
        printf("pineap\n    Start/stop PineAP detection.\n    Usage: pineap [-s]\n\n");
        printf("scanports\n    Scan ports.\n    Usage: scanports local|IP [option]\n\n");
        printf("tp_link_test\n    Test TP-Link smart plug.\n    Usage: tp_link_test <on|off|loop>\n\n");
        TERMINAL_VIEW_ADD_TEXT("help, chipinfo, timezone, webauth, pineap, scanports, tp_link_test\n");
        return;
    }
    if (strcmp(category, "gps") == 0) {
        printf("\nGPS Commands:\n\n");
        TERMINAL_VIEW_ADD_TEXT("\nGPS Commands:\n\n");
        printf("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo\n\n");
        printf("startwd\n    Start GPS wardriving.\n    Usage: startwd [seconds]\n\n");
        TERMINAL_VIEW_ADD_TEXT("gpsinfo, startwd\n");
        return;
    }
    printf("\n Ghost ESP Commands:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n Ghost ESP Commands:\n\n");

    printf("help\n");
    printf("    Description: Display this help message.\n");
    printf("    Usage: help\n\n");
    TERMINAL_VIEW_ADD_TEXT("help\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display this help message.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: help\n\n");

    printf("scanap\n");
    printf("    Description: Start a Wi-Fi access point (AP) scan.\n");
    printf("    Usage: scanap [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a Wi-Fi access point (AP) scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanap [seconds]\n\n");

    printf("scansta\n");
    printf("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    printf("    Usage: scansta\n\n");
    TERMINAL_VIEW_ADD_TEXT("scansta\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scansta\n\n");

    printf("stopscan\n");
    printf("    Description: Stop any ongoing Wi-Fi scan.\n");
    printf("    Usage: stopscan\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopscan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop any ongoing Wi-Fi scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopscan\n\n");

    printf("attack\n");
    printf("    Description: Launch an attack (e.g., deauthentication attack).\n");
    printf("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
    printf("    Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
    printf("    Arguments:\n");
    printf("        -d  : Start deauth attack (supports multiple APs)\n");
    printf("        -e  : Start EAPOL logoff attack\n");
    printf("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n");
    printf("\n");
    TERMINAL_VIEW_ADD_TEXT("attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Launch an attack (e.g., deauthentication attack).\n");
    TERMINAL_VIEW_ADD_TEXT("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: attack -d (deauth) | attack -e (EAPOL logoff) | attack -s (SAE flood)\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -d  : Start deauth attack (supports multiple APs)\n");
    TERMINAL_VIEW_ADD_TEXT("        -e  : Start EAPOL logoff attack\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n");
    TERMINAL_VIEW_ADD_TEXT("\n");

    printf("list\n");
    printf("    Description: List Wi-Fi scan results or connected stations.\n");
    printf("    Usage: list -a | list -s | list -airtags\n");
    printf("    Arguments:\n");
    printf("        -a  : Show access points from Wi-Fi scan\n");
    printf("        -s  : List connected stations\n");
    printf("        -airtags: List discovered AirTags\n\n");
    TERMINAL_VIEW_ADD_TEXT("list\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: List Wi-Fi scan results or connected stations.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: list -a | list -s | list -airtags\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a  : Show access points from Wi-Fi scan\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : List connected stations\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtags: List discovered AirTags\n\n");

    printf("beaconspam\n");
    printf("    Description: Start beacon spam with different modes.\n");
    printf("    Usage: beaconspam [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -r   : Start random beacon spam\n");
    printf("        -rr  : Start Rickroll beacon spam\n");
    printf("        -l   : Start AP List beacon spam\n");
    printf("        [SSID]: Use specified SSID for beacon spam\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spam with different modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspam [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Start random beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -rr  : Start Rickroll beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -l   : Start AP List beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        [SSID]: Use specified SSID for beacon spam\n\n");

    printf("stopspam\n");
    printf("    Description: Stop ongoing beacon spam.\n");
    printf("    Usage: stopspam\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing beacon spam.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopspam\n\n");

    printf("stopdeauth\n");
    printf("    Description: Stop ongoing deauthentication attack.\n");
    printf("    Usage: stopdeauth\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopdeauth\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing deauthentication attack.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopdeauth\n\n");

    printf("select\n");
    printf("    Description: Select access point(s), station, or AirTag by index from the scan "
           "results.\n");
    printf("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
    printf("    Arguments:\n");
    printf("        -a      : AP selection index (supports multiple: 1,3,5)\n");
    printf("        -s      : Station selection index\n");
    printf("        -airtag : AirTag selection index\n");
    printf("    Examples:\n");
    printf("        select -a 4      : Select single AP at index 4\n");
    printf("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
    TERMINAL_VIEW_ADD_TEXT("select\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Select access point(s), station, or AirTag by index "
                           "from the scan results.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a      : AP selection index (supports multiple: 1,3,5)\n");
    TERMINAL_VIEW_ADD_TEXT("        -s      : Station selection index\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtag : AirTag selection index\n");
    TERMINAL_VIEW_ADD_TEXT("    Examples:\n");
    TERMINAL_VIEW_ADD_TEXT("        select -a 4      : Select single AP at index 4\n");
    TERMINAL_VIEW_ADD_TEXT("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");

    printf("startportal\n");
    printf("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
    printf("                 /mnt/ prefix is added automatically to file paths if missing.\n");
    printf("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    printf("           PSK is optional for an open network.\n");
    printf("    Use 'default' as the file path for the default Evil Portal.");
    TERMINAL_VIEW_ADD_TEXT("startportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Start Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          Use 'default' as the file path for the default Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          /mnt/ added to paths automatically.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    TERMINAL_VIEW_ADD_TEXT("           PSK is optional for an open network.\n");


    printf("stopportal\n");
    printf("    Description: Stop Evil Portal\n");
    printf("    Usage: stopportal\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop Evil Portal\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopportal\n\n");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    printf("blescan\n");
    printf("    Description: Handle BLE scanning with various modes.\n");
    printf("    Usage: blescan [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    printf("        -ds  : Start BLE spam detector\n");
    printf("        -a   : Start AirTag scanner\n");
    printf("        -r   : Scan for raw BLE packets\n");
    printf("        -s   : Stop BLE scanning\n\n");
    TERMINAL_VIEW_ADD_TEXT("blescan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Handle BLE scanning with various modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blescan [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    TERMINAL_VIEW_ADD_TEXT("        -ds  : Start BLE spam detector\n");
    TERMINAL_VIEW_ADD_TEXT("        -a   : Start AirTag scanner\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Scan for raw BLE packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -s   : Stop BLE scanning\n\n");

    printf("blespam\n");
    printf("    Description: Start BLE advertisement spam attacks.\n");
    printf("    Usage: blespam [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -apple     : Apple device spam (AirPods, Apple TV, etc.)\n");
    printf("        -ms        : Microsoft Swift Pair spam\n");
    printf("        -samsung   : Samsung Galaxy Watch spam\n");
    printf("        -google    : Google Fast Pair spam\n");
    printf("        -random    : Random spam (cycles through all types)\n");
    printf("        -s         : Stop BLE spam\n\n");
    TERMINAL_VIEW_ADD_TEXT("blespam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start BLE advertisement spam attacks.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blespam [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -apple     : Apple device spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -ms        : Microsoft Swift Pair spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -samsung   : Samsung Galaxy Watch spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -google    : Google Fast Pair spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -random    : Random spam (all types)\n");
    TERMINAL_VIEW_ADD_TEXT("        -s         : Stop BLE spam\n\n");
#endif

    printf("capture\n");
    printf("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    printf("    Usage: capture [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -probe   : Start Capturing Probe Packets\n");
    printf("        -beacon  : Start Capturing Beacon Packets\n");
    printf("        -deauth   : Start Capturing Deauth Packets\n");
    printf("        -raw   :   Start Capturing Raw Packets\n");
    printf("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    printf("        -pwn   :   Start Capturing Pwnagotchi Packets");
    printf("        -stop   : Stops the active capture\n\n");
    TERMINAL_VIEW_ADD_TEXT("capture\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: capture [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -probe   : Start Capturing Probe Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -beacon  : Start Capturing Beacon Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -deauth   : Start Capturing Deauth Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -raw   :   Start Capturing Raw Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    TERMINAL_VIEW_ADD_TEXT("        -pwn   :   Start Capturing Pwnagotchi Packets");
    TERMINAL_VIEW_ADD_TEXT("        -stop   : Stops the active capture\n\n");

    printf("connect\n");
    printf("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    printf("    Usage: connect <SSID> [Password]\n");
    TERMINAL_VIEW_ADD_TEXT("connect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: connect <SSID> [Password]\n");

    printf("dialconnect\n");
    printf("    Description: Cast a Random Youtube Video on all Smart TV's on "
           "your LAN (Requires You to Run Connect First)\n");
    printf("    Usage: dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Cast a Random Youtube Video on all Smart TV's on your "
                           "LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dialconnect\n");

    printf("powerprinter\n");
    printf("    Description: Print Custom Text to a Printer on your LAN "
           "(Requires You to Run Connect First)\n");
    printf("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    printf("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
           "Right, BR = Bottom Right, BL = Bottom Left\n\n");
    TERMINAL_VIEW_ADD_TEXT("powerprinter\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Print Custom Text to a Printer on "
                           "your LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    TERMINAL_VIEW_ADD_TEXT("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
                           "Right, BR = Bottom Right, BL = Bottom Left\n\n");

    printf("blewardriving\n");
    printf("    Description: Start/Stop BLE wardriving with GPS logging\n");
    printf("    Usage: blewardriving [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop BLE wardriving\n\n");
    TERMINAL_VIEW_ADD_TEXT("blewardriving\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop BLE wardriving with GPS logging\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blewardriving [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop BLE wardriving\n\n");

    printf("pineap\n");
    printf("    Description: Start/Stop detecting WiFi Pineapples.\n");
    printf("    Usage: pineap [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop PineAP detection\n\n");
    TERMINAL_VIEW_ADD_TEXT("pineap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop detecting WiFi Pineapples.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: pineap [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop PineAP detection\n\n");

    printf("Port Scanner\n");
    printf("    Description: Scan ports on local subnet or specific IP\n");
    printf("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    printf("           scanports [IP] [-C/-A/start_port-end_port]\n");
    printf("    Arguments:\n");
    printf("        -C  : Scan common ports only\n");
    printf("        -A  : Scan all ports (1-65535)\n");
    printf("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");
    TERMINAL_VIEW_ADD_TEXT("Port Scanner\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Scan ports on local subnet or specific IP\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("           scanports [IP] [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -C  : Scan common ports only\n");
    TERMINAL_VIEW_ADD_TEXT("        -A  : Scan all ports (1-65535)\n");
    TERMINAL_VIEW_ADD_TEXT("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");

    printf("congestion\n");
    printf("    Description: Display Wi-Fi channel congestion chart.\n");
    printf("    Usage: congestion\n\n");
    TERMINAL_VIEW_ADD_TEXT("congestion\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display Wi-Fi channel congestion chart.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: congestion\n\n");

    printf("apcred\n");
    printf("    Description: Change or reset the GhostNet AP credentials\n");
    printf("    Usage: apcred <ssid> <password>\n");
    printf("           apcred -r (reset to defaults)\n");
    printf("    Arguments:\n");
    printf("        <ssid>     : New SSID for the AP\n");
    printf("        <password> : New password (min 8 characters)\n");
    printf("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
    TERMINAL_VIEW_ADD_TEXT("apcred\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change or reset the GhostNet AP credentials\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: apcred <ssid> <password>\n");
    TERMINAL_VIEW_ADD_TEXT("           apcred -r (reset to defaults)\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        <ssid>     : New SSID for the AP\n");
    TERMINAL_VIEW_ADD_TEXT("        <password> : New password (min 8 characters)\n");
    TERMINAL_VIEW_ADD_TEXT("        -r        : Reset to default (GhostNet/GhostNet)\n\n");

    printf("apenable\n");
    printf("    Description: Enable or disable the Access Point across reboots\n");
    printf("    Usage: apenable <on|off>\n");
    printf("    Arguments:\n");
    printf("        on  : Enable the Access Point (requires restart)\n");
    printf("        off : Disable the Access Point (requires restart)\n\n");
    TERMINAL_VIEW_ADD_TEXT("apenable\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Enable or disable the Access Point across reboots\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: apenable <on|off>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        on  : Enable the Access Point (requires restart)\n");
    TERMINAL_VIEW_ADD_TEXT("        off : Disable the Access Point (requires restart)\n\n");

    printf("chipinfo\n");
    printf("    Description: Display chip information including model, revision, and features\n");
    printf("    Usage: chipinfo\n");
    printf("    Shows:\n");
    printf("        - Chip model and revision\n");
    printf("        - CPU cores and features\n");
    printf("        - Flash size and memory info\n");
    printf("        - ESP-IDF version\n\n");
    TERMINAL_VIEW_ADD_TEXT("chipinfo\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display chip information including model, revision, and features\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: chipinfo\n");
    TERMINAL_VIEW_ADD_TEXT("    Shows chip model, revision, CPU cores, features, flash size, and memory info\n\n");

    printf("rgbmode\n");
    printf("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    printf("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");
    TERMINAL_VIEW_ADD_TEXT("rgbmode\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");

    printf("setrgbpins\n");
    printf("    Description: Change RGB LED pins\n");
    printf("    Usage: setrgbpins <red> <green> <blue>\n");
    printf("           (use same value for all pins for single-pin LED strips)\n\n");
    TERMINAL_VIEW_ADD_TEXT("setrgbpins\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change RGB LED pins\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: setrgbpins <red> <green> <blue>\n");
    TERMINAL_VIEW_ADD_TEXT("           (use same value for all pins for single-pin LED strips)\n\n");

    // SD Card Commands Help Text
    printf("\n-- SD Card Pin Configuration --\n");
    printf("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    printf("These commands configure pins for the *active* mode.\n");
    printf("Changing the mode requires recompiling firmware.\n");
    TERMINAL_VIEW_ADD_TEXT("\n-- SD Card Pin Configuration --\n");
    TERMINAL_VIEW_ADD_TEXT("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    TERMINAL_VIEW_ADD_TEXT("These commands configure pins for the *active* mode.\n");
    TERMINAL_VIEW_ADD_TEXT("Changing the mode requires recompiling firmware.\n");

    printf("sd_config\n");
    printf("    Description: Show the currently configured GPIO pins for both SDMMC and SPI modes.\n");
    printf("    Usage: sd_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show current SD GPIO pin configuration.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_config\n\n");

    printf("sd_pins_mmc\n");
    printf("    Description: Set GPIO pins for SDMMC mode (1 or 4 bit). Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SDMMC mode.\n");
    printf("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    printf("    Example: sd_pins_mmc 19 18 20 21 22 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_mmc\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SDMMC mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SDMMC.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");

    printf("sd_pins_spi\n");
    printf("    Description: Set GPIO pins for SPI mode. Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SPI mode.\n");
    printf("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    printf("    Example: sd_pins_spi 5 18 19 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_spi\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SPI mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SPI.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");

    printf("sd_save_config\n");
    printf("    Description: Save the current SD pin configuration (both modes) to the SD card.\n");
    printf("                 Requires SD card to be mounted.\n");
    printf("    Usage: sd_save_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_save_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Save current SD pin config to SD card.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_save_config\n\n");

    printf("scanall\n");
    printf("    Description: Perform combined AP and Station scan, display results.\n");
    printf("    Usage: scanall [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanall\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Combined AP/STA scan & display.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanall [seconds]\n\n");

    printf("timezone\n");
    printf("    Description: Set the display timezone for the clock view.\n");
    printf("    Usage: timezone <TZ_STRING>\n\n");
    TERMINAL_VIEW_ADD_TEXT("timezone\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set the display timezone for the clock view.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: timezone <TZ_STRING>\n\n");

    printf("beaconadd\n");
    printf("    Description: Add an SSID to the beacon spam list.\n");
    printf("    Usage: beaconadd <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconadd\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Add an SSID to the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconadd <SSID>\n\n");

    printf("beaconremove\n");
    printf("    Description: Remove an SSID from the beacon spam list.\n");
    printf("    Usage: beaconremove <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconremove\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Remove an SSID from the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconremove <SSID>\n\n");

    printf("beaconclear\n");
    printf("    Description: Clear the beacon spam list.\n");
    printf("    Usage: beaconclear\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconclear\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Clear the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconclear\n\n");

    printf("beaconshow\n");
    printf("    Description: Show the current beacon spam list.\n");
    printf("    Usage: beaconshow\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconshow\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show the current beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconshow\n\n");

    printf("beaconspamlist\n");
    printf("    Description: Start beacon spamming using the beacon spam list.\n");
    printf("    Usage: beaconspamlist\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspamlist\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spamming using the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspamlist\n\n");

    printf("dhcpstarve\n");
    printf("    Description: DHCP starvation flood attack\n");
    printf("    Usage: dhcpstarve start [threads]\n");
    printf("           dhcpstarve stop\n");
    printf("           dhcpstarve display\n\n");
    TERMINAL_VIEW_ADD_TEXT("dhcpstarve\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: DHCP starvation flood attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dhcpstarve start [threads]\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve stop\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve display\n\n");

    printf("saeflood\n");
    printf("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
    printf("    Usage: saeflood (requires selected WPA3 AP)\n\n");
    TERMINAL_VIEW_ADD_TEXT("saeflood\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: saeflood (requires selected WPA3 AP)\n\n");

    printf("stopsaeflood\n");
    printf("    Description: Stop SAE flood attack\n");
    printf("    Usage: stopsaeflood\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopsaeflood\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop SAE flood attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopsaeflood\n\n");

    printf("saefloodhelp\n");
    printf("    Description: Show detailed SAE flood attack help\n");
    printf("    Usage: saefloodhelp\n\n");
    TERMINAL_VIEW_ADD_TEXT("saefloodhelp\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show detailed SAE flood attack help\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: saefloodhelp\n\n");

#if CONFIG_IDF_TARGET_ESP32C5
    printf("setcountry\n");
    printf("    Description: Set the Wi-Fi country code.\n");
    printf("    Usage: setcountry <CC>\n");
    printf("    Arguments:\n");
    printf("        <CC> : Two-letter ISO country code (e.g., US, GB, JP)\n\n");
    TERMINAL_VIEW_ADD_TEXT("setcountry\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set the Wi-Fi country code.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: setcountry <CC>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        <CC> : Two-letter ISO country code (e.g., US, GB, JP)\n\n");
#endif

    printf("listenprobes\n");
    printf("    Description: Listen for and log probe requests.\n");
    printf("    Usage: listenprobes [channel] [stop]\n");
    printf("    Arguments:\n");
    printf("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
    printf("        stop      : Stop probe request listening\n\n");
    TERMINAL_VIEW_ADD_TEXT("listenprobes\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Listen for and log probe requests.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: listenprobes [channel] [stop]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
    TERMINAL_VIEW_ADD_TEXT("        stop      : Stop probe request listening\n\n");

    printf("webauth\n");
    printf("    Description: Enable/disable web authentication.\n");
    printf("    Usage: webauth <enable|disable>\n\n");
    TERMINAL_VIEW_ADD_TEXT("webauth\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Enable/disable web authentication.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: webauth <enable|disable>\n\n");

    printf("commdiscovery\n");
    printf("    Description: Check discovery status (auto-starts on boot).\n");
    printf("    Usage: commdiscovery\n\n");
    TERMINAL_VIEW_ADD_TEXT("commdiscovery\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Check discovery status (auto-starts on boot).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commdiscovery\n\n");

    printf("commconnect\n");
    printf("    Description: Connect to a discovered peer ESP32.\n");
    printf("    Usage: commconnect <peer_name>\n");
    printf("    Example: commconnect ESP_A1B2C3\n\n");
    TERMINAL_VIEW_ADD_TEXT("commconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Connect to a discovered peer ESP32.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commconnect <peer_name>\n\n");

    printf("commsend\n");
    printf("    Description: Send a command to connected peer ESP32.\n");
    printf("    Usage: commsend <command> [data]\n");
    printf("    Example: commsend scanap\n");
    printf("    Example: commsend hello world\n\n");
    TERMINAL_VIEW_ADD_TEXT("commsend\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Send a command to connected peer ESP32.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commsend <command> [data]\n\n");

    printf("commstatus\n");
    printf("    Description: Show communication status and connection state.\n");
    printf("    Usage: commstatus\n\n");
    TERMINAL_VIEW_ADD_TEXT("commstatus\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show communication status and connection state.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commstatus\n\n");

    printf("commdisconnect\n");
    printf("    Description: Disconnect from current peer.\n");
    printf("    Usage: commdisconnect\n\n");
    TERMINAL_VIEW_ADD_TEXT("commdisconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Disconnect from current peer.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commdisconnect\n\n");

    printf("commsetpins\n");
    printf("    Description: Change communication GPIO pins at runtime.\n");
    printf("    Usage: commsetpins <tx_pin> <rx_pin>\n");
    printf("    Example: commsetpins 4 5\n\n");
    TERMINAL_VIEW_ADD_TEXT("commsetpins\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change communication GPIO pins at runtime.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: commsetpins <tx_pin> <rx_pin>\n\n");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    printf("blescan\n");
    printf("    Description: Start Bluetooth Low Energy (BLE) scan.\n");
    printf("    Usage: blescan [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("blescan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start Bluetooth Low Energy (BLE) scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blescan [seconds]\n\n");
#endif
}


// Wardriving command
void handle_startwd(int argc, char **argv) {
    bool stop_flag = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }
    if (stop_flag) {
        gps_manager_deinit(&g_gpsManager);
        wifi_manager_stop_monitor_mode();
        csv_flush_buffer_to_file();
        csv_file_close();
        printf("Wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving stopped.\n");
    } else {
        gps_manager_init(&g_gpsManager);
        if (sd_card_exists("/mnt/ghostesp/gps")) {
            esp_err_t err = csv_file_open("wardriving");
            if (err != ESP_OK) {
                printf("Failed to open CSV for wardriving\n");
                TERMINAL_VIEW_ADD_TEXT("Failed to open CSV for wardriving\n");
            }
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        printf("Wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving started.\n");
    }
}

// Port scan command
void handle_scan_ports(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\nscanports local [-C/-A/start_port-end_port]\nscanports [IP] [-C/-A/start_port-end_port]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage:\nscanports local [-C/-A/start_port-end_port]\nscanports [IP] [-C/-A/start_port-end_port]\n");
        return;
    }
    bool is_local = strcmp(argv[1], "local") == 0;
    const char *target_ip = NULL;
    const char *port_arg = NULL;
    if (is_local) {
        if (argc < 3) {
            printf("Missing port argument for local scan\n");
            TERMINAL_VIEW_ADD_TEXT("Missing port argument for local scan\n");
            return;
        }
        port_arg = argv[2];
        wifi_manager_scan_subnet();
        return;
    } else {
        if (argc < 3) {
            printf("Missing port argument for IP scan\n");
            TERMINAL_VIEW_ADD_TEXT("Missing port argument for IP scan\n");
            return;
        }
        target_ip = argv[1];
        port_arg = argv[2];
    }
    // Example: scan_ports_on_host and scan_ip_port_range are assumed to be implemented elsewhere
    host_result_t result;
    if (strcmp(port_arg, "-C") == 0) {
        scan_ports_on_host(target_ip, &result);
        if (result.num_open_ports > 0) {
            printf("Open ports on %s:\n", target_ip);
            TERMINAL_VIEW_ADD_TEXT("Open ports on %s:\n", target_ip);
            for (int i = 0; i < result.num_open_ports; i++) {
                printf("Port %d\n", result.open_ports[i]);
                TERMINAL_VIEW_ADD_TEXT("Port %d\n", result.open_ports[i]);
            }
        }
    } else {
        int start_port, end_port;
        if (strcmp(port_arg, "-A") == 0) {
            start_port = 1;
            end_port = 65535;
        } else if (sscanf(port_arg, "%d-%d", &start_port, &end_port) != 2 || start_port < 1 ||
                   end_port > 65535 || start_port > end_port) {
            printf("Invalid port range\n");
            TERMINAL_VIEW_ADD_TEXT("Invalid port range\n");
            return;
        }
        scan_ip_port_range(target_ip, start_port, end_port);
    }
}

// Web authentication command
void handle_web_auth_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: webauth <on|off>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: webauth <on|off>\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        settings_set_web_auth_enabled(&G_Settings, true);
        settings_save(&G_Settings);
        printf("Web authentication enabled.\n");
        TERMINAL_VIEW_ADD_TEXT("Web authentication enabled.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        settings_set_web_auth_enabled(&G_Settings, false);
        settings_save(&G_Settings);
        printf("Web authentication disabled.\n");
        TERMINAL_VIEW_ADD_TEXT("Web authentication disabled.\n");
    } else {
        printf("Usage: webauth <on|off>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: webauth <on|off>\n");
    }
}

// PineAP detection command
void handle_pineap_detection(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping PineAP detection...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        return;
    }
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        printf("Warning: Failed to open PCAP file for logging\n");
        TERMINAL_VIEW_ADD_TEXT("Warning: Failed to open PCAP file for logging\n");
    }
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);
    printf("Monitoring for Pineapples\n");
    TERMINAL_VIEW_ADD_TEXT("Monitoring for Pineapples\n");
}

void encrypt_tp_link_command(const char *input, uint8_t *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = output[i];
    }
}

void decrypt_tp_link_response(const uint8_t *input, char *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = input[i];
    }
}
// TP-Link test command
void handle_tp_link_test(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: tp_link_test <on|off|loop>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: tp_link_test <on|off|loop>\n");
        return;
    }
    bool isloop = false;
    if (strcmp(argv[1], "loop") == 0) {
        isloop = true;
    } else if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        printf("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        return;
    }
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9999);
    int iterations = isloop ? 10 : 1;
    for (int i = 0; i < iterations; i++) {
        const char *command;
        if (isloop) {
            command = (i % 2 == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        } else {
            command = (strcmp(argv[1], "on") == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        }
        uint8_t encrypted_command[128] = {0};
        size_t command_len = strlen(command);
        if (command_len >= sizeof(encrypted_command)) {
            printf("Command too large to encrypt\n");
            TERMINAL_VIEW_ADD_TEXT("Command too large to encrypt\n");
            return;
        }
        // Assume encrypt_tp_link_command is implemented elsewhere
        encrypt_tp_link_command(command, encrypted_command, command_len);
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("Failed to create socket: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT("Failed to create socket\n");
            return;
        }
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Error occurred during sending: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT("Error occurred during sending\n");
            close(sock);
            return;
        }
        printf("Broadcast message sent: %s\n", command);
        TERMINAL_VIEW_ADD_TEXT("Broadcast message sent\n");
        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr, &addr_len);
        if (len < 0) {
            printf("No response from any device\n");
            TERMINAL_VIEW_ADD_TEXT("No response from any device\n");
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            // Assume decrypt_tp_link_response is implemented elsewhere
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            printf("Response: %s\n", decrypted_response);
            TERMINAL_VIEW_ADD_TEXT("Response received\n");
        }
        close(sock);
        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}