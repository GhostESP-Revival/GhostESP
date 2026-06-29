#pragma once

#include "managers/ghostscript_manager.h"
#include "managers/display_manager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GHOSTSCRIPT_STATE_EMPTY = 0,
    GHOSTSCRIPT_STATE_LOADED,
    GHOSTSCRIPT_STATE_RUNNING,
    GHOSTSCRIPT_STATE_DONE,
    GHOSTSCRIPT_STATE_FAILED,
    GHOSTSCRIPT_STATE_STOPPED,
} ghostscript_state_t;

typedef void (*ghostscript_output_fn_t)(const char *text, void *user);
typedef void (*ghostscript_title_fn_t)(const char *title, void *user);

typedef struct ghostscript_runtime ghostscript_runtime_t;

typedef struct {
    ghostscript_output_fn_t print;
    ghostscript_title_fn_t set_title;
    void *user;
} ghostscript_runtime_hooks_t;

ghostscript_runtime_t *ghostscript_runtime_create(const ghostscript_manifest_t *manifest, const ghostscript_runtime_hooks_t *hooks);
bool ghostscript_runtime_start(ghostscript_runtime_t *rt);
void ghostscript_runtime_stop(ghostscript_runtime_t *rt);
void ghostscript_runtime_destroy(ghostscript_runtime_t *rt);
void ghostscript_runtime_tick(ghostscript_runtime_t *rt, uint32_t elapsed_ms);
void ghostscript_runtime_input(ghostscript_runtime_t *rt, const InputEvent *event);
ghostscript_state_t ghostscript_runtime_state(const ghostscript_runtime_t *rt);
const char *ghostscript_runtime_error(const ghostscript_runtime_t *rt);
size_t ghostscript_runtime_memory_used(const ghostscript_runtime_t *rt);
size_t ghostscript_runtime_memory_limit(const ghostscript_runtime_t *rt);
const ghostscript_manifest_t *ghostscript_runtime_manifest(const ghostscript_runtime_t *rt);
void ghostscript_runtime_dispatch_event(ghostscript_runtime_t *rt, const char *name, const char *value);
void ghostscript_emit_event(const char *name, const char *value);
void ghostscript_emit_event_escaped(const char *name, const char *value);
bool ghostscript_runtime_mark_ble_seen(const uint8_t mac[6]);
void ghostscript_runtime_reset_ble_seen(void);
bool ghostscript_runtime_match_oui_prefix(const ghostscript_runtime_t *rt, const char *mac);
void ghostscript_runtime_set_active(ghostscript_runtime_t *rt);
ghostscript_runtime_t *ghostscript_runtime_get_active(void);

#ifdef __cplusplus
}
#endif
