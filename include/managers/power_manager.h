#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Power manager: the single owner of CPU frequency scaling, automatic light
 * sleep, PM locks, deep-sleep entry and battery-driven power policy.
 *
 * Profiles replace the old on/off "power save" flag:
 *   PERFORMANCE  fixed max clock, no automatic light sleep, AP allowed
 *   BALANCED     max clock under load, DFS down to 80 MHz, light sleep, AP allowed
 *   SAVER        capped clock, DFS to 80 MHz, light sleep, AP stopped
 * Light sleep is only ever enabled on builds with CONFIG_PM_ENABLE and boards
 * whose capability table allows it (e.g. T-Deck's trackball does not).
 *
 * The legacy power_save setting maps to SAVER (true) / PERFORMANCE (false).
 */

typedef enum {
    POWER_PROFILE_PERFORMANCE = 0,
    POWER_PROFILE_BALANCED = 1,
    POWER_PROFILE_SAVER = 2,
    POWER_PROFILE_COUNT
} power_profile_t;

const char *power_profile_name(power_profile_t profile);
/** Case-insensitive parse of "performance"/"balanced"/"saver" (or 0-2). */
bool power_profile_parse(const char *text, power_profile_t *out);

/** Called after the effective profile is (re)applied. Runs on the caller's task. */
typedef void (*power_profile_cb_t)(power_profile_t effective, void *ctx);

/** Apply the persisted profile and hook battery-level policy. Call once, after settings load. */
void power_manager_init(void);

/** Profile the user chose (persisted). */
power_profile_t power_manager_get_profile(void);
/** Profile actually in force; differs only while a low-battery override is active. */
power_profile_t power_manager_get_effective_profile(void);

/**
 * Select a profile. When persist is true the choice is stored in settings/NVS.
 * Also cancels any low-battery override so the user's choice always wins.
 */
esp_err_t power_manager_set_profile(power_profile_t profile, bool persist);

/** Re-apply whatever G_Settings currently holds (after CLI set, import, plugin write). */
void power_manager_apply_settings(void);

/** false while the effective profile wants the soft-AP down (SAVER). */
bool power_manager_ap_allowed(void);

/** Legacy power_save alias. */
void power_manager_set_power_save(bool enabled);

/** true when this build+board will actually use automatic light sleep. */
bool power_manager_light_sleep_available(void);

/** Notified after each apply (display uses it to re-latch the backlight timer). */
bool power_manager_add_profile_listener(power_profile_cb_t cb, void *ctx);

/* ---- PM locks -------------------------------------------------------- */

typedef enum {
    POWER_LOCK_NO_LIGHT_SLEEP = 0, // keep CPU/peripherals running, DFS still allowed
    POWER_LOCK_CPU_MAX,            // hold max CPU frequency
    POWER_LOCK_APB_MAX,            // hold max APB frequency
} power_lock_type_t;

typedef struct power_lock *power_lock_t;

/**
 * Create a lock. Returns NULL when PM is compiled out or allocation fails; every
 * other power_lock_* call accepts NULL as a no-op, so callers need no #ifdefs.
 */
power_lock_t power_lock_create(power_lock_type_t type, const char *name);
void power_lock_acquire(power_lock_t lock);
void power_lock_release(power_lock_t lock);
void power_lock_delete(power_lock_t lock);

/* ---- sleep ----------------------------------------------------------- */

typedef enum {
    POWER_SLEEP_LIGHT = 0, // explicit timed light sleep (power sleeptest)
    POWER_SLEEP_DEEP,      // deep sleep / shutdown; nothing survives except RTC state
} power_sleep_kind_t;

/**
 * Every subsystem that holds hardware in a state sleep must not find it in (backlight, LEDs,
 * radios, SD, rails...) registers a hook. prepare() runs before sleep, in REVERSE registration
 * order (last registered is quiesced first). restore() runs after an explicit light sleep, or
 * when a deep sleep is aborted. Either may be NULL. Keep both short and non-blocking.
 */
typedef struct {
    const char *name;
    void (*prepare)(power_sleep_kind_t kind, void *ctx);
    void (*restore)(power_sleep_kind_t kind, void *ctx);
    void *ctx;
} power_sleep_hook_t;

bool power_manager_register_sleep_hook(const power_sleep_hook_t *hook);

#define POWER_GPIO_NONE (-1)

typedef struct {
    int wake_gpio;            // RTC-capable wake pin, or POWER_GPIO_NONE
    int wake_level;           // level that wakes (0 = low)
    uint64_t timer_wake_us;   // 0 = no timer wake
    int power_rail_gpio;      // driven low before sleep (peripheral rail), or POWER_GPIO_NONE
    uint32_t settle_ms;       // wait for the wake button to be released/settle before arming
} power_deep_sleep_cfg_t;

/**
 * Standard shutdown: clear stale wake sources, release the wake pin so a held
 * button cannot wake immediately, arm the wake sources the chip supports
 * (EXT0, EXT1, timer), drop the power rail and enter deep sleep.
 * Does not return on success. On failure it restores the rail and returns an
 * error, leaving the caller running.
 */
esp_err_t power_manager_deep_sleep(const power_deep_sleep_cfg_t *cfg);

/**
 * Shut down using the board wiring declared in Kconfig (POWER_WAKE_GPIO / POWER_WAKE_LEVEL /
 * POWER_RAIL_GPIO). Returns ESP_ERR_NOT_SUPPORTED when the board declares no wake pin, so a new
 * config can never half-configure deep sleep.
 */
esp_err_t power_manager_shutdown(void);

/** The board's declared deep-sleep wiring (wake_gpio == POWER_GPIO_NONE when undeclared). */
power_deep_sleep_cfg_t power_manager_board_sleep_cfg(void);

/**
 * Explicit timed light sleep with the sleep hooks run around it. For hardware validation:
 * returns how long the chip really slept so a board that refuses or wakes early is obvious.
 */
esp_err_t power_manager_light_sleep_timed(uint32_t ms, uint32_t *slept_ms, uint32_t *wake_causes);

/** Bitmap of esp_sleep_wakeup_cause_t bits for the last wake (0 = none/cold boot). */
uint32_t power_manager_wake_causes(void);

/** Log why we booted (deep-sleep wake cause, reset reason) and release deep-sleep GPIO holds. */
void power_manager_log_boot_wake(void);

/** One-line diagnostics for the CLI. */
bool power_manager_pm_compiled(void);
bool power_manager_light_sleep_validated(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_MANAGER_H
