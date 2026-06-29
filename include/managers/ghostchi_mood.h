#ifndef GHOSTCHI_MOOD_H
#define GHOSTCHI_MOOD_H

#include <stdint.h>

typedef enum {
    GHOSTCHI_MOOD_NEUTRAL = 0,
    GHOSTCHI_MOOD_HAPPY,
    GHOSTCHI_MOOD_LOVE,
    GHOSTCHI_MOOD_EXCITED,
    GHOSTCHI_MOOD_FOCUSED,
    GHOSTCHI_MOOD_AGGRESSIVE,
    GHOSTCHI_MOOD_SURPRISED,
    GHOSTCHI_MOOD_TIRED,
    GHOSTCHI_MOOD_SLEEPY,
    GHOSTCHI_MOOD_CONFUSED,
    GHOSTCHI_MOOD_ANGRY,
    GHOSTCHI_MOOD_CELEBRATE,
} ghostchi_mood_t;

typedef enum {
    GHOSTCHI_MOOD_EVENT_BOOT = 0,
    GHOSTCHI_MOOD_EVENT_WAKE,
    GHOSTCHI_MOOD_EVENT_USER_INTERACTION,
    GHOSTCHI_MOOD_EVENT_XP_GAIN,
    GHOSTCHI_MOOD_EVENT_SUCCESS,
    GHOSTCHI_MOOD_EVENT_LEVEL_UP,
    GHOSTCHI_MOOD_EVENT_SESSION_START,
    GHOSTCHI_MOOD_EVENT_SESSION_STOP,
    GHOSTCHI_MOOD_EVENT_HUNTING,
    GHOSTCHI_MOOD_EVENT_ATTACK,
    GHOSTCHI_MOOD_EVENT_MISS,
    GHOSTCHI_MOOD_EVENT_BLOCKED,
    GHOSTCHI_MOOD_EVENT_STORAGE_READY,
    GHOSTCHI_MOOD_EVENT_SURPRISE,
} ghostchi_mood_event_t;

typedef struct {
    ghostchi_mood_t mood;
    uint8_t energy;      /* 0-100 */
    uint8_t happiness;   /* 0-100 */
    uint8_t focus;       /* 0-100 */
    uint8_t stress;      /* 0-100 */
    uint32_t last_event_ms;
    char label[24];
} ghostchi_mood_snapshot_t;

void ghostchi_mood_init(void);
void ghostchi_mood_record_event(ghostchi_mood_event_t event, uint8_t intensity);
void ghostchi_mood_get_snapshot(ghostchi_mood_snapshot_t *out);
const char *ghostchi_mood_label(ghostchi_mood_t mood);

#endif /* GHOSTCHI_MOOD_H */
