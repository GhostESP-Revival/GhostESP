#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_NFC_PN532) && defined(__has_include)
#if __has_include("pn532.h")
#include "pn532.h"
#else
typedef int NTAG2XX_MODEL;
enum {
    NTAG2XX_UNKNOWN = 0,
    NTAG2XX_NTAG213 = 1,
    NTAG2XX_NTAG215 = 2,
    NTAG2XX_NTAG216 = 3,
};
typedef void *pn532_io_handle_t;
#endif
#elif defined(CONFIG_NFC_PN532)
#include "pn532.h"
#else
typedef int NTAG2XX_MODEL;
enum {
    NTAG2XX_UNKNOWN = 0,
    NTAG2XX_NTAG213 = 1,
    NTAG2XX_NTAG215 = 2,
    NTAG2XX_NTAG216 = 3,
};
typedef void *pn532_io_handle_t;
#endif
