// system_manager.h

#ifndef SYSTEM_MANAGER_H
#define SYSTEM_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

typedef struct ManagedTask {
  TaskHandle_t task_handle;
  char task_name[16];
  UBaseType_t priority;
  void (*on_task_complete)(
      const char *task_name); // Callback when task completes
  struct ManagedTask *next;
} ManagedTask;

// Initialize the System Manager
void system_manager_init();

// Create a new task
bool system_manager_create_task(void (*task_function)(void *),
                                const char *task_name, uint32_t stack_size,
                                UBaseType_t priority,
                                void (*on_task_complete)(const char *));

// Remove an existing task by name
bool system_manager_remove_task(const char *task_name);

// Suspend a task
bool system_manager_suspend_task(const char *task_name);

// Resume a suspended task
bool system_manager_resume_task(const char *task_name);

// Change a task’s priority
bool system_manager_set_task_priority(const char *task_name,
                                      UBaseType_t new_priority);

// Print the list of all tasks
void system_manager_list_tasks();

// Create a task with PSRAM-preferred stack allocation.
// Tries PSRAM first, falls back to internal RAM. Always uses xTaskCreateStatic
// so the caller gets a valid handle for xTaskNotify etc.
#include "esp_heap_caps.h"

static inline BaseType_t xTaskCreate_psram(
    TaskFunction_t fn, const char *name, uint32_t stack_bytes,
    void *arg, UBaseType_t pri, TaskHandle_t *handle_out)
{
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    StackType_t *stack = (StackType_t *)heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack) {
        StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (tcb) {
            TaskHandle_t h = xTaskCreateStatic(fn, name, stack_bytes, arg, pri, stack, tcb);
            if (h) { if (handle_out) *handle_out = h; return pdPASS; }
            heap_caps_free(stack); heap_caps_free(tcb);
            // fall through to internal
        } else {
            heap_caps_free(stack);
        }
    }
#endif
    return xTaskCreate(fn, name, stack_bytes, arg, pri, handle_out);
}

#endif // SYSTEM_MANAGER_H