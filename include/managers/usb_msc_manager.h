#ifndef USB_MSC_MANAGER_H
#define USB_MSC_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB SD card passthrough (TinyUSB MSC).
 *
 * Hands the mounted SD card to a USB host as a mass-storage device so the
 * board behaves like a USB SD card reader. While active the firmware cannot
 * access the card and the native USB serial console is unavailable; exit the
 * mode (CLI over WiFi/WebUI, or the options screen) to restore them.
 */
esp_err_t usb_msc_start(void);
esp_err_t usb_msc_stop(void);
esp_err_t usb_msc_start_async(void);
bool usb_msc_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // USB_MSC_MANAGER_H
