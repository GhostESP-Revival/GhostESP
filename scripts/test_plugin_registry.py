"""Host regression for SD app discovery; run with Python and GCC on PATH.

Runs the production directory scan and ID lookup against temporary SD folders.
Manifest validation and cache freshness are stubbed; no ESP hardware is needed.
"""
from pathlib import Path
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "main/managers/plugin_manager.c"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
// Exit directly on failure; Windows assert() can open a blocking crash dialog.
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "Failed: %s at line %d\n", #condition, __LINE__); \
    exit(1); \
} } while (0)
#define PLUGIN_APP_PATH_MAX 384
#define PLUGIN_APP_REGISTRY_CAPACITY 4
#define ESP_LOGW(...) ((void)0)
typedef struct {
    char id[32], name[64], base_path[PLUGIN_APP_PATH_MAX], error[96];
} plugin_app_manifest_t;
static plugin_app_manifest_t storage[PLUGIN_APP_REGISTRY_CAPACITY];
static plugin_app_manifest_t *s_apps = storage;
static int s_app_count;
static char installed[PLUGIN_APP_PATH_MAX], cache[PLUGIN_APP_PATH_MAX];
#define PLUGIN_APPS_DIR installed
#define PLUGIN_APP_CACHE_DIR cache
static void set_error(const char *fmt, const char *path) {
    (void)fmt; (void)path;
}
static bool join_path(char *out, size_t size, const char *base, const char *name) {
    int written = snprintf(out, size, "%s/%s", base, name);
    return written >= 0 && (size_t)written < size;
}
static bool cache_source_current(const char *path) {
    char marker[PLUGIN_APP_PATH_MAX];
    struct stat st;
    return join_path(marker, sizeof(marker), path, ".current") && stat(marker, &st) == 0;
}
static bool parse_manifest(const char *path, plugin_app_manifest_t *app) {
    char manifest[PLUGIN_APP_PATH_MAX];
    if (!join_path(manifest, sizeof(manifest), path, "manifest.txt")) return false;
    FILE *f = fopen(manifest, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%31s %63s", app->id, app->name) == 2;
    fclose(f);
    snprintf(app->base_path, sizeof(app->base_path), "%s", path);
    return ok;
}
'''

MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    snprintf(installed, sizeof(installed), "%s", argv[1]);
    snprintf(cache, sizeof(cache), "%s", argv[2]);
    for (int cycle = 0; cycle < 3; ++cycle) {
        assert(scan() == 3);
        const plugin_app_manifest_t *app = plugin_manager_find("snake");
        assert(app && strcmp(app->name, "InstalledSnake") == 0);
        assert(strncmp(app->base_path, installed, strlen(installed)) == 0);
        assert(plugin_manager_find("pong"));
        // Equal display names are distinct apps when their IDs differ.
        assert(plugin_manager_find("other"));
        assert(!plugin_manager_find("stale"));
        assert(!plugin_manager_find("invalid"));
    }
    // With the installed copy removed, its current cache is still discoverable.
    char path[PLUGIN_APP_PATH_MAX];
    assert(join_path(path, sizeof(path), installed, "snake/manifest.txt"));
    assert(remove(path) == 0);
    assert(scan() == 3);
    const plugin_app_manifest_t *app = plugin_manager_find("snake");
    assert(app && strncmp(app->base_path, cache, strlen(cache)) == 0);
    int snakes = 0;
    for (int i = 0; i < s_app_count; ++i) snakes += strcmp(s_apps[i].id, "snake") == 0;
    assert(snakes == 1);
    puts("SD registry: installed/cache duplicates, cache-only duplicates, distinct IDs, stale/invalid entries and reloads passed");
    return 0;
}
'''


def extract_function(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def main():
    source = SOURCE.read_text(encoding="utf-8")
    scan_start = source.index("    const char *scan_dirs[] =")
    scan_end = source.index("    /* readdir order", scan_start)
    scan = "int scan(void) {\n s_app_count = 0; memset(storage, 0, sizeof(storage));\n"
    scan += source[scan_start:scan_end] + " return s_app_count;\n}\n"
    find = extract_function(source, "const plugin_app_manifest_t *plugin_manager_find(")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        installed, cache = work / "apps", work / "app_cache"
        installed.mkdir()
        cache.mkdir()

        def fixture(base, folder, manifest, current=False):
            app = base / folder
            app.mkdir()
            (app / "manifest.txt").write_text(manifest, encoding="utf-8")
            if current:
                (app / ".current").touch()

        fixture(installed, "snake", "snake InstalledSnake")
        fixture(cache, "snake-package", "snake CachedSnake", True)
        fixture(cache, "snake-second-package", "snake CachedSnake", True)
        fixture(cache, "pong", "pong SameName", True)
        fixture(cache, "other", "other SameName", True)
        fixture(cache, "stale", "stale StaleApp")
        fixture(installed, "invalid", "invalid")
        (installed / "snake.gapp").touch()
        (installed / ".hidden").mkdir()
        harness, exe = work / "registry.c", work / "registry.exe"
        harness.write_text(HARNESS + find + scan + MAIN, encoding="utf-8")
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(harness), "-o", str(exe)], check=True)
        subprocess.run([str(exe), installed.as_posix(), cache.as_posix()], check=True)


if __name__ == "__main__":
    main()
