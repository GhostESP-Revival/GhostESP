// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NFC_BACKEND_H
#define NFC_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

typedef enum {
    NFC_BACKEND_AUTO = 0,
    NFC_BACKEND_PN532,
    NFC_BACKEND_ST25R3916,
} nfc_backend_t;

nfc_backend_t nfc_backend_get(void);
void nfc_backend_set(nfc_backend_t backend);
const char *nfc_backend_name(nfc_backend_t backend);
bool nfc_backend_parse(const char *name, nfc_backend_t *out_backend);

#ifdef __cplusplus
}
#endif

#endif // NFC_BACKEND_H
