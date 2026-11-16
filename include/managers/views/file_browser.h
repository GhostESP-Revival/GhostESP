#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define FILE_BROWSER_PATH_MAX 256

/* Directory paths carry a trailing slash, so UI callbacks need no SD access
 * to distinguish folders from files after a shared-SPI card is unmounted. */
static inline bool file_browser_is_dir(const char *path) {
    size_t len = strlen(path);
    return len && path[len - 1] == '/';
}

static inline const char *file_browser_name(const char *path) {
    size_t len = strlen(path);
    if (len && path[len - 1] == '/') len--;
    while (len && path[len - 1] != '/') len--;
    return path + len; /* Folder labels retain the slash. */
}

static inline bool file_browser_parent(char *path, const char *root) {
    size_t len = strlen(root);
    if (strncmp(path, root, len) || path[len] != '/') return false;
    char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) < len) return false;
    *slash = '\0';
    return true;
}

static inline bool file_browser_enter(char *dir, size_t capacity, const char *path) {
    size_t len = strlen(path);
    if (!file_browser_is_dir(path) || len >= capacity) return false;
    memmove(dir, path, len - 1);
    dir[len - 1] = '\0';
    return true;
}

static inline void file_browser_free(char **paths, size_t count) {
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Caller owns the SD mount. Scan folders first, then matching regular files.
 * A finite limit keeps paged browsers bounded regardless of directory size.
 * On failure no partial list is returned. */
static inline bool file_browser_read(const char *dir, bool (*accept)(const char *),
                                     size_t offset, size_t limit, char ***paths,
                                     size_t *count, bool *has_next) {
    *paths = NULL;
    *count = 0;
    if (has_next) *has_next = false;
    DIR *d = opendir(dir);
    if (!d) return false;
    size_t seen = 0;
    size_t capacity = 0;
    for (int folders = 1; folders >= 0; folders--) {
        rewinddir(d);
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[FILE_BROWSER_PATH_MAX];
            int len = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            if (len < 0 || (size_t)len >= sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) != 0) continue;
            bool is_dir = S_ISDIR(st.st_mode);
            if (is_dir != (folders != 0)) continue;
            if (!is_dir && (!S_ISREG(st.st_mode) || !accept(entry->d_name))) continue;
            if (is_dir && (size_t)len + 1 >= sizeof(path)) continue;
            if (seen++ < offset) continue;
            if (*count == limit) {
                if (has_next) *has_next = true;
                closedir(d);
                return true;
            }
            if (is_dir) {
                path[len++] = '/';
                path[len] = '\0';
            }
            if (*count == capacity) {
                size_t next = capacity ? capacity * 2 : 8;
                if (next > limit) next = limit;
                char **grown = realloc(*paths, next * sizeof(char *));
                if (!grown) goto failed;
                *paths = grown;
                capacity = next;
            }
            char *copy = strdup(path);
            if (!copy) goto failed;
            (*paths)[(*count)++] = copy;
        }
    }
    closedir(d);
    return true;
failed:
    closedir(d);
    file_browser_free(*paths, *count);
    *paths = NULL;
    *count = 0;
    return false;
}
