#include "sdkconfig.h"

#ifdef CONFIG_HAS_BADBLE

#include "core/commands.h"
#include "core/glog.h"
#include "managers/badble_manager.h"
#include "managers/badusb_builtin_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t badble_join_args(char *out, size_t out_len, int argc, char **argv,
                               int start_idx) {
    if (!out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t used = 0;
    for (int i = start_idx; i < argc; ++i) {
        const char *arg = argv[i] ? argv[i] : "";
        size_t len = strlen(arg);
        if (used + len + (used ? 1 : 0) >= out_len) {
            break;
        }
        if (used) {
            out[used++] = ' ';
        }
        memcpy(out + used, arg, len);
        used += len;
        out[used] = '\0';
    }
    return used;
}

static void badble_strip_quotes(char *text) {
    if (!text) {
        return;
    }
    size_t len = strlen(text);
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') ||
                     (text[0] == '\'' && text[len - 1] == '\''))) {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

static void badble_usage(void) {
    glog("Usage: badble <status|list|run|stop|keyboard_start|keyboard_stop|type|keysend|name|set_name>\n");
    glog("  badble status                 - Show BLE keyboard state\n");
    glog("  badble list                   - List scripts in /mnt/ghostesp/badble/\n");
    glog("  badble run <filename>         - Wait for a host and execute a script\n");
    glog("  badble stop                  - Stop the keyboard and restore BLE\n");
    glog("  badble keyboard_start         - Advertise for live typing\n");
    glog("  badble keyboard_stop          - Stop live typing\n");
    glog("  badble type <text>            - Type text through the connected host\n");
    glog("  badble keysend <mod> <key>   - Send one HID keypress\n");
    glog("  badble name                   - Show the advertised name\n");
    glog("  badble set_name <text>        - Set the advertised name\n");
}

void handle_badble_cmd(int argc, char **argv) {
    if (argc < 2) {
        badble_usage();
        return;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "status") == 0) {
        badble_manager_print_status();
    } else if (strcmp(sub, "list") == 0) {
        char scripts[32][64];
        int count = badble_manager_list_scripts(scripts, 32);
        glog("BadBLE scripts (%d):\n", count);
        for (int i = 0; i < count; ++i) {
            glog("  [%d] %s\n", i, scripts[i]);
        }
        if (count == 0) {
            glog("  No scripts found in /mnt/ghostesp/badble/\n");
        }
    } else if (strcmp(sub, "run") == 0) {
        if (argc < 3) {
            glog("Usage: badble run <filename>\n");
            return;
        }
        char name[64];
        badble_join_args(name, sizeof(name), argc, argv, 2);
        badble_strip_quotes(name);
        esp_err_t ret;
        if (strcmp(name, BADUSB_BUILTIN_SCRIPT_NAME) == 0) {
            ret = badble_manager_run_builtin();
        } else {
            ret = badble_manager_run_script(name);
        }
        if (ret != ESP_OK) {
            glog("BadBLE: Failed to run '%s': %s\n", name, esp_err_to_name(ret));
        } else {
            glog("BadBLE: Script queued\n");
        }
    } else if (strcmp(sub, "stop") == 0) {
        (void)badble_manager_stop();
        glog("BadBLE: Stopped\n");
    } else if (strcmp(sub, "keyboard_start") == 0 || strcmp(sub, "start") == 0) {
        esp_err_t ret = badble_manager_keyboard_start();
        if (ret != ESP_OK) {
            glog("BadBLE: Failed to start keyboard: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadBLE: Keyboard mode started\n");
        }
    } else if (strcmp(sub, "keyboard_stop") == 0) {
        esp_err_t ret = badble_manager_keyboard_stop();
        if (ret != ESP_OK) {
            glog("BadBLE: Failed to stop keyboard mode: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadBLE: Keyboard mode stopped\n");
        }
    } else if (strcmp(sub, "type") == 0) {
        if (argc < 3) {
            glog("Usage: badble type <text>\n");
            return;
        }
        char text[256];
        badble_join_args(text, sizeof(text), argc, argv, 2);
        badble_strip_quotes(text);
        if (!badble_manager_send_text(text)) {
            glog("BadBLE: Failed to type text (connect and start keyboard mode first)\n");
        } else {
            glog("BadBLE: Typed %u chars\n", (unsigned)strlen(text));
        }
    } else if (strcmp(sub, "keysend") == 0) {
        if (argc < 4) {
            glog("Usage: badble keysend <modifier> <keycode>\n");
            return;
        }
        uint8_t modifier = (uint8_t)strtoul(argv[2], NULL, 0);
        uint8_t keycode = (uint8_t)strtoul(argv[3], NULL, 0);
        if (!badble_manager_send_keypress(modifier, keycode)) {
            glog("BadBLE: Failed to send key (connect and start keyboard mode first)\n");
        } else {
            glog("BadBLE: Key sent (mod=0x%02X key=0x%02X)\n", modifier, keycode);
        }
    } else if (strcmp(sub, "name") == 0) {
        glog("BadBLE: name=\"%s\"\n", badble_manager_get_name());
    } else if (strcmp(sub, "set_name") == 0) {
        if (argc < 3) {
            glog("Usage: badble set_name <text>\n");
            return;
        }
        char name[64];
        badble_join_args(name, sizeof(name), argc, argv, 2);
        badble_strip_quotes(name);
        esp_err_t ret = badble_manager_set_name(name);
        if (ret != ESP_OK) {
            glog("BadBLE: Invalid name or save failed: %s\n", esp_err_to_name(ret));
        } else {
            glog("BadBLE: Name saved for the next advertising session\n");
        }
    } else {
        glog("Unknown badble subcommand: %s\n", sub);
        badble_usage();
    }
}

#endif // CONFIG_HAS_BADBLE
