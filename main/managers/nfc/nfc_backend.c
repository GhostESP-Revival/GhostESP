// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdkconfig.h"

#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)

#include <stdbool.h>
#include <string.h>

#include "managers/nfc/nfc_backend.h"

#if defined(CONFIG_NFC_DEFAULT_BACKEND_ST25R3916)
#define NFC_BACKEND_DEFAULT NFC_BACKEND_ST25R3916
#elif defined(CONFIG_NFC_DEFAULT_BACKEND_PN532)
#define NFC_BACKEND_DEFAULT NFC_BACKEND_PN532
#else
#define NFC_BACKEND_DEFAULT NFC_BACKEND_AUTO
#endif

static nfc_backend_t s_nfc_backend = NFC_BACKEND_DEFAULT;

nfc_backend_t nfc_backend_get(void) {
    return s_nfc_backend;
}

void nfc_backend_set(nfc_backend_t backend) {
    s_nfc_backend = backend;
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
