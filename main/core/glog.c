/* haha glog */
#include "core/glog.h"

#include <stdarg.h>
#include <stdio.h>
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
static char s_glog_q[GLOG_DEFER_MAX][GLOG_BUF_SIZE];
static uint8_t s_q_head = 0, s_q_tail = 0, s_q_count = 0;

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
    printf("%s", buf);
    if (esp_comm_manager_is_remote_command()) {
        esp_comm_manager_send_response((const uint8_t *)buf, strlen(buf));
    }
    terminal_view_add_text(buf);
    ap_manager_add_log(buf);
}

void glog(const char *fmt, ...) {
    if (!fmt) return;

    glog_lock();

    static char buf[GLOG_BUF_SIZE];

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
            s_q_head = (s_q_head + 1) % GLOG_DEFER_MAX;
            s_q_count--;
        }
        memcpy(s_glog_q[s_q_tail], buf, (size_t)written + 1);
        s_q_tail = (s_q_tail + 1) % GLOG_DEFER_MAX;
        s_q_count++;
        glog_unlock();
        return;
    }
    glog_unlock();

    glog_emit(buf);
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
        static char out[GLOG_BUF_SIZE];
        memcpy(out, s_glog_q[s_q_head], GLOG_BUF_SIZE);
        s_q_head = (s_q_head + 1) % GLOG_DEFER_MAX;
        s_q_count--;
        glog_unlock();
        glog_emit(out);
    }
}


