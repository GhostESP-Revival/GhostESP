#include "sdkconfig.h"

#if !defined(CONFIG_ENABLE_GHOSTSCRIPT) || !CONFIG_ENABLE_GHOSTSCRIPT

#include "managers/ghostscript_runtime.h"

ghostscript_runtime_t *ghostscript_runtime_create(const ghostscript_manifest_t *manifest,
                                                   const ghostscript_runtime_hooks_t *hooks) {
    (void)manifest;
    (void)hooks;
    return NULL;
}

bool ghostscript_runtime_start(ghostscript_runtime_t *rt) { (void)rt; return false; }
void ghostscript_runtime_stop(ghostscript_runtime_t *rt) { (void)rt; }
void ghostscript_runtime_destroy(ghostscript_runtime_t *rt) { (void)rt; }
void ghostscript_runtime_tick(ghostscript_runtime_t *rt, uint32_t elapsed_ms) { (void)rt; (void)elapsed_ms; }
void ghostscript_runtime_input(ghostscript_runtime_t *rt, const InputEvent *event) { (void)rt; (void)event; }
ghostscript_state_t ghostscript_runtime_state(const ghostscript_runtime_t *rt) { (void)rt; return GHOSTSCRIPT_STATE_EMPTY; }
const char *ghostscript_runtime_error(const ghostscript_runtime_t *rt) { (void)rt; return "GhostScript is disabled"; }
size_t ghostscript_runtime_memory_used(const ghostscript_runtime_t *rt) { (void)rt; return 0; }
size_t ghostscript_runtime_memory_limit(const ghostscript_runtime_t *rt) { (void)rt; return 0; }
const ghostscript_manifest_t *ghostscript_runtime_manifest(const ghostscript_runtime_t *rt) { (void)rt; return NULL; }
void ghostscript_runtime_dispatch_event(ghostscript_runtime_t *rt, const char *name, const char *value) {
    (void)rt;
    (void)name;
    (void)value;
}
void ghostscript_emit_event(const char *name, const char *value) { (void)name; (void)value; }
void ghostscript_emit_event_escaped(const char *name, const char *value) { (void)name; (void)value; }
bool ghostscript_runtime_mark_ble_seen(const uint8_t mac[6]) { (void)mac; return true; }
void ghostscript_runtime_reset_ble_seen(void) {}
bool ghostscript_runtime_match_oui_prefix(const ghostscript_runtime_t *rt, const char *mac) {
    (void)rt;
    (void)mac;
    return false;
}
void ghostscript_runtime_set_active(ghostscript_runtime_t *rt) { (void)rt; }
ghostscript_runtime_t *ghostscript_runtime_get_active(void) { return NULL; }

#endif
