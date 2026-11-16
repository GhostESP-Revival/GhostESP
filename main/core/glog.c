/* haha glog */
#include "core/glog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include "core/esp_comm_manager.h"
#include "managers/ap_manager.h"

#define GLOG_BUF_SIZE 512
#define GLOG_DEFER_MAX 4

static SemaphoreHandle_t s_glog_mutex;
static volatile int s_glog_defer = 0;
static char *s_glog_q[GLOG_DEFER_MAX];
static uint8_t s_q_head = 0, s_q_tail = 0, s_q_count = 0;
static glog_capture_fn_t s_glog_capture_fn;
static void *s_glog_capture_user;

/* Serializes physical console output. printf writes straight to the UART
 * FIFO, bypassing the UART driver's TX ring buffer; without this lock a glog
 * line can land in the middle of a still-draining uart_write_bytes() chunk
 * (e.g. a framed CSV/PCAP stream) and corrupt it. */
static SemaphoreHandle_t s_glog_out_mutex;

static inline void glog_out_lock(void) {
    if (!s_glog_out_mutex) {
        static portMUX_TYPE out_init_mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&out_init_mux);
        if (!s_glog_out_mutex) {
            s_glog_out_mutex = new_mutex;
            new_mutex = NULL;
        }
        portEXIT_CRITICAL(&out_init_mux);
        if (new_mutex) vSemaphoreDelete(new_mutex);
    }
    if (s_glog_out_mutex) xSemaphoreTake(s_glog_out_mutex, portMAX_DELAY);
}

static inline void glog_out_unlock(void) {
    if (s_glog_out_mutex) xSemaphoreGive(s_glog_out_mutex);
}

static inline void glog_lock(void) {
    if (!s_glog_mutex) {
        static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&init_mux);
        if (!s_glog_mutex) {
            s_glog_mutex = new_mutex;
            new_mutex = NULL;
        }
        portEXIT_CRITICAL(&init_mux);
        if (new_mutex) vSemaphoreDelete(new_mutex);
    }
    if (s_glog_mutex) xSemaphoreTake(s_glog_mutex, portMAX_DELAY);
}

static inline void glog_unlock(void) {
    if (s_glog_mutex) xSemaphoreGive(s_glog_mutex);
}

static inline void glog_emit(const char *buf) {
    glog_out_lock();
    printf("%s", buf);
    glog_out_unlock();
    if (esp_comm_manager_should_forward_output()) {
        esp_comm_manager_send_response((const uint8_t *)buf, strlen(buf));
    }
    ap_manager_add_log(buf);
}

void glog(const char *fmt, ...) {
    if (!fmt) return;

    glog_lock();

    char buf[GLOG_BUF_SIZE];

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) {
        glog_unlock();
        return;
    }

    if (written >= (int)sizeof(buf)) {
        written = (int)sizeof(buf) - 1;
    }

    if (written == 0 || buf[written - 1] != '\n') {
        if (written < (int)sizeof(buf) - 1) {
            buf[written++] = '\n';
            buf[written] = '\0';
        } else {
            buf[sizeof(buf) - 2] = '\n';
            buf[sizeof(buf) - 1] = '\0';
            written = (int)sizeof(buf) - 1;
        }
    }

    if (s_glog_defer) {
        if (s_q_count == GLOG_DEFER_MAX) {
            free(s_glog_q[s_q_head]);
            s_glog_q[s_q_head] = NULL;
            s_q_head = (s_q_head + 1) % GLOG_DEFER_MAX;
            s_q_count--;
        }
        char *queued = malloc((size_t)written + 1);
        if (!queued) {
            glog_unlock();
            glog_emit(buf);
            return;
        }
        memcpy(queued, buf, (size_t)written + 1);
        s_glog_q[s_q_tail] = queued;
        s_q_tail = (s_q_tail + 1) % GLOG_DEFER_MAX;
        s_q_count++;
        glog_unlock();
        return;
    }

    glog_unlock();

    glog_emit(buf);

    glog_capture_fn_t cb = s_glog_capture_fn;
    void *cu = s_glog_capture_user;
    if (cb) cb(buf, cu);
}

void glog_set_capture(glog_capture_fn_t fn, void *user) {
    glog_lock();
    s_glog_capture_fn = fn;
    s_glog_capture_user = user;
    glog_unlock();
}

void glog_set_defer(int on) {
    glog_lock();
    s_glog_defer = (on != 0);
    glog_unlock();
}

void glog_flush_deferred(void) {
    for (;;) {
        glog_lock();
        if (s_q_count == 0) {
            glog_unlock();
            break;
        }
        char *out = s_glog_q[s_q_head];
        s_glog_q[s_q_head] = NULL;
        s_q_head = (s_q_head + 1) % GLOG_DEFER_MAX;
        s_q_count--;
        glog_unlock();
        glog_emit(out);
        free(out);
    }
}

void glog_console_hold(void) {
    /* Queue messages instead of printing them, then take the output lock so
     * anything that raced past the defer check still blocks until the stream
     * writer is done. */
    glog_set_defer(1);
    glog_out_lock();
    // printf/ESP_LOG callers do not take the glog mutex. Hold their stdio
    // stream lock too, and drain text buffered before the binary frame.
    flockfile(stdout);
    fflush(stdout);
}

void glog_console_release(void) {
    /* Callers must have waited for the UART TX to drain (uart_wait_tx_done)
     * before releasing: deferred text is emitted right here and must not land
     * in the middle of the chunk that was just streamed. */
    funlockfile(stdout);
    glog_out_unlock();
    glog_set_defer(0);
    glog_flush_deferred();
}


