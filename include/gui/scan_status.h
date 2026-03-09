#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scan_status_t scan_status_t;

scan_status_t *scan_status_create(const char *message);

void scan_status_update(scan_status_t *ss, const char *message);

void scan_status_set_subtext(scan_status_t *ss, const char *subtext);

void scan_status_set_progress(scan_status_t *ss, int current, int total);

void scan_status_close(scan_status_t *ss);

bool scan_status_is_active(const scan_status_t *ss);

#ifdef __cplusplus
}
#endif
