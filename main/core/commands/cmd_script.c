// GhostScript CLI command.

#include "core/commands.h"

#if CONFIG_ENABLE_GHOSTSCRIPT

#include "core/glog.h"
#include "managers/ghostscript_manager.h"
#include "managers/ghostscript_runtime.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CONFIG_WITH_SCREEN
#include "managers/display_manager.h"
#include "managers/views/ghostscript_runner_view.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define SCRIPT_CLI_MAX_ENTRIES 32
#define SCRIPT_CLI_MAX_DEPTH 4
#define SCRIPT_CLI_TASK_STACK 8192

static bool script_cli_join_path(char *out, size_t out_len, const char *base, const char *name) {
    if (!out || !base || !name) return false;
    int n = snprintf(out, out_len, "%s/%s", base, name);
    return n > 0 && (size_t)n < out_len;
}

static bool script_cli_has_manifest(const char *path) {
    char manifest_path[GHOSTSCRIPT_PATH_MAX];
    struct stat st;
    return script_cli_join_path(manifest_path, sizeof(manifest_path), path, "manifest.json") &&
           stat(manifest_path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static void script_cli_collect(const char *dir, int depth,
                               char (*paths)[GHOSTSCRIPT_PATH_MAX], size_t *count) {
    if (!dir || !paths || !count || *count >= SCRIPT_CLI_MAX_ENTRIES || depth > SCRIPT_CLI_MAX_DEPTH) return;
    DIR *directory = opendir(dir);
    if (!directory) return;

    struct dirent *entry;
    while (*count < SCRIPT_CLI_MAX_ENTRIES && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[GHOSTSCRIPT_PATH_MAX];
        struct stat st;
        if (!script_cli_join_path(path, sizeof(path), dir, entry->d_name) || stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (script_cli_has_manifest(path)) {
                snprintf(paths[(*count)++], GHOSTSCRIPT_PATH_MAX, "%s", path);
            } else {
                script_cli_collect(path, depth + 1, paths, count);
            }
        } else if (ghostscript_manager_is_script_file(entry->d_name)) {
            snprintf(paths[(*count)++], GHOSTSCRIPT_PATH_MAX, "%s", path);
        }
    }
    closedir(directory);
}

static int script_cli_compare_paths(const void *left, const void *right) {
    return strcmp((const char *)left, (const char *)right);
}

static bool script_cli_get_paths(char (*paths)[GHOSTSCRIPT_PATH_MAX], size_t *count) {
    if (count) *count = 0;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) {
        glog("GhostScript: %s\n", ghostscript_manager_last_error());
        return false;
    }
    /* Keep directory setup nested in this mount to avoid an extra display/SD handoff. */
    ghostscript_manager_init();
    size_t found = 0;
    script_cli_collect(GHOSTSCRIPT_ROOT_DIR, 0, paths, &found);
    ghostscript_manager_sd_end(display_was_suspended);
    qsort(paths, found, sizeof(*paths), script_cli_compare_paths);
    if (count) *count = found;
    return true;
}

#ifndef CONFIG_WITH_SCREEN
typedef struct {
    char path[GHOSTSCRIPT_PATH_MAX];
} script_cli_task_args_t;

static TaskHandle_t s_script_cli_task;
static SemaphoreHandle_t s_script_cli_mutex;
static ghostscript_runtime_t *s_script_cli_runtime;

static const char *script_cli_state_name(ghostscript_state_t state) {
    switch (state) {
        case GHOSTSCRIPT_STATE_LOADED: return "starting";
        case GHOSTSCRIPT_STATE_RUNNING: return "running";
        case GHOSTSCRIPT_STATE_DONE: return "completed";
        case GHOSTSCRIPT_STATE_FAILED: return "failed";
        case GHOSTSCRIPT_STATE_STOPPED: return "stopped";
        default: return "idle";
    }
}

static void script_cli_print(const char *text, void *user) {
    (void)user;
    if (text) glog("[GhostScript] %s", text);
}

static void script_cli_set_title(const char *title, void *user) {
    (void)user;
    if (title) glog("GhostScript: %s\n", title);
}

static void script_cli_task(void *arg) {
    script_cli_task_args_t *task_args = (script_cli_task_args_t *)arg;
    ghostscript_manifest_t manifest;
    ghostscript_runtime_t *runtime = NULL;
    bool loaded = false;
    if (task_args && task_args->path[0]) {
        if (ghostscript_manager_is_script_file(task_args->path)) {
            loaded = ghostscript_manager_make_single_file_manifest(task_args->path, &manifest);
            if (!loaded) loaded = ghostscript_manager_load_manifest(task_args->path, &manifest);
        } else {
            loaded = ghostscript_manager_load_manifest(task_args->path, &manifest);
        }
    }
    free(task_args);
    task_args = NULL;
    if (!loaded) {
        glog("GhostScript load failed: %s\n", ghostscript_manager_last_error());
        goto done;
    }

    ghostscript_runtime_hooks_t hooks = { .print = script_cli_print, .set_title = script_cli_set_title };
    runtime = ghostscript_runtime_create(&manifest, &hooks);
    if (!runtime) {
        glog("GhostScript: failed to create runtime\n");
        goto done;
    }

    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    s_script_cli_runtime = runtime;
    bool started = ghostscript_runtime_start(runtime);
    xSemaphoreGive(s_script_cli_mutex);
    if (started) {
        while (true) {
            xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
            if (ghostscript_runtime_state(runtime) != GHOSTSCRIPT_STATE_RUNNING) {
                xSemaphoreGive(s_script_cli_mutex);
                break;
            }
            ghostscript_runtime_tick(runtime, 100);
            xSemaphoreGive(s_script_cli_mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    ghostscript_state_t state = ghostscript_runtime_state(runtime);
    s_script_cli_runtime = NULL;
    xSemaphoreGive(s_script_cli_mutex);
    if (state == GHOSTSCRIPT_STATE_FAILED) {
        glog("GhostScript failed: %s\n", ghostscript_runtime_error(runtime));
        ghostscript_manager_record_failure(ghostscript_runtime_manifest(runtime), ghostscript_runtime_error(runtime));
    } else if (state == GHOSTSCRIPT_STATE_DONE) {
        glog("GhostScript completed\n");
        ghostscript_manager_record_clean_exit(ghostscript_runtime_manifest(runtime));
    }

    ghostscript_runtime_destroy(runtime);

done:
    free(task_args);
    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    s_script_cli_task = NULL;
    xSemaphoreGive(s_script_cli_mutex);
    vTaskDeleteWithCaps(NULL);
}

static bool script_cli_start_headless(const char *path) {
    if (!s_script_cli_mutex) s_script_cli_mutex = xSemaphoreCreateMutex();
    if (!s_script_cli_mutex) return false;
    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    if (s_script_cli_task) {
        xSemaphoreGive(s_script_cli_mutex);
        glog("GhostScript is already running\n");
        return false;
    }
    script_cli_task_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        xSemaphoreGive(s_script_cli_mutex);
        return false;
    }
    snprintf(args->path, sizeof(args->path), "%s", path);
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreateWithCaps(script_cli_task, "gs_cli", SCRIPT_CLI_TASK_STACK,
                                             args, 5, &task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        created = xTaskCreateWithCaps(script_cli_task, "gs_cli", SCRIPT_CLI_TASK_STACK,
                                      args, 5, &task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created == pdPASS) s_script_cli_task = task;
    xSemaphoreGive(s_script_cli_mutex);
    if (created != pdPASS) free(args);
    return created == pdPASS;
}

static void script_cli_status_headless(void) {
    if (!s_script_cli_mutex) {
        glog("GhostScript is idle\n");
        return;
    }
    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    if (!s_script_cli_task) {
        glog("GhostScript is idle\n");
    } else if (!s_script_cli_runtime) {
        glog("GhostScript is starting\n");
    } else {
        glog("GhostScript is %s\n", script_cli_state_name(ghostscript_runtime_state(s_script_cli_runtime)));
    }
    xSemaphoreGive(s_script_cli_mutex);
}

static void script_cli_stop_headless(void) {
    if (!s_script_cli_mutex) {
        glog("No GhostScript is running\n");
        return;
    }
    xSemaphoreTake(s_script_cli_mutex, portMAX_DELAY);
    bool active = s_script_cli_runtime &&
                  ghostscript_runtime_state(s_script_cli_runtime) == GHOSTSCRIPT_STATE_RUNNING;
    if (active) ghostscript_runtime_stop(s_script_cli_runtime);
    xSemaphoreGive(s_script_cli_mutex);
    glog(active ? "Stopping GhostScript\n" : "No GhostScript is running\n");
}
#endif

void handle_script_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: script <list|run|status|stop>\n");
        glog("  script list        - List runnable GhostScripts\n");
        glog("  script run <index> - Launch a listed GhostScript\n");
        glog("  script status      - Show the active GhostScript state\n");
        glog("  script stop        - Stop the active GhostScript\n");
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
#ifdef CONFIG_WITH_SCREEN
        glog(ghostscript_runner_is_script_active() ? "GhostScript is active\n" : "GhostScript is idle\n");
#else
        script_cli_status_headless();
#endif
        return;
    }
    if (strcmp(argv[1], "stop") == 0) {
#ifdef CONFIG_WITH_SCREEN
        glog(ghostscript_runner_stop_script() ? "Stopping GhostScript\n" : "No GhostScript is running\n");
#else
        script_cli_stop_headless();
#endif
        return;
    }
    if (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "run") != 0) {
        glog("Unknown script command: %s\n", argv[1]);
        return;
    }

    char (*paths)[GHOSTSCRIPT_PATH_MAX] = heap_caps_calloc_prefer(
        SCRIPT_CLI_MAX_ENTRIES, GHOSTSCRIPT_PATH_MAX, 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!paths) {
        glog("GhostScript: out of memory\n");
        return;
    }
    size_t count = 0;
    if (!script_cli_get_paths(paths, &count)) {
        heap_caps_free(paths);
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (count == 0) {
            glog("No runnable GhostScripts found in %s\n", GHOSTSCRIPT_ROOT_DIR);
        } else {
            size_t root_len = strlen(GHOSTSCRIPT_ROOT_DIR);
            glog("GhostScripts (%u):\n", (unsigned)count);
            for (size_t i = 0; i < count; ++i) {
                const char *relative_path = paths[i] + root_len;
                if (*relative_path == '/') ++relative_path;
                glog("  [%u] %s\n", (unsigned)i, relative_path);
            }
            if (count == SCRIPT_CLI_MAX_ENTRIES) glog("  List limited to %u entries\n", SCRIPT_CLI_MAX_ENTRIES);
        }
    } else if (strcmp(argv[1], "run") == 0) {
        char *end = NULL;
        long index = argc >= 3 ? strtol(argv[2], &end, 10) : -1;
        if (argc < 3 || !end || *end != '\0' || index < 0 || (size_t)index >= count) {
            glog("Usage: script run <index> (use 'script list' first)\n");
        } else {
#ifdef CONFIG_WITH_SCREEN
            ghostscript_runner_set_script(paths[index]);
            display_manager_switch_view(&ghostscript_runner_view);
            glog("Launching GhostScript [%ld]\n", index);
#else
            if (script_cli_start_headless(paths[index])) glog("Launching GhostScript [%ld]\n", index);
            else glog("GhostScript launch failed\n");
#endif
        }
    }
    heap_caps_free(paths);
}

#endif
