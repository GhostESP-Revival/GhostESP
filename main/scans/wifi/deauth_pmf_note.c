/**
 * @file deauth_pmf_note.c
 * @brief Plain-English note about whether deauth is expected to get through
 *
 * A network that requires protected management frames (PMF, 802.11w) discards
 * any deauthentication it cannot decrypt, so plain deauth silently does
 * nothing against WPA3. Saying so before the attack starts is the difference
 * between "it is broken" and "it is working as designed".
 *
 * The wording is deliberately plain. This is printed to a terminal or a small
 * status display, so it avoids PMF, MFPR and transition-mode jargon and says
 * what will actually happen and what to try instead.
 */

#include "scans/wifi/deauth_pmf_note.h"

/* Only the enum is needed from ESP-IDF, and only so the switch names the modes
 * symbolically instead of using bare numbers. */
#include "esp_wifi_types_generic.h"

const char *deauth_pmf_note(uint8_t authmode) {
    switch ((wifi_auth_mode_t)authmode) {
        /* WPA3 requires protected management frames, so an unprotected deauth
         * is thrown away without a reply. */
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA3_ENTERPRISE:
        case WIFI_AUTH_WPA3_ENT_192:
            return "WPA3 network, so it drops unprotected deauth.";

        /* Transition mode mixes WPA2 and WPA3 on one SSID: WPA2 clients drop,
         * WPA3 clients hold. */
        case WIFI_AUTH_WPA2_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
            return "WPA2/WPA3 mixed, so WPA3 clients will drop deauth.";

        /* WPA2 networks are usually PMF-capable but not PMF-required, so they
         * still honour an unprotected deauth. */
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WPA_ENTERPRISE:
            return "WPA2 network, so deauth should work.";

        /* Nothing to protect. */
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_WEP:
            return NULL;

        /* The AP was not in the scan, or the mode is not one we recognise. */
        default:
            return "Unknown network security, so deauth may not work.";
    }
}
