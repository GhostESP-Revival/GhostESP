#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NDEF_WIFI_AUTH_OPEN = 0,
    NDEF_WIFI_AUTH_WEP,
    NDEF_WIFI_AUTH_WPA,
    NDEF_WIFI_AUTH_WPA2,
} ndef_wifi_auth_t;

// Each builder returns a complete TLV blob (0x03 <len> <NDEF message> 0xFE)
// ready to be dropped into Type 2 tag user memory starting at page 4.
// On success *out is malloc'd; caller must free() it.
bool ndef_builder_uri(const char *uri, uint8_t **out, size_t *out_len);
bool ndef_builder_text(const char *text, uint8_t **out, size_t *out_len);
bool ndef_builder_vcard(const char *name, const char *phone, const char *email,
                        uint8_t **out, size_t *out_len);
bool ndef_builder_wifi(const char *ssid, const char *password, ndef_wifi_auth_t auth,
                       uint8_t **out, size_t *out_len);
bool ndef_builder_aar(const char *package_name, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
