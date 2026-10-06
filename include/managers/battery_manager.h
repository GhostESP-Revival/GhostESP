#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery HAL. One interface over every battery source the firmware supports:
 *   - dedicated fuel gauge (CONFIG_HAS_FUEL_GAUGE: BQ27220 / MAX17048)
 *   - AXP2101 PMU          (CONFIG_HAS_BATTERY)
 *   - plain ADC divider    (CONFIG_HAS_BATTERY_ADC)
 * Boards with none of these report battery_manager_present() == false.
 *
 * Reads are cached and rate-limited, so any task may call battery_manager_get()
 * freely (status bar, plugins, CLI) without hammering the I2C bus or ADC.
 */

typedef struct {
    bool valid;            // false until a backend produced a reading
    uint8_t percent;       // 0-100
    uint16_t voltage_mv;   // 0 when the backend cannot report it
    bool charging;
} battery_status_t;

typedef enum {
    BATTERY_LEVEL_NORMAL = 0,
    BATTERY_LEVEL_LOW,       // <= BATTERY_LOW_PERCENT, not charging
    BATTERY_LEVEL_CRITICAL,  // <= BATTERY_CRITICAL_PERCENT, not charging
} battery_level_t;

#ifndef BATTERY_LOW_PERCENT
#define BATTERY_LOW_PERCENT 15
#endif
#ifndef BATTERY_CRITICAL_PERCENT
#define BATTERY_CRITICAL_PERCENT 5
#endif
// Percent above a threshold required before the level is released again.
#ifndef BATTERY_LEVEL_HYSTERESIS
#define BATTERY_LEVEL_HYSTERESIS 3
#endif

/**
 * Called when the debounced battery level changes. Runs on whichever task
 * performed the refresh, so listeners must be thread-agnostic (marshal to
 * LVGL themselves if they touch the UI).
 */
typedef void (*battery_level_cb_t)(battery_level_t level, const battery_status_t *status, void *ctx);

/** true when this build has any battery backend. */
bool battery_manager_present(void);

/** Bring up the backend (fuel gauge init). Safe to call more than once. */
bool battery_manager_init(void);

/**
 * Latest battery status. Refreshes from hardware when the cache is older than
 * the backend's poll interval. Returns false when no valid reading exists.
 */
bool battery_manager_get(battery_status_t *out);

/** Last debounced level; BATTERY_LEVEL_NORMAL when unknown. */
battery_level_t battery_manager_get_level(void);

/**
 * While set, battery_manager_get() never touches hardware and serves the cache
 * (used by NFC views that need exclusive, jitter-free I2C access).
 */
void battery_manager_set_low_i2c_mode(bool on);
bool battery_manager_is_low_i2c_mode(void);

/** Register a level-change listener (small fixed table). false when full. */
bool battery_manager_add_level_listener(battery_level_cb_t cb, void *ctx);

const char *battery_level_name(battery_level_t level);

#ifdef __cplusplus
}
#endif

#endif // BATTERY_MANAGER_H
