#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record that the previous boot ended in a crash (a coredump was
 * found in flash during boot). A popup is shown once the UI is up,
 * similar to the Flipper Zero's post-crash notice.
 *
 * Thread-safe to call from any task; only the reason string is copied.
 * Pass NULL or an empty string when the reason could not be decoded.
 */
void crash_reporter_set_boot_crash(const char *panic_reason);

/**
 * @brief Schedule the boot-crash popup once the display is initialized.
 *
 * Call once from app_main after the display manager is initialized
 * (CONFIG_WITH_SCREEN builds).
 */
void crash_reporter_init(void);

/* Returns true while the crash popup owns all local input. */
bool crash_reporter_handle_input(const void *event);

#ifdef __cplusplus
}
#endif
