#include "sdkconfig.h"
#include "core/glog.h"
#include "managers/usb_msc_manager.h"
#include "managers/settings_manager.h"
#include <string.h>

void handle_usbsd_cmd(int argc, char **argv) {
#ifndef CONFIG_HAS_USB_MSC_SD
    (void)argc;
    (void)argv;
    glog("usbsd: USB SD passthrough is not supported on this board\n");
#else
    bool active = usb_msc_is_active();

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        glog("USB SD passthrough is %s (preference: %s)\n",
             active ? "ACTIVE" : (settings_get_usb_msc_enabled(&G_Settings) ? "enabled (inactive this boot - run 'usbsd on')" : "inactive"),
             settings_get_usb_msc_enabled(&G_Settings) ? "on" : "off");
        if (active) {
            glog("Card is owned by the USB host. Serial console is unavailable until 'usbsd off'.\n");
        }
        return;
    }

    bool turn_on = !active;
    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            turn_on = true;
        } else if (strcmp(argv[1], "off") == 0) {
            turn_on = false;
        } else if (strcmp(argv[1], "toggle") == 0) {
            turn_on = !active;
        } else {
            glog("Usage: usbsd [on|off|toggle|status]\n");
            return;
        }
    } else if (argc > 2) {
        glog("Usage: usbsd [on|off|toggle|status]\n");
        return;
    }

    esp_err_t ret = turn_on ? usb_msc_start() : usb_msc_stop();
    if (ret == ESP_OK) {
        settings_set_usb_msc_enabled(&G_Settings, turn_on);
        settings_save(&G_Settings);
    }
#endif
}
