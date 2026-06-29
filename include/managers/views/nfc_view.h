#ifndef NFC_VIEW_H
#define NFC_VIEW_H

#include "managers/display_manager.h"

extern View nfc_view;

#ifdef __cplusplus
extern "C" {
#endif

bool nfc_api_get_last_uid(uint8_t *uid_out, uint8_t *uid_len_out);

#ifdef __cplusplus
}
#endif

#endif
