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

/* Console hold/release for wire-atomic binary streams (CSV/PCAP over UART).
 *
 * printf-based console output writes straight to the UART FIFO and can interleave
 * with a still-draining uart_write_bytes() chunk, corrupting the data and the
 * [BUF/BEGIN]/[BUF/CLOSE] framing host tools rely on. Bracket such chunks with
 * glog_console_hold()/glog_console_release(); deferred and concurrent glog
 * lines and stdout writers then wait until the stream is idle again. Callers must finish with
 * uart_wait_tx_done() before glog_console_release() so the chunk has left the
 * wire before any text is emitted. Not ISR-safe.
 *
 *   glog_console_hold();
 *   uart_write_bytes(...);
 *   uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
 *   glog_console_release();
 */
void glog_console_hold(void);
void glog_console_release(void);

/* Capture sink: when set, every glog line is also delivered to the callback.
 * Used by GhostScript to stream command output to scripts. The callback
 * runs on the emitting task; keep it fast. Pass NULL to disable. */
typedef void (*glog_capture_fn_t)(const char *line, void *user);
void glog_set_capture(glog_capture_fn_t fn, void *user);

#endif /* GLOG_H */


