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

#endif /* GLOG_H */


