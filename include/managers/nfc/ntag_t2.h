#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef CONFIG_NFC_PN532
#include "pn532.h"  // for NTAG2XX_MODEL and pn532_io_handle_t
#else
// pn532 disabled: provide minimal stand-ins so parsing APIs compile
typedef int NTAG2XX_MODEL;
enum { NTAG2XX_UNKNOWN = 0, NTAG2XX_NTAG213 = 1, NTAG2XX_NTAG215 = 2, NTAG2XX_NTAG216 = 3 };
typedef void* pn532_io_handle_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_NFC_PN532
bool ntag_t2_read_user_memory(pn532_io_handle_t io,
                              uint8_t **out_buf,
                              size_t *out_len,
                              NTAG2XX_MODEL *out_model);
#endif

bool ntag_t2_find_ndef(const uint8_t *mem,
                       size_t mem_len,
                       size_t *msg_off,
                       size_t *msg_len);

const char *ntag_t2_model_str(NTAG2XX_MODEL m);

// Convenience: Build a details string from raw Type 2 memory
// Returns malloc'd string which the caller must free.
char *ntag_t2_build_details_from_mem(const uint8_t *mem,
                                     size_t mem_len,
                                     const uint8_t *uid,
                                     uint8_t uid_len,
                                     NTAG2XX_MODEL model);

#ifdef __cplusplus
}
#endif
