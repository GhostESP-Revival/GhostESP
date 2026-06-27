// cmd_ir.c
// Infrared remote control, learning, and universal sending commands.

#include "core/commands.h"
#include "core/glog.h"
#include "core/universal_ir.h"
#include "managers/infrared_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/error_popup.h"
#include "managers/ghostchi_manager.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TaskHandle_t g_ir_universal_send_task = NULL;
static volatile bool g_ir_universal_send_cancel = false;

static TaskHandle_t g_ir_rx_learn_task = NULL;
#define IR_CLI_MAX_REMOTES 128
static char *g_ir_cli_remote_paths[IR_CLI_MAX_REMOTES];
static size_t g_ir_cli_remote_count = 0;

typedef struct {
    bool use_builtin;
    char path[256];
    char name[64];
    uint32_t delay_ms;
} IrUniversalSendArgs;

typedef enum {
    IR_BG_MODE_RX,
    IR_BG_MODE_LEARN,
} IrRxLearnMode;

typedef struct {
    IrRxLearnMode mode;
    int timeout_sec;
    char path[256];
} IrRxLearnArgs;

static void ir_universal_send_task(void *arg);
static void ir_rx_learn_task(void *arg);

static void ir_cli_clear_remote_index(void) {
    for (size_t i = 0; i < g_ir_cli_remote_count; ++i) {
        free(g_ir_cli_remote_paths[i]);
        g_ir_cli_remote_paths[i] = NULL;
    }
    g_ir_cli_remote_count = 0;
}

static bool ir_cli_is_number(const char *s) {
    if (!s || !*s) return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        ++s;
    }
    return true;
}

static void resolve_ir_path(const char *input, char *output, size_t max_len) {
    if (!input || strlen(input) == 0) {
        snprintf(output, max_len, "/mnt/ghostesp/infrared/remotes");
    } else if (input[0] == '/') {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
    } else {
        snprintf(output, max_len, "/mnt/ghostesp/infrared/remotes/%s", input);
    }
}

static void resolve_ir_universal_path(const char *input, char *output, size_t max_len) {
    const char *base = "/mnt/ghostesp/infrared/universals";
    if (!input || strlen(input) == 0) {
        strncpy(output, base, max_len - 1);
        output[max_len - 1] = '\0';
    } else if (input[0] == '/') {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
    } else {
        snprintf(output, max_len, "%s/%s", base, input);
    }
}

