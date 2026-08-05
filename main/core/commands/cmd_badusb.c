// cmd_badusb.c
// BadUSB and USB keyboard host commands.

#include "core/commands.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "managers/settings_manager.h"
#include "managers/usb_keyboard_manager.h"
#include "sdkconfig.h"
#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_builtin_script.h"
#include "managers/badusb_manager.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t badusb_join_args(char *out, size_t out_len, int argc, char **argv, int start_idx) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    for (int i = start_idx; i < argc; i++) {
        const char *arg = argv[i] ? argv[i] : "";
        size_t arg_len = strlen(arg);
        if (used + arg_len + 1 >= out_len) break;
        if (used > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, arg, arg_len);
        used += arg_len;
        out[used] = '\0';
    }
    return used;
}

static void badusb_strip_quotes(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    if (len < 2) return;
    if ((text[0] == '"' && text[len - 1] == '"') || (text[0] == '\'' && text[len - 1] == '\'')) {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

void handle_badusb_cmd(int argc, char **argv) {
#ifdef CONFIG_HAS_BADUSB
    bool remote_request = esp_comm_manager_is_remote_command();

    if (argc < 2) {
        glog("Usage: badusb <run|list|stop|exec|set_vid|set_pid|set_mfr|set_prod|set_rand|set_layout|type|keysend|jiggle_start|jiggle_stop|keyboard_start|keyboard_stop|trackpad_start|trackpad_stop|trackpad_move|trackpad_button|trackpad_wheel>\n");
        glog("  badusb run <filename>       - Execute a DuckyScript from /mnt/ghostesp/badusb/\n");
        glog("  badusb list                 - List available scripts\n");
        glog("  badusb stop                 - Stop current execution\n");
        glog("  badusb exec <size>          - Prepare to receive a script via stream\n");
        glog("  badusb set_vid <hex>        - Set USB VID for next run\n");
        glog("  badusb set_pid <hex>        - Set USB PID for next run\n");
        glog("  badusb set_mfr <text>       - Set USB manufacturer for next run\n");
        glog("  badusb set_prod <text>      - Set USB product for next run\n");
        glog("  badusb set_rand <0|1>       - Toggle USB detail randomization\n");
        glog("  badusb set_layout <n>       - Set keyboard layout for next run\n");
        glog("  badusb type <text>          - Type text through active keyboard mode\n");
        glog("  badusb keysend <mod> <key>  - Send a single keypress (HID codes)\n");
        glog("  badusb jiggle_start         - Start mouse jiggler\n");
        glog("  badusb jiggle_stop          - Stop mouse jiggler\n");
        glog("  badusb keyboard_start       - Start USB keyboard mode\n");
        glog("  badusb keyboard_stop        - Stop USB keyboard mode\n");
        glog("  badusb trackpad_start       - Start USB trackpad (mouse) mode\n");
        glog("  badusb trackpad_stop        - Stop USB trackpad mode\n");
        glog("  badusb trackpad_move <dx> <dy> - Send relative mouse move (each axis clamped to int8)\n");
        glog("  badusb trackpad_button <mask>  - Set held mouse buttons (1=L 2=R 4=M, 0=release)\n");
        glog("  badusb trackpad_wheel <delta> - Send vertical mouse wheel delta (int8)\n");
        return;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "list") == 0) {
        char scripts[32][64];
        int count = badusb_manager_list_scripts(scripts, 32);
        if (count == 0) {
            glog("No scripts found in /mnt/ghostesp/badusb/\n");
        } else {
            glog("BadUSB scripts (%d):\n", count);
            for (int i = 0; i < count; i++) {
                glog("  [%d] %s\n", i, scripts[i]);
            }
        }
    } else if (strcmp(sub, "run") == 0) {
        if (argc < 3) {
            glog("Usage: badusb run <filename|builtin>\n");
            return;
        }
        if (strcmp(argv[2], "builtin") == 0) {
            char *buf = strdup(badusb_builtin_script);
            if (buf) {
                esp_err_t ret = badusb_manager_execute_buffer(buf, BADUSB_BUILTIN_SCRIPT_LEN);
                if (ret != ESP_OK) {
                    glog("BadUSB: Failed to execute built-in script\n");
                }
            } else {
                glog("BadUSB: Out of memory\n");
            }
        } else {
            char path[256];
            snprintf(path, sizeof(path), "/mnt/ghostesp/badusb/%s", argv[2]);
            esp_err_t ret = badusb_manager_execute_file(path);
            if (ret != ESP_OK) {
                glog("BadUSB: Failed to execute %s\n", argv[2]);
            }
        }
    } else if (strcmp(sub, "exec") == 0) {
        if (argc < 3) {
            glog("Usage: badusb exec <size>\n");
            return;
        }
        size_t size = (size_t)atoi(argv[2]);
        esp_err_t ret = badusb_manager_prepare_receive(size);
        if (ret != ESP_OK && !remote_request) {
            glog("BadUSB: Failed to prepare receive\n");
        }
    } else if (strcmp(sub, "stop") == 0) {
        badusb_manager_stop();
        if (!remote_request) glog("BadUSB: Stopped\n");
    } else if (strcmp(sub, "set_vid") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_vid <hex>\n");
            return;
        }
        uint16_t vid = (uint16_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_vid(&G_Settings, vid);
        if (!remote_request) glog("BadUSB: VID set\n");
    } else if (strcmp(sub, "set_pid") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_pid <hex>\n");
            return;
        }
        uint16_t pid = (uint16_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_pid(&G_Settings, pid);
        if (!remote_request) glog("BadUSB: PID set\n");
    } else if (strcmp(sub, "set_mfr") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_mfr <text>\n");
            return;
        }
        char value[64];
        badusb_join_args(value, sizeof(value), argc, argv, 2);
        badusb_strip_quotes(value);
        settings_set_badusb_manufacturer(&G_Settings, value);
        if (!remote_request) glog("BadUSB: Manufacturer set\n");
    } else if (strcmp(sub, "set_prod") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_prod <text>\n");
            return;
        }
        char value[64];
        badusb_join_args(value, sizeof(value), argc, argv, 2);
        badusb_strip_quotes(value);
        settings_set_badusb_product(&G_Settings, value);
        if (!remote_request) glog("BadUSB: Product set\n");
    } else if (strcmp(sub, "set_rand") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_rand <0|1>\n");
            return;
        }
        bool enabled = atoi(argv[2]) != 0;
        settings_set_badusb_randomize(&G_Settings, enabled);
        if (!remote_request) glog("BadUSB: Randomize set to %u\n", enabled ? 1 : 0);
    } else if (strcmp(sub, "set_layout") == 0) {
        if (argc < 3) {
            glog("Usage: badusb set_layout <n>\n");
            return;
        }
        uint8_t layout = (uint8_t)strtol(argv[2], NULL, 0);
        settings_set_badusb_kb_layout(&G_Settings, layout);
        if (!remote_request) glog("BadUSB: Layout set to %u\n", layout);
    } else if (strcmp(sub, "keysend") == 0) {
        if (argc < 4) {
            glog("Usage: badusb keysend <modifier> <keycode>\n");
            return;
        }
        uint8_t mod = (uint8_t)strtol(argv[2], NULL, 0);
        uint8_t key = (uint8_t)strtol(argv[3], NULL, 0);
        if (!badusb_manager_send_keypress(mod, key)) {
            glog("BadUSB: Failed to send key (is HID active?)\n");
        } else {
            glog("BadUSB: Key sent (mod=0x%02X key=0x%02X)\n", mod, key);
        }
    } else if (strcmp(sub, "type") == 0) {
        if (argc < 3) {
            glog("Usage: badusb type <text>\n");
            return;
        }
        char value[256];
        badusb_join_args(value, sizeof(value), argc, argv, 2);
        badusb_strip_quotes(value);
        if (!badusb_manager_is_active()) {
            esp_err_t ret = badusb_manager_keyboard_mode_start();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                glog("BadUSB: Failed to start keyboard mode: %s\n", esp_err_to_name(ret));
                return;
            }
        }
        if (!badusb_manager_send_text(value)) {
            glog("BadUSB: Failed to type text\n");
        } else {
            glog("BadUSB: Typed %u chars\n", (unsigned)strlen(value));
        }
    } else if (strcmp(sub, "type_char") == 0) {
        if (argc < 3) {
            glog("Usage: badusb type_char <ascii>\n");
            return;
        }
        int code = (int)strtol(argv[2], NULL, 0);
        if (code < 1 || code > 126) {
            glog("BadUSB: Invalid ASCII code\n");
            return;
        }
        if (!badusb_manager_is_active()) {
            esp_err_t ret = badusb_manager_keyboard_mode_start();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                glog("BadUSB: Failed to start keyboard mode: %s\n", esp_err_to_name(ret));
                return;
            }
        }
        char one[2] = {(char)code, '\0'};
        if (!badusb_manager_send_text(one)) {
            glog("BadUSB: Failed to type char\n");
        }
    } else if (strcmp(sub, "jiggle_start") == 0) {
        esp_err_t ret = badusb_manager_mouse_jiggle_start();
        if (ret != ESP_OK) {
            glog("BadUSB: Failed to start jiggler: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadUSB: Mouse jiggler started\n");
        }
    } else if (strcmp(sub, "jiggle_stop") == 0) {
        badusb_manager_mouse_jiggle_stop();
        glog("BadUSB: Mouse jiggler stopped\n");
    } else if (strcmp(sub, "keyboard_start") == 0) {
        esp_err_t ret = badusb_manager_keyboard_mode_start();
        if (ret != ESP_OK) {
            glog("BadUSB: Failed to start keyboard mode: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadUSB: Keyboard mode started\n");
        }
    } else if (strcmp(sub, "keyboard_stop") == 0) {
        badusb_manager_keyboard_mode_stop();
        glog("BadUSB: Keyboard mode stopped\n");
    } else if (strcmp(sub, "trackpad_start") == 0) {
        esp_err_t ret = badusb_manager_trackpad_start();
        if (ret != ESP_OK) {
            glog("BadUSB: Failed to start trackpad mode: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadUSB: Trackpad mode started\n");
        }
    } else if (strcmp(sub, "trackpad_stop") == 0) {
        badusb_manager_trackpad_stop();
        glog("BadUSB: Trackpad mode stopped\n");
    } else if (strcmp(sub, "trackpad_move") == 0) {
        if (argc < 4) {
            glog("Usage: badusb trackpad_move <dx> <dy>\n");
            return;
        }
        int dx = atoi(argv[2]);
        int dy = atoi(argv[3]);
        badusb_manager_trackpad_move(dx, dy);
    } else if (strcmp(sub, "trackpad_button") == 0) {
        if (argc < 3) {
            glog("Usage: badusb trackpad_button <mask>\n");
            return;
        }
        uint8_t buttons = (uint8_t)strtoul(argv[2], NULL, 0);
        badusb_manager_trackpad_button(buttons);
    } else if (strcmp(sub, "trackpad_wheel") == 0) {
        if (argc < 3) {
            glog("Usage: badusb trackpad_wheel <delta>\n");
            return;
        }
        int delta = atoi(argv[2]);
        badusb_manager_trackpad_wheel(delta);
    } else if (strcmp(sub, "status") == 0) {
        // Status update from peer - forward to view
#ifdef CONFIG_WITH_SCREEN
        if (argc >= 3) {
            extern void badusb_view_update_status(const char *status);
            badusb_view_update_status(argv[2]);
        }
#endif
    } else {
        glog("Unknown badusb subcommand: %s\n", sub);
    }
#elif defined(CONFIG_HAS_BADUSB_REMOTE)
    if (argc < 2) {
        glog("BadUSB remote: no subcommand\n");
        return;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "status") == 0) {
        // Status update from S3 peer - forward to display view
#ifdef CONFIG_WITH_SCREEN
        if (argc >= 3) {
            extern void badusb_view_update_status(const char *status);
            badusb_view_update_status(argv[2]);
        }
#endif
    } else {
        glog("Unknown badusb subcommand: %s\n", sub);
    }
#else
    glog("BadUSB not enabled on this build\n");
#endif
}

void handle_usb_kbd_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: usbkbd <on|off|status>\n");
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        usb_keyboard_manager_set_host_mode(true);
        glog("USB keyboard host mode enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        usb_keyboard_manager_set_host_mode(false);
        glog("USB keyboard host mode disabled\n");
    } else if (strcmp(argv[1], "status") == 0) {
        glog("USB keyboard host mode: %s\n", usb_keyboard_manager_is_host_mode() ? "on" : "off");
    } else {
        glog("Usage: usbkbd <on|off|status>\n");
    }
}
