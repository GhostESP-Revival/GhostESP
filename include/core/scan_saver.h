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
    uint16_t write_count;
} scan_file_t;

#define SCAN_FILE_INIT { NULL, false, false, 0 }

esp_err_t scan_file_open(scan_file_t *sf, const char *prefix, const char *extension);
void scan_file_printf(scan_file_t *sf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void scan_file_close(scan_file_t *sf);

#endif
