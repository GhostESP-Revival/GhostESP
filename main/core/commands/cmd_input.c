// cmd_input.c
// Input helpers, IO buttons, visualizer, and identify commands.

#include "core/commands.h"
#include "core/glog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/music_visualizer.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_raveport_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    glog("RAVE_SERIAL A55AC33C 79\n");
}

void handle_rave_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: rave <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        display_manager_switch_view(&music_visualizer_view);
        glog("Visualizer opened\n");
    } else if (strcmp(argv[1], "off") == 0) {
        display_manager_switch_view(&apps_menu_view);
        glog("Visualizer closed\n");
    } else {
        glog("Usage: rave <on|off>\n");
    }
}

void handle_identify_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    glog("GHOSTESP_OK\n");
}

void handle_input_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: input <left|right|up|down|select>\n");
        return;
    }
    int joystick_index = -1;
    if (strcmp(argv[1], "left") == 0) joystick_index = 0;
    else if (strcmp(argv[1], "select") == 0 || strcmp(argv[1], "ok") == 0) joystick_index = 1;
    else if (strcmp(argv[1], "up") == 0) joystick_index = 2;
    else if (strcmp(argv[1], "right") == 0) joystick_index = 3;
    else if (strcmp(argv[1], "down") == 0) joystick_index = 4;
    
    if (joystick_index < 0) {
        glog("Unknown input: %s\n", argv[1]);
        return;
    }
    
    if (input_queue) {
        InputEvent evt = {
            .type = INPUT_TYPE_JOYSTICK,
            .data.joystick_index = joystick_index,
            .data.joystick_pressed = true
        };
        xQueueSend(input_queue, &evt, 0);
    }
}

void handle_iobtn_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: iobtn <1|2|3> [command]\n");
        glog("  Button 1 = P10, 2 = P11, 3 = P12. Show or set command run on press.\n");
        glog("  Omit [command] to show current; use \"\" to clear (then button sends joystick event).\n");
        return;
    }
    int btn = atoi(argv[1]);
    if (btn < 1 || btn > 3) {
        glog("Button must be 1, 2, or 3 (P10, P11, P12)\n");
        return;
    }
    const char *cur = NULL;
    if (btn == 1) cur = settings_get_io_btn_p10_cmd(&G_Settings);
    else if (btn == 2) cur = settings_get_io_btn_p11_cmd(&G_Settings);
    else cur = settings_get_io_btn_p12_cmd(&G_Settings);
    if (argc == 2) {
        glog("P1%u: \"%s\"\n", (unsigned)(btn + 9), cur && cur[0] ? cur : "(none)");
        return;
    }
    if (btn == 1) settings_set_io_btn_p10_cmd(&G_Settings, argv[2]);
    else if (btn == 2) settings_set_io_btn_p11_cmd(&G_Settings, argv[2]);
    else settings_set_io_btn_p12_cmd(&G_Settings, argv[2]);
    settings_save(&G_Settings);
    glog("P1%u command set to \"%s\"\n", (unsigned)(btn + 9), argv[2][0] ? argv[2] : "(cleared)");
}
