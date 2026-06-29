#include "managers/ghostchi_mood.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    uint8_t energy;
    uint8_t happiness;
    uint8_t focus;
    uint8_t stress;
    ghostchi_mood_t override_mood;
    uint32_t override_until_ms;
    uint32_t last_event_ms;
    uint32_t last_decay_ms;
} ghostchi_mood_state_t;

static SemaphoreHandle_t s_mood_lock;
static ghostchi_mood_state_t s_mood;
static bool s_mood_ready;

static uint32_t mood_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint8_t clamp_score_int(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

static uint8_t approach_score(uint8_t value, uint8_t target, uint32_t amount) {
    if (value < target) {
        uint32_t next = value + amount;
        return next > target ? target : (uint8_t)next;
    }
    if (value > target) {
        return (value - target) < amount ? target : (uint8_t)(value - amount);
    }
    return value;
}

static void mood_ensure_init(void) {
    if (!s_mood_ready) {
        memset(&s_mood, 0, sizeof(s_mood));
        s_mood.energy = 65;
        s_mood.happiness = 55;
        s_mood.focus = 20;
        s_mood.stress = 10;
        s_mood.override_mood = GHOSTCHI_MOOD_NEUTRAL;
        s_mood.last_decay_ms = mood_now_ms();
        s_mood.last_event_ms = s_mood.last_decay_ms;
        s_mood_ready = true;
    }
    if (!s_mood_lock) {
        s_mood_lock = xSemaphoreCreateMutex();
    }
}

static void mood_decay_locked(uint32_t now) {
    if (s_mood.last_decay_ms == 0) {
        s_mood.last_decay_ms = now;
        return;
    }

    uint32_t elapsed_ms = now - s_mood.last_decay_ms;
    uint32_t ticks = elapsed_ms / 30000u;
    if (ticks == 0) return;
    if (ticks > 20u) ticks = 20u;

    s_mood.energy = approach_score(s_mood.energy, 40, ticks * 2u);
    s_mood.happiness = approach_score(s_mood.happiness, 55, ticks);
    s_mood.focus = approach_score(s_mood.focus, 20, ticks * 3u);
    s_mood.stress = approach_score(s_mood.stress, 10, ticks * 2u);
    s_mood.last_decay_ms += ticks * 30000u;
}

static ghostchi_mood_t mood_derive_locked(uint32_t now) {
    if (s_mood.override_until_ms != 0 && now < s_mood.override_until_ms) {
        return s_mood.override_mood;
    }
    s_mood.override_until_ms = 0;

    if (s_mood.stress >= 70) return GHOSTCHI_MOOD_ANGRY;
    if (s_mood.energy <= 15) return GHOSTCHI_MOOD_SLEEPY;
    if (s_mood.energy <= 32) return GHOSTCHI_MOOD_TIRED;
    if (s_mood.focus >= 70) return GHOSTCHI_MOOD_FOCUSED;
    if (s_mood.happiness >= 82) return GHOSTCHI_MOOD_LOVE;
    if (s_mood.happiness >= 65) return GHOSTCHI_MOOD_HAPPY;
    if (s_mood.focus >= 45) return GHOSTCHI_MOOD_FOCUSED;
    return GHOSTCHI_MOOD_NEUTRAL;
}

const char *ghostchi_mood_label(ghostchi_mood_t mood) {
    switch (mood) {
        case GHOSTCHI_MOOD_HAPPY:      return "happy";
        case GHOSTCHI_MOOD_LOVE:       return "bonded";
        case GHOSTCHI_MOOD_EXCITED:    return "excited";
        case GHOSTCHI_MOOD_FOCUSED:    return "focused";
        case GHOSTCHI_MOOD_AGGRESSIVE: return "spicy";
        case GHOSTCHI_MOOD_SURPRISED:  return "surprised";
        case GHOSTCHI_MOOD_TIRED:      return "tired";
        case GHOSTCHI_MOOD_SLEEPY:     return "sleepy";
        case GHOSTCHI_MOOD_CONFUSED:   return "confused";
        case GHOSTCHI_MOOD_ANGRY:      return "angry";
        case GHOSTCHI_MOOD_CELEBRATE:  return "celebrating";
        case GHOSTCHI_MOOD_NEUTRAL:
        default:                       return "calm";
    }
}

void ghostchi_mood_init(void) {
    mood_ensure_init();
}

void ghostchi_mood_record_event(ghostchi_mood_event_t event, uint8_t intensity) {
    mood_ensure_init();
    if (intensity == 0) intensity = 1;
    if (intensity > 10) intensity = 10;

    uint32_t now = mood_now_ms();
    if (s_mood_lock && xSemaphoreTake(s_mood_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    mood_decay_locked(now);
    s_mood.last_event_ms = now;

    switch (event) {
        case GHOSTCHI_MOOD_EVENT_BOOT:
            s_mood.energy = clamp_score_int(s_mood.energy + 5);
            s_mood.override_mood = GHOSTCHI_MOOD_NEUTRAL;
            s_mood.override_until_ms = now + 2500u;
            break;
        case GHOSTCHI_MOOD_EVENT_WAKE:
        case GHOSTCHI_MOOD_EVENT_USER_INTERACTION:
            s_mood.energy = clamp_score_int(s_mood.energy + 2 + intensity);
            s_mood.happiness = clamp_score_int(s_mood.happiness + intensity);
            s_mood.stress = clamp_score_int(s_mood.stress - 2);
            s_mood.override_mood = GHOSTCHI_MOOD_HAPPY;
            s_mood.override_until_ms = now + 2500u;
            break;
        case GHOSTCHI_MOOD_EVENT_XP_GAIN:
            s_mood.energy = clamp_score_int(s_mood.energy + intensity);
            s_mood.happiness = clamp_score_int(s_mood.happiness + 1 + intensity / 2);
            s_mood.focus = clamp_score_int(s_mood.focus + intensity);
            break;
        case GHOSTCHI_MOOD_EVENT_SUCCESS:
            s_mood.happiness = clamp_score_int(s_mood.happiness + 8 + intensity * 2);
            s_mood.energy = clamp_score_int(s_mood.energy + intensity);
            s_mood.stress = clamp_score_int(s_mood.stress - 8);
            s_mood.override_mood = GHOSTCHI_MOOD_EXCITED;
            s_mood.override_until_ms = now + 6000u;
            break;
        case GHOSTCHI_MOOD_EVENT_LEVEL_UP:
            s_mood.happiness = clamp_score_int(s_mood.happiness + 20);
            s_mood.energy = clamp_score_int(s_mood.energy + 10);
            s_mood.stress = clamp_score_int(s_mood.stress - 12);
            s_mood.override_mood = GHOSTCHI_MOOD_CELEBRATE;
            s_mood.override_until_ms = now + 8000u;
            break;
        case GHOSTCHI_MOOD_EVENT_SESSION_START:
            s_mood.focus = clamp_score_int(s_mood.focus + 18);
            s_mood.energy = clamp_score_int(s_mood.energy + 8);
            s_mood.override_mood = GHOSTCHI_MOOD_FOCUSED;
            s_mood.override_until_ms = now + 4000u;
            break;
        case GHOSTCHI_MOOD_EVENT_SESSION_STOP:
            s_mood.energy = clamp_score_int(s_mood.energy - 8);
            s_mood.focus = clamp_score_int(s_mood.focus - 16);
            s_mood.override_mood = GHOSTCHI_MOOD_TIRED;
            s_mood.override_until_ms = now + 4000u;
            break;
        case GHOSTCHI_MOOD_EVENT_HUNTING:
            s_mood.focus = clamp_score_int(s_mood.focus + 8 + intensity);
            s_mood.energy = clamp_score_int(s_mood.energy - 1);
            s_mood.override_mood = GHOSTCHI_MOOD_FOCUSED;
            s_mood.override_until_ms = now + 3000u;
            break;
        case GHOSTCHI_MOOD_EVENT_ATTACK:
            s_mood.focus = clamp_score_int(s_mood.focus + 10);
            s_mood.stress = clamp_score_int(s_mood.stress + 6 + intensity);
            s_mood.override_mood = GHOSTCHI_MOOD_AGGRESSIVE;
            s_mood.override_until_ms = now + 3500u;
            break;
        case GHOSTCHI_MOOD_EVENT_MISS:
            s_mood.happiness = clamp_score_int(s_mood.happiness - 5);
            s_mood.stress = clamp_score_int(s_mood.stress + 5 + intensity);
            s_mood.energy = clamp_score_int(s_mood.energy - 3);
            s_mood.override_mood = GHOSTCHI_MOOD_CONFUSED;
            s_mood.override_until_ms = now + 3500u;
            break;
        case GHOSTCHI_MOOD_EVENT_BLOCKED:
            s_mood.stress = clamp_score_int(s_mood.stress + 12 + intensity);
            s_mood.happiness = clamp_score_int(s_mood.happiness - 8);
            s_mood.override_mood = GHOSTCHI_MOOD_CONFUSED;
            s_mood.override_until_ms = now + 6000u;
            break;
        case GHOSTCHI_MOOD_EVENT_STORAGE_READY:
            s_mood.happiness = clamp_score_int(s_mood.happiness + 8);
            s_mood.stress = clamp_score_int(s_mood.stress - 10);
            s_mood.override_mood = GHOSTCHI_MOOD_HAPPY;
            s_mood.override_until_ms = now + 3500u;
            break;
        case GHOSTCHI_MOOD_EVENT_SURPRISE:
            s_mood.focus = clamp_score_int(s_mood.focus + 6);
            s_mood.override_mood = GHOSTCHI_MOOD_SURPRISED;
            s_mood.override_until_ms = now + 3500u;
            break;
    }

    if (s_mood_lock) {
        xSemaphoreGive(s_mood_lock);
    }
}

void ghostchi_mood_get_snapshot(ghostchi_mood_snapshot_t *out) {
    if (!out) return;
    mood_ensure_init();

    uint32_t now = mood_now_ms();
    if (s_mood_lock && xSemaphoreTake(s_mood_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        out->mood = GHOSTCHI_MOOD_NEUTRAL;
        strncpy(out->label, ghostchi_mood_label(out->mood), sizeof(out->label) - 1);
        return;
    }

    mood_decay_locked(now);
    out->mood = mood_derive_locked(now);
    out->energy = s_mood.energy;
    out->happiness = s_mood.happiness;
    out->focus = s_mood.focus;
    out->stress = s_mood.stress;
    out->last_event_ms = s_mood.last_event_ms;
    strncpy(out->label, ghostchi_mood_label(out->mood), sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    if (s_mood_lock) {
        xSemaphoreGive(s_mood_lock);
    }
}
