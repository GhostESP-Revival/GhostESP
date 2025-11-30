#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <stddef.h>

// Per-animation entry points (implemented in separate files)
void status_anim_game_of_life_reset(void);
void status_anim_game_of_life_step(TickType_t now, int frame, const StatusAnimGfx *gfx);

void status_anim_ghost_reset(void);
void status_anim_ghost_step(TickType_t now, int frame, const StatusAnimGfx *gfx);

void status_anim_starfield_reset(void);
void status_anim_starfield_step(TickType_t now, int frame, const StatusAnimGfx *gfx);

void status_anim_hud_reset(void);
void status_anim_hud_step(TickType_t now, int frame, const StatusAnimGfx *gfx);

void status_anim_matrix_reset(void);
void status_anim_matrix_step(TickType_t now, int frame, const StatusAnimGfx *gfx);

typedef struct {
    void (*reset)(void);
    void (*step)(TickType_t now, int frame, const StatusAnimGfx *gfx);
} AnimOps;

static const AnimOps s_anim_ops[] = {
    { status_anim_game_of_life_reset, status_anim_game_of_life_step },
    { status_anim_ghost_reset,        status_anim_ghost_step        },
    { status_anim_starfield_reset,    status_anim_starfield_step    },
    { status_anim_hud_reset,          status_anim_hud_step          },
    { status_anim_matrix_reset,       status_anim_matrix_step       },
};

static IdleAnimation s_current_anim = (IdleAnimation)(-1);

static const AnimOps *get_ops_for_anim(IdleAnimation anim)
{
    int idx = (int)anim;
    if (idx < 0 || idx >= (int)(sizeof(s_anim_ops) / (int)sizeof(s_anim_ops[0]))) {
        return NULL;
    }
    return &s_anim_ops[idx];
}

void status_display_animations_reset(void)
{
    s_current_anim = (IdleAnimation)(-1);
    for (size_t i = 0; i < sizeof(s_anim_ops) / sizeof(s_anim_ops[0]); ++i) {
        if (s_anim_ops[i].reset) {
            s_anim_ops[i].reset();
        }
    }
}

void status_display_animations_step(IdleAnimation anim, TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    const AnimOps *ops = get_ops_for_anim(anim);
    if (!ops || !ops->step) {
        return;
    }
    if (anim != s_current_anim) {
        s_current_anim = anim;
        if (ops->reset) {
            ops->reset();
        }
    }
    ops->step(now, frame, gfx);
}

#endif // CONFIG_WITH_STATUS_DISPLAY
