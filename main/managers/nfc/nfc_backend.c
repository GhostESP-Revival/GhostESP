// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdkconfig.h"

#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "managers/nfc/nfc_backend.h"
#include "nvs.h"

#if defined(CONFIG_NFC_DEFAULT_BACKEND_ST25R3916)
#define NFC_BACKEND_DEFAULT NFC_BACKEND_ST25R3916
#elif defined(CONFIG_NFC_DEFAULT_BACKEND_PN532)
#define NFC_BACKEND_DEFAULT NFC_BACKEND_PN532
#else
#define NFC_BACKEND_DEFAULT NFC_BACKEND_AUTO
#endif

#define NFC_BACKEND_NVS_NS  "nfc"
#define NFC_BACKEND_NVS_KEY "backend"

static nfc_backend_t s_nfc_backend = NFC_BACKEND_DEFAULT;
static bool s_nfc_backend_loaded = false;

// Lazily hydrate the cached backend from NVS on first access, so both the CLI
// and the UI observe the persisted choice regardless of init order.
static void nfc_backend_load(void) {
    if (s_nfc_backend_loaded) return;
    s_nfc_backend_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NFC_BACKEND_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, NFC_BACKEND_NVS_KEY, &v) == ESP_OK && v <= NFC_BACKEND_ST25R3916) {
        s_nfc_backend = (nfc_backend_t)v;
    }
    nvs_close(h);
}

nfc_backend_t nfc_backend_get(void) {
    nfc_backend_load();
    return s_nfc_backend;
}

void nfc_backend_set(nfc_backend_t backend) {
    nfc_backend_load();
    if (backend == s_nfc_backend) return;
    s_nfc_backend = backend;
    nvs_handle_t h;
    if (nvs_open(NFC_BACKEND_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, NFC_BACKEND_NVS_KEY, (uint8_t)backend) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

const char *nfc_backend_name(nfc_backend_t backend) {
    switch (backend) {
        case NFC_BACKEND_AUTO: return "auto";
        case NFC_BACKEND_PN532: return "pn532";
        case NFC_BACKEND_ST25R3916: return "st25r";
        default: return "unknown";
    }
}

bool nfc_backend_parse(const char *name, nfc_backend_t *out_backend) {
    if (!name || !out_backend) return false;
    if (strcmp(name, "auto") == 0) {
        *out_backend = NFC_BACKEND_AUTO;
    } else if (strcmp(name, "pn532") == 0) {
        *out_backend = NFC_BACKEND_PN532;
    } else if (strcmp(name, "st25r") == 0 || strcmp(name, "st25r3916") == 0) {
        *out_backend = NFC_BACKEND_ST25R3916;
    } else {
        return false;
    }
    return true;
}

#endif
