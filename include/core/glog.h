#ifndef GLOG_H
#define GLOG_H

#include <stddef.h>
#include "managers/views/terminal_screen.h"

/*
 * glog - lightweight global logger that writes to both stdout (printf)
 * and the terminal view (if available). Designed to be low-memory and
 * truncate long messages rather than allocate dynamic memory.
 */
void glog(const char *fmt, ...);

void glog_set_defer(int on);
void glog_flush_deferred(void);

/* Capture sink: when set, every glog line is also delivered to the callback.
 * Used by GhostScript to stream command output to scripts. The callback
 * runs on the emitting task; keep it fast. Pass NULL to disable. */
typedef void (*glog_capture_fn_t)(const char *line, void *user);
void glog_set_capture(glog_capture_fn_t fn, void *user);

#endif /* GLOG_H */


