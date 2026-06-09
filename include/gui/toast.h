#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOAST_INFO    0
#define TOAST_SUCCESS 1
#define TOAST_WARN    2
#define TOAST_ERROR   3

#define TOAST_DEFAULT_DURATION_MS 1500
#define TOAST_MAX_TEXT_LEN        63

void toast_show(const char *text, uint8_t type);
void toast_show_duration(const char *text, uint8_t type, uint16_t duration_ms);
void toast_dismiss(void);
void toast_cancel_all(void);
bool toast_is_visible(void);

#ifdef __cplusplus
}
#endif
