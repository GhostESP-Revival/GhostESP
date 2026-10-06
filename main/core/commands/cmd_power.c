#include "core/commands.h"
#include "core/glog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/battery_manager.h"
#include "managers/power_manager.h"
#include "sdkconfig.h"

static void power_print_status(void) {
    battery_status_t bs;
    bool have_batt = battery_manager_get(&bs);

    glog("profile:        %s (effective: %s)\n", power_profile_name(power_manager_get_profile()),
         power_profile_name(power_manager_get_effective_profile()));
    glog("cpu:            %d MHz\n", esp_clk_cpu_freq() / 1000000);
    glog("pm compiled:    %s\n", power_manager_pm_compiled() ? "yes" : "no (no DFS / light sleep on this build)");
    glog("light sleep:    %s (validated flag: %s)\n", power_manager_light_sleep_available() ? "available" : "off",
         power_manager_light_sleep_validated() ? "set" : "not set");
    power_deep_sleep_cfg_t cfg = power_manager_board_sleep_cfg();
    if (cfg.wake_gpio == POWER_GPIO_NONE) {
        glog("deep sleep:     no wake pin declared (CONFIG_POWER_WAKE_GPIO)\n");
    } else {
        glog("deep sleep:     wake GPIO%d level %d, rail GPIO %d\n", cfg.wake_gpio, cfg.wake_level,
             cfg.power_rail_gpio);
    }
    if (!battery_manager_present()) {
        glog("battery:        none on this board\n");
    } else if (have_batt) {
        glog("battery:        %u%% %umV %s, level %s\n", bs.percent, bs.voltage_mv,
             bs.charging ? "charging" : "discharging", battery_level_name(battery_manager_get_level()));
    } else {
        glog("battery:        no valid reading yet\n");
    }
    glog("last boot:      reset reason %d, wake causes 0x%x\n", (int)esp_reset_reason(),
         (unsigned)power_manager_wake_causes());
}

void handle_power_cmd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        power_print_status();
        return;
    }

    if (strcmp(argv[1], "profile") == 0) {
        if (argc < 3) {
            glog("Usage: power profile <performance|balanced|saver>\n");
            return;
        }
        power_profile_t pp;
        if (!power_profile_parse(argv[2], &pp)) {
            glog("Invalid profile. Use performance, balanced or saver\n");
            return;
        }
        power_manager_set_profile(pp, true);
        glog("profile set to %s\n", power_profile_name(pp));
        return;
    }

    if (strcmp(argv[1], "locks") == 0) {
#if CONFIG_PM_ENABLE
        // Anything listed with a non-zero count is what is currently preventing sleep/DFS.
        esp_pm_dump_locks(stdout);
#else
        glog("PM is not compiled into this build\n");
#endif
        return;
    }

    if (strcmp(argv[1], "sleeptest") == 0) {
        int sec = argc >= 3 ? atoi(argv[2]) : 3;
        if (sec < 1 || sec > 60) {
            glog("Usage: power sleeptest [1-60 seconds]\n");
            return;
        }
        glog("light sleeping %ds (console is silent while asleep)...\n", sec);
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t slept_ms = 0;
        uint32_t causes = 0;
        esp_err_t err = power_manager_light_sleep_timed((uint32_t)sec * 1000u, &slept_ms, &causes);
        glog("light sleep result: %s, slept %u ms of %d ms, wake causes 0x%x\n", esp_err_to_name(err),
             (unsigned)slept_ms, sec * 1000, (unsigned)causes);
        if (err != ESP_OK) {
            glog("FAIL: the SoC refused light sleep (a PM lock or peripheral is blocking it)\n");
        } else if (slept_ms + 500 < (uint32_t)sec * 1000u) {
            glog("WARN: woke early - an interrupt source is waking the board; check wake cause\n");
        } else {
            glog("PASS: slept the full time; check the backlight/LEDs/touch still behave, then\n");
            glog("      set CONFIG_POWER_LIGHT_SLEEP_VALIDATED for this config\n");
        }
        return;
    }

    if (strcmp(argv[1], "deepsleep") == 0) {
        int sec = argc >= 3 ? atoi(argv[2]) : 10;
        if (sec < 1 || sec > 3600) {
            glog("Usage: power deepsleep [1-3600 seconds]\n");
            return;
        }
        power_deep_sleep_cfg_t cfg = power_manager_board_sleep_cfg();
        cfg.timer_wake_us = (uint64_t)sec * 1000000ULL;
        cfg.settle_ms = 100;
        glog("deep sleeping %ds; the board resets on wake (boot log shows the wake cause).\n", sec);
        glog("Check: screen dark, LEDs off, rails off, then it comes back on its own.\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_err_t err = power_manager_deep_sleep(&cfg);
        glog("deep sleep failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        esp_err_t err = power_manager_shutdown();
        glog("shutdown failed: %s\n", esp_err_to_name(err));
        return;
    }

    glog("Usage: power [status]\n");
    glog("       power profile <performance|balanced|saver>\n");
    glog("       power locks\n");
    glog("       power sleeptest [seconds]    timed light sleep, reports how long it really slept\n");
    glog("       power deepsleep [seconds]    timed deep sleep through the full shutdown path\n");
    glog("       power off                    deep sleep until the board's wake button\n");
}
