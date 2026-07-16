#ifndef SCAN_SAVER_H
#define SCAN_SAVER_H

#include "esp_err.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE *fp;
    bool jit_mounted;
    bool display_suspended;
    bool buffered;
    uint16_t write_count;
    char *buffer;
    size_t buffer_len;
    size_t buffer_capacity;
    char path[128];
} scan_file_t;

#define SCAN_FILE_INIT { \
    .fp = NULL, \
    .jit_mounted = false, \
    .display_suspended = false, \
    .buffered = false, \
    .write_count = 0, \
    .buffer = NULL, \
    .buffer_len = 0, \
    .buffer_capacity = 0, \
    .path = {0}, \
}

esp_err_t scan_file_open(scan_file_t *sf, const char *prefix, const char *extension);
void scan_file_printf(scan_file_t *sf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void scan_file_close(scan_file_t *sf);
bool scan_file_is_open(const scan_file_t *sf);

#endif
