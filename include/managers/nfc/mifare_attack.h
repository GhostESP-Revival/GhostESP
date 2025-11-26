#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void (*on_phase)(int sector, int first_block, bool key_b, int total_keys);
    void (*on_cache_mode)(bool on);
    void (*on_paused)(bool on);
    bool (*should_cancel)(void);
    bool (*should_skip_dict)(void);
} mfc_attack_hooks_t;

static inline mfc_attack_hooks_t mfc_attack_hooks_noop(void) {
    mfc_attack_hooks_t h = {0};
    return h;
}
