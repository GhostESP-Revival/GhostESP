#ifndef GHOSTCHI_MANAGER_H
#define GHOSTCHI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GHOSTCHI_STATE_BLOCKED = 0,
    GHOSTCHI_STATE_IDLE,
    GHOSTCHI_STATE_SWEEP,
    GHOSTCHI_STATE_RANK,
    GHOSTCHI_STATE_LOCK,
    GHOSTCHI_STATE_STIM,
    GHOSTCHI_STATE_COOLDOWN,
    GHOSTCHI_STATE_STOPPING,
} ghostchi_state_t;

typedef struct {
    bool sd_ready;
    bool running;
    bool idle_clock_valid;
    ghostchi_state_t state;
    uint32_t handshakes;
    uint32_t attempts;
    uint32_t failures;
    uint32_t idle_for_sec;
    uint32_t total_sessions;
    uint32_t total_xp;
    uint32_t level_up_at_ms;  /* esp_timer_get_time()/1000 of last level-up; 0 = none */
    uint16_t aps_visible;
    uint8_t current_channel;
    uint8_t confidence;
    uint8_t cooldown_targets;
    uint32_t uptime_sec;
    char target_ssid[33];
    char target_bssid[18];
    char reason[48];
    char status_line[48];
    char session_name[32];
} ghostchi_snapshot_t;

void ghostchi_manager_get_snapshot(ghostchi_snapshot_t *out);
bool ghostchi_manager_probe_storage(void);
bool ghostchi_manager_start(void);
void ghostchi_manager_tick(void);
void ghostchi_manager_stop(void);
void ghostchi_manager_add_xp(uint32_t amount);

/* Passive (default) listens for handshakes without sending deauths.
 * Aggressive preserves the legacy behaviour with a deauth burst per
 * target. The change takes effect on the next sweep (the next time
 * choose_strategy() is called in tick()). Persisted to the state file. */
void ghostchi_manager_set_aggressive(bool on);
bool ghostchi_manager_is_aggressive(void);

#endif
