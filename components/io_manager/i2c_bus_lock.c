#include "i2c_bus_lock.h"
#include <stddef.h>
#include "driver/i2c.h"

#if defined(__has_include)
#  if __has_include("freertos/FreeRTOS.h")
#    include "freertos/FreeRTOS.h"
#    include "freertos/semphr.h"
#    define I2C_BUS_LOCK_HAS_FREERTOS 1
#  endif
#endif

#ifndef I2C_BUS_LOCK_HAS_FREERTOS
// minimal stubs for static analysis environments without FreeRTOS include paths
typedef void* SemaphoreHandle_t;
typedef unsigned int TickType_t;
#ifndef portMAX_DELAY
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#endif
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline void vSemaphoreDelete(SemaphoreHandle_t h) { (void)h; }
static inline int xSemaphoreTake(SemaphoreHandle_t h, TickType_t to) { (void)h; (void)to; return 1; }
static inline void xSemaphoreGive(SemaphoreHandle_t h) { (void)h; }
static inline TickType_t pdMS_TO_TICKS(int ms) { return (TickType_t)ms; }
#endif

// global shared mutex per I2C port (currently only port 0 is used, but keep array for future support)
static SemaphoreHandle_t s_i2c_mutexes[I2C_NUM_MAX] = { NULL };

static inline SemaphoreHandle_t get_mutex_for_port(int port)
{
    if (port < 0 || port >= I2C_NUM_MAX) {
        return NULL;
    }
    SemaphoreHandle_t *slot = &s_i2c_mutexes[port];
    if (*slot == NULL) {
        // lazy init; best-effort, caller tolerates NULL as no-op
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m != NULL) {
            // benign race: if two tasks create, we keep the first stored
            if (*slot == NULL) {
                *slot = m;
            } else {
                vSemaphoreDelete(m);
            }
        }
    }
    return *slot;
}

bool i2c_bus_lock(int port, int timeout_ms)
{
    SemaphoreHandle_t mtx = get_mutex_for_port(port);
    if (!mtx) return false; // locking not required or not available
    TickType_t fto = (timeout_ms <= 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
#ifdef I2C_BUS_LOCK_HAS_FREERTOS
    if (xSemaphoreTake(mtx, fto) == pdTRUE) return true;
#else
    if (xSemaphoreTake(mtx, fto) == 1) return true;
#endif
    return false;
}

void i2c_bus_unlock(int port)
{
    SemaphoreHandle_t mtx = get_mutex_for_port(port);
    if (!mtx) return;
    xSemaphoreGive(mtx);
}

void *i2c_bus_get_lock_handle(void)
{
    return (void*)s_i2c_mutexes;
}