static void ir_universal_send_task(void *arg) {
    IrUniversalSendArgs *args = (IrUniversalSendArgs *)arg;
    bool use_builtin = args->use_builtin;
    uint32_t delay_ms = args->delay_ms ? args->delay_ms : 150;

    char path[256];
    char button[64];
    path[0] = '\0';
    strncpy(button, args->name, sizeof(button) - 1);
    button[sizeof(button) - 1] = '\0';
    if (!use_builtin) {
        strncpy(path, args->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    free(args);

    g_ir_universal_send_cancel = false;

    ghostchi_manager_add_xp(1);

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    bool poltergeist_held = false;
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "poltergeist") == 0) {
        infrared_manager_poltergeist_hold_io24_begin();
        poltergeist_held = true;
    }
#endif

    if (use_builtin) {
        size_t total = universal_ir_get_signal_count();
        size_t sent = 0;

        if (total == 0) {
            glog("IR: no built-in universal signals.\n");
        } else {
            glog("IR: universal sendall builtin '%s'\n", button);
            for (size_t i = 0; i < total && !g_ir_universal_send_cancel; ++i) {
                infrared_signal_t sig;
                if (!universal_ir_get_signal(i, &sig)) continue;
                if (strcmp(sig.name, button) != 0) {
                    infrared_manager_free_signal(&sig);
                    continue;
                }
                glog("IR: universal sendall %s [builtin %d]\n", button, (int)i);
                bool ok = infrared_manager_transmit(&sig);
                glog("IR: universal sendall %s -> %s\n", button, ok ? "OK" : "FAIL");
                sent++;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            if (sent == 0) {
                glog("IR: no builtin signals named '%s'\n", button);
            }
        }

    } else {
        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read universal file %s\n", path);
        } else if (count == 0) {
            infrared_manager_free_list(signals, count);
            glog("IR: no signals in %s\n", path);
        } else {
            size_t sent = 0;
            glog("IR: universal sendall '%s' from %s (%zu signals)\n", button, path, count);
            for (size_t i = 0; i < count && !g_ir_universal_send_cancel; ++i) {
                if (strcmp(signals[i].name, button) != 0) continue;
                glog("IR: universal sendall %s [index %d]\n", button, (int)i);
                bool ok = infrared_manager_transmit(&signals[i]);
                glog("IR: universal sendall %s -> %s\n", button, ok ? "OK" : "FAIL");
                sent++;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            if (sent == 0) {
                glog("IR: no signals named '%s' in %s\n", button, path);
            }
            infrared_manager_free_list(signals, count);
        }
    }

    if (g_ir_universal_send_cancel) {
        glog("IR: universal sendall stopped.\n");
    } else {
        glog("IR: universal sendall finished.\n");
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (poltergeist_held) {
        infrared_manager_poltergeist_hold_io24_end();
    }
#endif

    g_ir_universal_send_task = NULL;
    vTaskDelete(NULL);
}

static void ir_rx_learn_task(void *arg) {
    IrRxLearnArgs *args = (IrRxLearnArgs *)arg;
    IrRxLearnMode mode = args->mode;
    int timeout_sec = args->timeout_sec;
    char path[256];
    path[0] = '\0';
    if (mode == IR_BG_MODE_LEARN) {
        strncpy(path, args->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    free(args);

    if (!infrared_manager_rx_init()) {
        if (mode == IR_BG_MODE_RX) {
            glog("IR: failed to init RX (hardware busy?)\n");
        } else {
            glog("IR: failed to init RX\n");
        }
        g_ir_rx_learn_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (mode == IR_BG_MODE_RX) {
        if (timeout_sec <= 0) timeout_sec = 60;
        glog("IR RX mode started. Press Ctrl+C or reset to stop (or wait for timeout).\n");

        bool received_any = false;
        int64_t end = esp_timer_get_time() + (int64_t)timeout_sec * 1000000;

        while (true) {
            int64_t now = esp_timer_get_time();
            if (now >= end) break;

            int remaining_ms = (int)((end - now) / 1000);
            if (remaining_ms <= 0) break;

            infrared_signal_t sig;
            memset(&sig, 0, sizeof(sig));

            if (infrared_manager_rx_receive(&sig, remaining_ms)) {
                if (sig.is_raw) {
                    glog("Received RAW: %zu samples, %lu Hz\n",
                         sig.payload.raw.timings_size,
                         (unsigned long)sig.payload.raw.frequency);
                    infrared_manager_free_signal(&sig);
                } else {
                    glog("Received %s: Addr: 0x%lX Cmd: 0x%lX\n",
                         sig.payload.message.protocol,
                         (unsigned long)sig.payload.message.address,
                         (unsigned long)sig.payload.message.command);
                }
                received_any = true;
                break;
            } else {
                break;
            }
        }

        infrared_manager_rx_deinit();
        if (received_any) {
            glog("IR RX stopped after first signal.\n");
        } else {
            glog("IR RX timed out.\n");
        }
    } else {
        if (timeout_sec <= 0) timeout_sec = 10;
        glog("Waiting for IR signal (10s timeout)...\n");

        infrared_signal_t sig;
        memset(&sig, 0, sizeof(sig));
        if (infrared_manager_rx_receive(&sig, timeout_sec * 1000)) {
            if (sig.is_raw) {
                glog("Captured RAW signal (%zu samples)\n", sig.payload.raw.timings_size);
            } else {
                glog("Captured: %s A:0x%lX C:0x%lX\n", 
                     sig.payload.message.protocol,
                     (unsigned long)sig.payload.message.address,
                     (unsigned long)sig.payload.message.command);
            }

            if (path[0] == '\0') {
                const char *base_dir = "/mnt/ghostesp/infrared/remotes";
                char name_part[96];
                if (!sig.is_raw && sig.payload.message.protocol[0] != '\0') {
                    snprintf(name_part, sizeof(name_part), "Learned_%s_%08lX_%08lX",
                             sig.payload.message.protocol,
                             (unsigned long)sig.payload.message.address,
                             (unsigned long)sig.payload.message.command);
                } else {
                    unsigned long t_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
                    snprintf(name_part, sizeof(name_part), "Learned_RAW_%lu", t_ms);
                }
                snprintf(path, sizeof(path), "%s/%s.ir", base_dir, name_part);
            }

            FILE *f = fopen(path, "a");
            if (f) {
                fprintf(f, "\nname: %s\n", sig.name[0] ? sig.name : "Learned");
                if (sig.is_raw) {
                    fprintf(f, "type: raw\nfrequency: %lu\nduty_cycle: %f\ndata: ", 
                            (unsigned long)sig.payload.raw.frequency, sig.payload.raw.duty_cycle);
                    for(size_t i=0; i<sig.payload.raw.timings_size; i++) {
                        fprintf(f, "%lu%s", (unsigned long)sig.payload.raw.timings[i], (i<sig.payload.raw.timings_size-1)?" ":"\n");
                    }
                } else {
                    fprintf(f, "type: parsed\nprotocol: %s\naddress: %02lX %02lX %02lX %02lX\ncommand: %02lX %02lX %02lX %02lX\n",
                            sig.payload.message.protocol,
                            (unsigned long)((sig.payload.message.address >> 24) & 0xFF),
                            (unsigned long)((sig.payload.message.address >> 16) & 0xFF),
                            (unsigned long)((sig.payload.message.address >> 8) & 0xFF),
                            (unsigned long)(sig.payload.message.address & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 24) & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 16) & 0xFF),
                            (unsigned long)((sig.payload.message.command >> 8) & 0xFF),
                            (unsigned long)(sig.payload.message.command & 0xFF));
                }
                fclose(f);
                glog("Saved to %s\n", path);
            } else {
                glog("Error: Failed to open file %s for writing\n", path);
            }
            infrared_manager_free_signal(&sig);
        } else {
            glog("Timeout, no signal received.\n");
        }
        infrared_manager_rx_deinit();
    }

    g_ir_rx_learn_task = NULL;
    vTaskDelete(NULL);
}



void handle_ir_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: ir <send|inline|list|show|universals|rx|dazzler>\n");
        glog("  ir send <path|remote_index> [button_index]\n");
        glog("  ir inline\n");
        glog("  ir list [path]\n");
        glog("  ir show <path|remote_index>\n");
        glog("  ir universals <list|send|sendall> ...\n");
        glog("  ir rx\n");
        glog("  ir dazzler [stop]\n");
        return;
    }

    const char *sub = argv[1];
    char path[256];

    if (strcmp(sub, "send") == 0) {
        if (argc < 3) {
            glog("Usage: ir send <path|remote_index> [button_index]\n");
            return;
        }
        const char *arg = argv[2];
        int button_index = 0;
        if (argc >= 4) {
            button_index = atoi(argv[3]);
        }

        if (ir_cli_is_number(arg) && g_ir_cli_remote_count > 0) {
            int remote_index = atoi(arg);
            if (remote_index < 0 || (size_t)remote_index >= g_ir_cli_remote_count) {
                glog("IR: remote index out of range (0-%d). Run 'ir list' to see indices.\n",
                     (int)(g_ir_cli_remote_count ? g_ir_cli_remote_count - 1 : 0));
                return;
            }
            strncpy(path, g_ir_cli_remote_paths[remote_index], sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            resolve_ir_path(arg, path, sizeof(path));
        }

        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read list from %s\n", path);
            return;
        }

        if (count == 0) {
            infrared_manager_free_list(signals, count);
            glog("IR: no signals in %s\n", path);
            return;
        }

        if (button_index < 0 || (size_t)button_index >= count) {
            infrared_manager_free_list(signals, count);
            glog("IR: index out of range (0-%d)\n", (int)(count - 1));
            return;
        }

        infrared_signal_t *sig = &signals[button_index];
        ghostchi_manager_add_xp(4);
        bool ok = infrared_manager_transmit(sig);
        glog("IR: send %s\n", ok ? "OK" : "FAIL");
        if (sig->is_raw) {
            if (sig->payload.raw.timings && sig->payload.raw.timings_size > 0) {
                glog("IR: signal raw len=%u freq=%luHz duty=%.2f\n",
                     (unsigned)sig->payload.raw.timings_size,
                     (unsigned long)sig->payload.raw.frequency,
                     (double)sig->payload.raw.duty_cycle);
            }
        } else {
            const char *proto = sig->payload.message.protocol;
            if (proto && proto[0] != '\0') {
                uint32_t addr = sig->payload.message.address;
                uint32_t cmd = sig->payload.message.command;
                if (sig->name[0] != '\0') {
                    glog("IR: signal [%s] protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                         sig->name, proto, (unsigned long)addr, (unsigned long)cmd);
                } else {
                    glog("IR: signal protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                         proto, (unsigned long)addr, (unsigned long)cmd);
                }
            }
        }
        infrared_manager_free_list(signals, count);
        return;
    }

    if (strcmp(sub, "inline") == 0) {
        glog("IR inline mode:\n");
        glog("  Send IR content between [IR/BEGIN] and [IR/CLOSE] markers.\n");
        glog("  Content may be a JSON object or .ir-style text block.\n");
        return;
    }

    if (strcmp(sub, "list") == 0) {
        resolve_ir_path((argc >= 3) ? argv[2] : NULL, path, sizeof(path));
        DIR *d = opendir(path);
        if (!d) {
            glog("IR: failed to open directory %s\n", path);
            ir_cli_clear_remote_index();
            return;
        }
        ir_cli_clear_remote_index();
        struct dirent *dir;
        glog("IR files in %s:\n", path);
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG) {
                 if (strstr(dir->d_name, ".ir") || strstr(dir->d_name, ".json")) {
                     int idx = (int)g_ir_cli_remote_count;
                     if (g_ir_cli_remote_count < IR_CLI_MAX_REMOTES) {
                         char full[512];
                         snprintf(full, sizeof(full), "%s/%s", path, dir->d_name);
                         g_ir_cli_remote_paths[g_ir_cli_remote_count] = strdup(full);
                         if (g_ir_cli_remote_paths[g_ir_cli_remote_count]) {
                             g_ir_cli_remote_count++;
                         }
                     }
                     glog("  [%d] %s\n", idx, dir->d_name);
                 }
            }
        }
        closedir(d);
        if (g_ir_cli_remote_count == 0) {
            glog("  (none)\n");
        }
        return;
    }

    if (strcmp(sub, "show") == 0) {
        if (argc < 3) {
            glog("Usage: ir show <path|remote_index>\n");
            return;
        }
        const char *arg = argv[2];
        if (ir_cli_is_number(arg) && g_ir_cli_remote_count > 0) {
            int remote_index = atoi(arg);
            if (remote_index < 0 || (size_t)remote_index >= g_ir_cli_remote_count) {
                glog("IR: remote index out of range (0-%d). Run 'ir list' first.\n",
                     (int)(g_ir_cli_remote_count ? g_ir_cli_remote_count - 1 : 0));
                return;
            }
            strncpy(path, g_ir_cli_remote_paths[remote_index], sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            resolve_ir_path(arg, path, sizeof(path));
        }
        infrared_signal_t *signals = NULL;
        size_t count = 0;
        if (!infrared_manager_read_list(path, &signals, &count)) {
            glog("IR: failed to read/parse %s\n", path);
            return;
        }
        bool is_universal_file = (strstr(path, "/infrared/universals") != NULL);
        if (is_universal_file) {
            size_t unique = 0;
            for (size_t i = 0; i < count; i++) {
                const char *name = signals[i].name;
                bool seen = false;
                for (size_t j = 0; j < i; j++) {
                    if (strcmp(signals[j].name, name) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) unique++;
            }

            glog("Unique buttons in %s (%zu):\n", path, unique);
            size_t idx = 0;
            for (size_t i = 0; i < count; i++) {
                const char *name = signals[i].name;
                bool seen = false;
                for (size_t j = 0; j < i; j++) {
                    if (strcmp(signals[j].name, name) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (seen) continue;
                glog("  [%d] %s\n", (int)idx, name);
                idx++;
            }
        } else {
            glog("Signals in %s (%zu):\n", path, count);
            for (size_t i = 0; i < count; i++) {
                 const char *proto = signals[i].is_raw ? "RAW" : signals[i].payload.message.protocol;
                 glog("  [%d] %s (%s)", (int)i, signals[i].name, proto);
                 if (!signals[i].is_raw) {
                     glog(" Addr: 0x%lX Cmd: 0x%lX", 
                          (unsigned long)signals[i].payload.message.address, 
                          (unsigned long)signals[i].payload.message.command);
                 }
                 glog("\n");
            }
        }
        infrared_manager_free_list(signals, count);
        return;
    }

    if (strcmp(sub, "universals") == 0) {
        if (argc < 3) {
            glog("Usage: ir universals <list|send|sendall>\n");
            return;
        }
        const char *u_sub = argv[2];

        if (strcmp(u_sub, "list") == 0) {
            bool show_all = (argc >= 4 && strcmp(argv[3], "-all") == 0);

            const char *uni_path = "/mnt/ghostesp/infrared/universals";
            DIR *d = opendir(uni_path);
            if (d) {
                glog("Universal Files in %s:\n", uni_path);
                struct dirent *dir;
                int file_count = 0;
                while ((dir = readdir(d)) != NULL) {
                    if (dir->d_type == DT_REG) {
                         if (strstr(dir->d_name, ".ir") || strstr(dir->d_name, ".json")) {
                             glog("  %s\n", dir->d_name);
                             file_count++;
                         }
                    }
                }
                closedir(d);
                if (file_count == 0) glog("  (none)\n");
            }

            size_t count = universal_ir_get_signal_count();
            if (show_all) {
                glog("\nBuilt-in Universal Signals (%zu):\n", count);
                for (size_t i = 0; i < count; i++) {
                    infrared_signal_t sig;
                    if (universal_ir_get_signal(i, &sig)) {
                         glog("  [%d] %s (%s) Addr: 0x%lX Cmd: 0x%lX\n", (int)i, 
                              sig.name, sig.payload.message.protocol,
                              (unsigned long)sig.payload.message.address,
                              (unsigned long)sig.payload.message.command);
                    }
                }
            } else {
                glog("\nBuilt-in Universal Signals: %zu available.\n", count);
                glog("Use 'ir universals list -all' to list them.\n");
            }
            return;
        }
        if (strcmp(u_sub, "send") == 0) {
            if (argc < 4) {
                glog("Usage: ir universals send <index>\n");
                return;
            }
            int idx = atoi(argv[3]);
            infrared_signal_t sig;
            if (universal_ir_get_signal(idx, &sig)) {
                ghostchi_manager_add_xp(4);
                bool ok = infrared_manager_transmit(&sig);
                glog("IR: universal send %s\n", ok ? "OK" : "FAIL");
            } else {
                glog("IR: invalid universal index\n");
            }
            return;
        }
        if (strcmp(u_sub, "sendall") == 0) {
            if (g_ir_universal_send_task != NULL) {
                glog("IR: universal sendall already running; use 'stop' to cancel.\n");
                return;
            }
            if (argc < 4) {
                glog("Usage: ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]\n");
                return;
            }
            const char *arg = argv[3];
            const char *button_name = NULL;
            uint32_t delay_ms = 150;

            if (strcmp(arg, "TURNHISTVOFF") == 0) {
                if (argc >= 5) {
                    button_name = argv[4];
                    if (argc >= 6) {
                        int d = atoi(argv[5]);
                        if (d > 0) delay_ms = (uint32_t)d;
                    }
                } else {
                    button_name = "Power Off";
                    if (argc >= 5) {
                        int d = atoi(argv[4]);
                        if (d > 0) delay_ms = (uint32_t)d;
                    }
                }
            } else {
                if (argc < 5) {
                    glog("Usage: ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]\n");
                    return;
                }
                button_name = argv[4];
                if (argc >= 6) {
                    int d = atoi(argv[5]);
                    if (d > 0) delay_ms = (uint32_t)d;
                }
            }

            IrUniversalSendArgs *args = (IrUniversalSendArgs *)malloc(sizeof(IrUniversalSendArgs));
            if (!args) {
                glog("IR: failed to allocate sendall args.\n");
                return;
            }
            memset(args, 0, sizeof(*args));
            args->delay_ms = delay_ms;
            strncpy(args->name, button_name, sizeof(args->name) - 1);
            args->name[sizeof(args->name) - 1] = '\0';
            if (strcmp(arg, "TURNHISTVOFF") == 0) {
                args->use_builtin = true;
            } else {
                args->use_builtin = false;
                resolve_ir_universal_path(arg, args->path, sizeof(args->path));
            }
            g_ir_universal_send_cancel = false;
            if (settings_get_epilepsy_warning_enabled(&G_Settings)) {
                error_popup_create("EPILEPSY WARNING\nRGB LED will flash\nduring IR transmission");
            }
            if (xTaskCreate(ir_universal_send_task, "ir_uni_sendall", 4096, args, 5, &g_ir_universal_send_task) != pdPASS) {
                glog("IR: failed to start universal sendall task.\n");
                free(args);
                g_ir_universal_send_task = NULL;
                return;
            }
            glog("IR: universal sendall started for '%s'; use 'stop' to cancel.\n", button_name);
            return;
        }
    }

    if (strcmp(sub, "rx") == 0) {
        if (g_ir_rx_learn_task != NULL) {
            glog("IR RX/learn already running; use 'stop' to cancel.\n");
            return;
        }
        int timeout_sec = 60;
        if (argc >= 3) {
            int t = atoi(argv[2]);
            if (t > 0) timeout_sec = t;
        }
        IrRxLearnArgs *args = (IrRxLearnArgs *)malloc(sizeof(IrRxLearnArgs));
        if (!args) {
            glog("IR: failed to allocate RX task args.\n");
            return;
        }
        memset(args, 0, sizeof(*args));
        args->mode = IR_BG_MODE_RX;
        args->timeout_sec = timeout_sec;
        args->path[0] = '\0';
        if (xTaskCreate(ir_rx_learn_task, "ir_rx", 4096, args, 5, &g_ir_rx_learn_task) != pdPASS) {
            glog("IR: failed to start RX task.\n");
            free(args);
            g_ir_rx_learn_task = NULL;
            return;
        }
        glog("IR RX task started; use 'stop' to cancel.\n");
        return;
    }

    if (strcmp(sub, "learn") == 0) {
        if (g_ir_rx_learn_task != NULL) {
            glog("IR RX/learn already running; use 'stop' to cancel.\n");
            return;
        }
        if (argc >= 3) {
            resolve_ir_path(argv[2], path, sizeof(path));
        } else {
            path[0] = '\0';
        }
        IrRxLearnArgs *args = (IrRxLearnArgs *)malloc(sizeof(IrRxLearnArgs));
        if (!args) {
            glog("IR: failed to allocate learn task args.\n");
            return;
        }
        memset(args, 0, sizeof(*args));
        args->mode = IR_BG_MODE_LEARN;
        args->timeout_sec = 10;
        strncpy(args->path, path, sizeof(args->path) - 1);
        args->path[sizeof(args->path) - 1] = '\0';
        if (xTaskCreate(ir_rx_learn_task, "ir_learn", 5120, args, 5, &g_ir_rx_learn_task) != pdPASS) {
            glog("IR: failed to start learn task.\n");
            free(args);
            g_ir_rx_learn_task = NULL;
            return;
        }
        glog("IR learn task started; use 'stop' to cancel.\n");
        return;
    }

    if (strcmp(sub, "dazzler") == 0) {
        if (argc >= 3 && strcmp(argv[2], "stop") == 0) {
            if (infrared_manager_dazzler_is_active()) {
                infrared_manager_dazzler_stop();
                glog("IR_DAZZLER:STOPPING\n");
            } else {
                glog("IR_DAZZLER:NOT_RUNNING\n");
            }
            return;
        }
        if (infrared_manager_dazzler_is_active()) {
            glog("IR_DAZZLER:ALREADY_RUNNING\n");
            return;
        }
        if (infrared_manager_dazzler_start()) {
            glog("IR_DAZZLER:STARTED\n");
        } else {
            glog("IR_DAZZLER:FAILED\n");
        }
        return;
    }

    glog("Unknown ir subcommand: %s\n", sub);
}

bool cmd_ir_stop_universal_send(void) {
    if (g_ir_universal_send_task != NULL) {
        g_ir_universal_send_cancel = true;
        return true;
    }
    return false;
}
