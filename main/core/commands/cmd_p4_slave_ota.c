#include "core/commands.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "core/glog.h"
#include "managers/p4_slave_ota_manager.h"
#include <string.h>

void handle_p4_slave_ota_cmd(int argc, char **argv)
{
    bool force = argc > 1 && strcmp(argv[1], "force") == 0;
    if (argc > 1 && !force) {
        glog("Usage: c6ota [force]\n");
        return;
    }

    glog(force ? "Updating hosted C6 (forced)...\n" :
                 "Checking hosted C6 firmware...\n");
    p4_slave_ota_result_t result = p4_slave_ota_update(force);
    if (result == P4_SLAVE_OTA_UPDATED) {
        glog("C6 updated. Rebooting P4...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else if (result == P4_SLAVE_OTA_SKIPPED) {
        glog("C6 firmware is already current.\n");
    } else {
        glog("C6 update failed. Check the hosted link and slave_fw partition.\n");
    }
}

#endif
