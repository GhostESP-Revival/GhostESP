#include "core/ghostchi_identity.h"

#include "esp_wifi.h"

#include <stdio.h>

static const char s_onsets[] = "bcdfghjklmnprstvwyz";
static const char s_vowels[] = "aeiou";
static const char s_soft_codas[] = "lmnrst";

bool ghostchi_identity_from_mac(const uint8_t mac[6], char *buf, size_t buf_len) {
    if (!mac || !buf || buf_len < GHOSTCHI_IDENTITY_NAME_MAX) return false;

    uint8_t h0 = mac[0] ^ mac[1] ^ mac[2];
    uint8_t h1 = mac[3] ^ mac[4] ^ mac[5];
    uint8_t h2 = (mac[0] ^ mac[3]) + (mac[1] ^ mac[4]) + (mac[2] ^ mac[5]);
    uint8_t h3 = (h0 >> 3) ^ (h1 << 2) ^ h2;
    uint8_t h4 = mac[1] ^ mac[4] ^ (uint8_t)(h2 + h3);
    uint8_t h5 = mac[2] ^ mac[5] ^ (uint8_t)(h0 + h1);
    char c1 = s_onsets[h0 % 20];
    char v1 = s_vowels[(h0 >> 4) % 5];
    char c2 = s_onsets[h1 % 20];
    char v2 = s_vowels[(h1 >> 4) % 5];
    char c3 = s_onsets[h2 % 20];
    char v3 = s_vowels[(h2 >> 4) % 5];

    if (h4 % 3 == 0) {
        snprintf(buf, buf_len, "%c%c%c%c%c", c1, v1, c2, v2, s_soft_codas[h5 % 6]);
    } else if (h4 % 3 == 1) {
        snprintf(buf, buf_len, "%c%c%c%c%c%c", c1, v1, c2, v2, c3, v3);
    } else {
        snprintf(buf, buf_len, "%c%c%c%c%c%c", c1, v1, c2, v2, c3, s_soft_codas[h5 % 6]);
    }
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - ('a' - 'A'));
    return true;
}

bool ghostchi_identity_get_name(char *buf, size_t buf_len) {
    uint8_t mac[6] = {0};
    return esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK &&
           ghostchi_identity_from_mac(mac, buf, buf_len);
}
