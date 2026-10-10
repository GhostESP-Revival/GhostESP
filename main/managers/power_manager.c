#include "managers/power_manager.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_idf_version.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_RTCIO_PIN_COUNT > 0
#include "driver/rtc_io.h"
#endif

#include "managers/ap_manager.h"
#include "managers/battery_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"

static const char *TAG = "Power";

// Automatically fall back to SAVER while the battery is low (user can still override).
#ifndef POWER_LOW_BATTERY_AUTO_SAVER
#define POWER_LOW_BATTERY_AUTO_SAVER 1
#endif

#define MAX_PROFILE_LISTENERS 4

/* ------------------------------------------------------------------ */
/* Board capabilities                                                  */
/* ------------------------------------------------------------------ */

/*
 * Automatic light sleep needs PM + tickless idle in the sdkconfig AND the config must be flagged
 * CONFIG_POWER_LIGHT_SLEEP_VALIDATED. Light sleep interacts with backlight PWM, GPIO interrupts,
 * USB and peripherals in board-specific ways; a config that merely turns PM on gets CPU frequency
 * scaling only until someone has validated it (power sleeptest) and set the flag.
 */
bool power_manager_light_sleep_available(void) {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && defined(CONFIG_POWER_LIGHT_SLEEP_VALIDATED)
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* Profiles                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t max_mhz;
    uint32_t min_mhz;
    bool light_sleep;
    bool ap_allowed;
} profile_params_t;

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static profile_params_t params_for(power_profile_t p) {
    const uint32_t def = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    profile_params_t out;
    switch (p) {
        case POWER_PROFILE_SAVER:
            out.max_mhz = min_u32(160, def);
            out.min_mhz = min_u32(80, out.max_mhz);
            out.light_sleep = true;
            out.ap_allowed = false;
            break;
        case POWER_PROFILE_BALANCED:
            out.max_mhz = def;
            out.min_mhz = min_u32(80, def);
            out.light_sleep = true;
            out.ap_allowed = true;
            break;
        case POWER_PROFILE_PERFORMANCE:
        default:
            out.max_mhz = def;
            out.min_mhz = def;
            out.light_sleep = false;
            out.ap_allowed = true;
            break;
    }
    out.light_sleep = out.light_sleep && power_manager_light_sleep_available();
    return out;
}

const char *power_profile_name(power_profile_t profile) {
    switch (profile) {
        case POWER_PROFILE_BALANCED: return "balanced";
        case POWER_PROFILE_SAVER: return "saver";
        case POWER_PROFILE_PERFORMANCE:
        default: return "performance";
    }
}

bool power_profile_parse(const char *text, power_profile_t *out) {
    if (!text || !out) return false;
    char buf[16];
    size_t n = strlen(text);
    if (n == 0 || n >= sizeof(buf)) return false;
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)text[i]);
    buf[n] = '\0';
    if (!strcmp(buf, "performance") || !strcmp(buf, "perf") || !strcmp(buf, "0")) {
        *out = POWER_PROFILE_PERFORMANCE;
    } else if (!strcmp(buf, "balanced") || !strcmp(buf, "1")) {
        *out = POWER_PROFILE_BALANCED;
    } else if (!strcmp(buf, "saver") || !strcmp(buf, "save") || !strcmp(buf, "2")) {
        *out = POWER_PROFILE_SAVER;
    } else {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static portMUX_TYPE s_init_spin = portMUX_INITIALIZER_UNLOCKED;

static power_profile_t s_effective = POWER_PROFILE_PERFORMANCE;
static bool s_lowbatt_override;
static bool s_override_dismissed; // user picked a profile while low; leave it alone until recovery
static int s_last_ap_allowed = -1; // -1 = never applied
static bool s_initialized;

static struct {
    power_profile_cb_t cb;
    void *ctx;
} s_listeners[MAX_PROFILE_LISTENERS];
static int s_listener_count;

static SemaphoreHandle_t mtx(void) {
    if (!s_mtx) {
        taskENTER_CRITICAL(&s_init_spin);
        if (!s_mtx) s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
        taskEXIT_CRITICAL(&s_init_spin);
    }
    return s_mtx;
}

static power_profile_t user_profile(void) {
    uint8_t p = settings_get_power_profile(&G_Settings);
    return p < POWER_PROFILE_COUNT ? (power_profile_t)p : POWER_PROFILE_PERFORMANCE;
}

static power_profile_t compute_effective(void) {
    power_profile_t user = user_profile();
    if (s_lowbatt_override && user != POWER_PROFILE_SAVER) return POWER_PROFILE_SAVER;
    return user;
}

power_profile_t power_manager_get_profile(void) { return user_profile(); }
power_profile_t power_manager_get_effective_profile(void) { return s_effective; }

bool power_manager_ap_allowed(void) { return params_for(s_effective).ap_allowed; }

/* Apply the effective profile to the hardware. Caller holds the mutex. */
static void apply_locked(void) {
    power_profile_t eff = compute_effective();
    profile_params_t p = params_for(eff);

#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = {
        .max_freq_mhz = (int)p.max_mhz,
        .min_freq_mhz = (int)p.min_mhz,
        .light_sleep_enable = p.light_sleep,
    };
    rgb_manager_power_transition_begin();
    esp_err_t err = esp_pm_configure(&cfg);
    rgb_manager_power_transition_end();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(%s) failed: %s", power_profile_name(eff), esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "profile %s: cpu %u-%u MHz, light sleep %s", power_profile_name(eff),
                 (unsigned)p.min_mhz, (unsigned)p.max_mhz, p.light_sleep ? "on" : "off");
    }
#else
    ESP_LOGI(TAG, "profile %s (PM compiled out: no DFS/light sleep on this build)", power_profile_name(eff));
#endif

    s_effective = eff;

    // The AP is only touched when its allowed-state flips (or on the very first apply,
    // which is what brings it up at boot), so low-battery transitions don't churn it.
    if (s_last_ap_allowed != (int)p.ap_allowed && !wifi_manager_is_evil_portal_active()) {
        if (!p.ap_allowed) {
            ap_manager_stop_services();
        } else if (settings_get_ap_enabled(&G_Settings)) {
            (void)ap_manager_restore_after_attack("power profile");
        }
    }
    s_last_ap_allowed = (int)p.ap_allowed;
}

static void notify_listeners(power_profile_t eff) {
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].cb) s_listeners[i].cb(eff, s_listeners[i].ctx);
    }
}

static void apply_and_notify(void) {
    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    apply_locked();
    power_profile_t eff = s_effective;
    xSemaphoreGive(m);
    notify_listeners(eff);
}

esp_err_t power_manager_set_profile(power_profile_t profile, bool persist) {
    if ((int)profile < 0 || profile >= POWER_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;

    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    if (s_lowbatt_override) {
        s_lowbatt_override = false;
        s_override_dismissed = true;
    }
    xSemaphoreGive(m);

    settings_set_power_profile(&G_Settings, (uint8_t)profile);
    if (persist) settings_persist_setting(SETTING_POWER_PROFILE);
    apply_and_notify();
    return ESP_OK;
}

void power_manager_set_power_save(bool enabled) {
    power_profile_t cur = user_profile();
    if (enabled) {
        (void)power_manager_set_profile(POWER_PROFILE_SAVER, true);
    } else if (cur == POWER_PROFILE_SAVER) {
        (void)power_manager_set_profile(POWER_PROFILE_PERFORMANCE, true);
    }
}

void power_manager_apply_settings(void) { apply_and_notify(); }

bool power_manager_add_profile_listener(power_profile_cb_t cb, void *ctx) {
    if (!cb) return false;
    bool added = false;
    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    if (s_listener_count < MAX_PROFILE_LISTENERS) {
        s_listeners[s_listener_count].cb = cb;
        s_listeners[s_listener_count].ctx = ctx;
        s_listener_count++;
        added = true;
    }
    xSemaphoreGive(m);
    return added;
}

/* ------------------------------------------------------------------ */
/* Battery policy                                                      */
/* ------------------------------------------------------------------ */

static void on_battery_level(battery_level_t level, const battery_status_t *st, void *ctx) {
    (void)st;
    (void)ctx;
#if POWER_LOW_BATTERY_AUTO_SAVER
    bool changed = false;
    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    if (level == BATTERY_LEVEL_NORMAL) {
        s_override_dismissed = false;
        if (s_lowbatt_override) {
            s_lowbatt_override = false;
            changed = true;
        }
    } else if (!s_lowbatt_override && !s_override_dismissed && user_profile() != POWER_PROFILE_SAVER) {
        s_lowbatt_override = true;
        changed = true;
    }
    xSemaphoreGive(m);
    if (changed) {
        ESP_LOGI(TAG, "battery %s: %s low-battery saver", battery_level_name(level),
                 level == BATTERY_LEVEL_NORMAL ? "releasing" : "engaging");
        apply_and_notify();
    }
#else
    (void)level;
#endif
}

static void wifi_sleep_prepare(power_sleep_kind_t kind, void *ctx) {
    (void)ctx;
    if (kind == POWER_SLEEP_DEEP) (void)esp_wifi_stop(); // NOT_INIT is fine
}

static void sd_sleep_prepare(power_sleep_kind_t kind, void *ctx) {
    (void)ctx;
    if (kind == POWER_SLEEP_DEEP) sd_card_unmount_with_context(SD_UNMOUNT_CONTEXT_SHUTDOWN); // flush before power is cut
}

void power_manager_init(void) {
    if (s_initialized) {
        apply_and_notify();
        return;
    }
    s_initialized = true;
    power_manager_log_boot_wake();
    // Registered first so they run last: radios/SD go down after UI hooks (display, LEDs).
    const power_sleep_hook_t wifi_hook = {.name = "wifi", .prepare = wifi_sleep_prepare};
    const power_sleep_hook_t sd_hook = {.name = "sd", .prepare = sd_sleep_prepare};
    (void)power_manager_register_sleep_hook(&wifi_hook);
    (void)power_manager_register_sleep_hook(&sd_hook);
    (void)battery_manager_add_level_listener(on_battery_level, NULL);
    apply_and_notify();
}

/* ------------------------------------------------------------------ */
/* PM locks                                                            */
/* ------------------------------------------------------------------ */

power_lock_t power_lock_create(power_lock_type_t type, const char *name) {
#if CONFIG_PM_ENABLE
    esp_pm_lock_type_t t;
    switch (type) {
        case POWER_LOCK_CPU_MAX: t = ESP_PM_CPU_FREQ_MAX; break;
        case POWER_LOCK_APB_MAX: t = ESP_PM_APB_FREQ_MAX; break;
        case POWER_LOCK_NO_LIGHT_SLEEP:
        default: t = ESP_PM_NO_LIGHT_SLEEP; break;
    }
    esp_pm_lock_handle_t h = NULL;
    esp_err_t err = esp_pm_lock_create(t, 0, name ? name : "power", &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lock '%s' create failed: %s", name ? name : "?", esp_err_to_name(err));
        return NULL;
    }
    return (power_lock_t)h;
#else
    (void)type;
    (void)name;
    return NULL;
#endif
}

void power_lock_acquire(power_lock_t lock) {
#if CONFIG_PM_ENABLE
    if (lock) (void)esp_pm_lock_acquire((esp_pm_lock_handle_t)lock);
#else
    (void)lock;
#endif
}

void power_lock_release(power_lock_t lock) {
#if CONFIG_PM_ENABLE
    if (lock) (void)esp_pm_lock_release((esp_pm_lock_handle_t)lock);
#else
    (void)lock;
#endif
}

void power_lock_delete(power_lock_t lock) {
#if CONFIG_PM_ENABLE
    if (lock) (void)esp_pm_lock_delete((esp_pm_lock_handle_t)lock);
#else
    (void)lock;
#endif
}

/* ------------------------------------------------------------------ */
/* Sleep hooks                                                         */
/* ------------------------------------------------------------------ */

#define MAX_SLEEP_HOOKS 12
// A hook slower than this is logged: a stalled prepare() is how a board "won't go to sleep".
#define HOOK_SLOW_MS 500

static power_sleep_hook_t s_hooks[MAX_SLEEP_HOOKS];
static int s_hook_count;

bool power_manager_register_sleep_hook(const power_sleep_hook_t *hook) {
    if (!hook || !hook->name) return false;
    bool added = false;
    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    if (s_hook_count < MAX_SLEEP_HOOKS) {
        s_hooks[s_hook_count++] = *hook;
        added = true;
    } else {
        ESP_LOGW(TAG, "sleep hook table full, dropping '%s'", hook->name);
    }
    xSemaphoreGive(m);
    return added;
}

static void run_prepare_hooks(power_sleep_kind_t kind) {
    for (int i = s_hook_count - 1; i >= 0; i--) {
        if (!s_hooks[i].prepare) continue;
        int64_t t0 = esp_timer_get_time();
        s_hooks[i].prepare(kind, s_hooks[i].ctx);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        if (ms >= HOOK_SLOW_MS) {
            ESP_LOGW(TAG, "sleep hook '%s' prepare took %lld ms", s_hooks[i].name, (long long)ms);
        } else {
            ESP_LOGI(TAG, "sleep hook '%s' prepared (%lld ms)", s_hooks[i].name, (long long)ms);
        }
    }
}

static void run_restore_hooks(power_sleep_kind_t kind) {
    for (int i = 0; i < s_hook_count; i++) {
        if (s_hooks[i].restore) s_hooks[i].restore(kind, s_hooks[i].ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Deep sleep                                                          */
/* ------------------------------------------------------------------ */

// Wake pin must read its idle level this long before we arm it, or we would wake instantly.
#define WAKE_IDLE_STABLE_MS 50
#define WAKE_IDLE_TIMEOUT_MS 5000

static esp_err_t arm_gpio_wake(int gpio, int level) {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    esp_err_t err = esp_sleep_enable_ext0_wakeup((gpio_num_t)gpio, level);
    if (err == ESP_OK) return err;
#endif
#if SOC_PM_SUPPORT_EXT1_WAKEUP
    {
        esp_sleep_ext1_wakeup_mode_t mode = level ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW;
        esp_err_t err1 = esp_sleep_enable_ext1_wakeup_io(1ULL << gpio, mode);
        if (err1 == ESP_OK) return err1;
    }
#endif
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        esp_err_t errg = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << gpio, level ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
#else
        esp_err_t errg = esp_deep_sleep_enable_gpio_wakeup(
            1ULL << gpio, level ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
        if (errg == ESP_OK) return errg;
    }
#endif
    return ESP_ERR_NOT_SUPPORTED;
}

/* Wait until the wake pin has sat at its idle level for WAKE_IDLE_STABLE_MS. */
static bool wait_wake_pin_idle(int gpio, int wake_level) {
    int stable = 0;
    for (int waited = 0; waited < WAKE_IDLE_TIMEOUT_MS; waited += 10) {
        if (gpio_get_level((gpio_num_t)gpio) != wake_level) {
            stable += 10;
            if (stable >= WAKE_IDLE_STABLE_MS) return true;
        } else {
            stable = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void rail_set(int gpio, int level) {
    if (gpio == POWER_GPIO_NONE) return;
    gpio_hold_dis((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)gpio, level);
}

esp_err_t power_manager_deep_sleep(const power_deep_sleep_cfg_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    const bool have_wake = cfg->wake_gpio != POWER_GPIO_NONE;
    const bool have_rail = cfg->power_rail_gpio != POWER_GPIO_NONE;
    if (!have_wake && cfg->timer_wake_us == 0) {
        ESP_LOGE(TAG, "deep sleep refused: no wake source configured");
        return ESP_ERR_INVALID_ARG;
    }

    if (have_wake) {
        const gpio_config_t in = {
            .pin_bit_mask = 1ULL << cfg->wake_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = cfg->wake_level ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en = cfg->wake_level ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        if (cfg->settle_ms) vTaskDelay(pdMS_TO_TICKS(cfg->settle_ms));
        if (!wait_wake_pin_idle(cfg->wake_gpio, cfg->wake_level)) {
            ESP_LOGE(TAG, "deep sleep refused: GPIO%d still at wake level %d (button held/stuck?)",
                     cfg->wake_gpio, cfg->wake_level);
            return ESP_ERR_INVALID_STATE;
        }
    }

    run_prepare_hooks(POWER_SLEEP_DEEP);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (have_wake) {
        esp_err_t err = arm_gpio_wake(cfg->wake_gpio, cfg->wake_level);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d wake unsupported (%s)%s", cfg->wake_gpio, esp_err_to_name(err),
                     cfg->timer_wake_us ? ", falling back to timer only" : "");
            if (!cfg->timer_wake_us) {
                run_restore_hooks(POWER_SLEEP_DEEP);
                return err;
            }
        } else {
#if SOC_RTCIO_PIN_COUNT > 0
            // The digital pull is lost in deep sleep; keep the idle level on the RTC domain.
            if (rtc_gpio_is_valid_gpio((gpio_num_t)cfg->wake_gpio)) {
                if (cfg->wake_level) {
                    rtc_gpio_pullup_dis((gpio_num_t)cfg->wake_gpio);
                    rtc_gpio_pulldown_en((gpio_num_t)cfg->wake_gpio);
                } else {
                    rtc_gpio_pulldown_dis((gpio_num_t)cfg->wake_gpio);
                    rtc_gpio_pullup_en((gpio_num_t)cfg->wake_gpio);
                }
            }
#endif
        }
    }

    if (cfg->timer_wake_us) {
        esp_err_t err = esp_sleep_enable_timer_wakeup(cfg->timer_wake_us);
        if (err != ESP_OK && !have_wake) {
            run_restore_hooks(POWER_SLEEP_DEEP);
            return err;
        }
    }

    // Drop the rail last, then latch it: without the hold the pin floats once the digital
    // domain powers down and the peripherals it gates come back on mid-"sleep".
    if (have_rail) {
        rail_set(cfg->power_rail_gpio, 0);
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP
        gpio_hold_en((gpio_num_t)cfg->power_rail_gpio);
#endif
    }
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif

    ESP_LOGI(TAG, "entering deep sleep (wake gpio %d level %d, timer %llu us, rail gpio %d)",
             cfg->wake_gpio, cfg->wake_level, (unsigned long long)cfg->timer_wake_us,
             cfg->power_rail_gpio);
    vTaskDelay(pdMS_TO_TICKS(100)); // let the log drain
    esp_deep_sleep_start();

    // Only reached when the SoC rejected deep sleep.
    ESP_LOGE(TAG, "esp_deep_sleep_start returned; aborting shutdown");
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    if (have_rail) rail_set(cfg->power_rail_gpio, 1);
    run_restore_hooks(POWER_SLEEP_DEEP);
    return ESP_FAIL;
}

power_deep_sleep_cfg_t power_manager_board_sleep_cfg(void) {
    power_deep_sleep_cfg_t cfg = {
        .wake_gpio = CONFIG_POWER_WAKE_GPIO,
        .wake_level = CONFIG_POWER_WAKE_LEVEL,
        .timer_wake_us = 0,
        .power_rail_gpio = CONFIG_POWER_RAIL_GPIO,
        .settle_ms = 0,
    };
    if (cfg.wake_gpio < 0) cfg.wake_gpio = POWER_GPIO_NONE;
    if (cfg.power_rail_gpio < 0) cfg.power_rail_gpio = POWER_GPIO_NONE;
    return cfg;
}

esp_err_t power_manager_shutdown(void) {
    power_deep_sleep_cfg_t cfg = power_manager_board_sleep_cfg();
    if (cfg.wake_gpio == POWER_GPIO_NONE) {
        ESP_LOGW(TAG, "shutdown refused: this board declares no CONFIG_POWER_WAKE_GPIO");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return power_manager_deep_sleep(&cfg);
}

/* ------------------------------------------------------------------ */
/* Light-sleep validation + boot diagnostics                           */
/* ------------------------------------------------------------------ */

uint32_t power_manager_wake_causes(void) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return esp_sleep_get_wakeup_causes();
#else
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return cause == ESP_SLEEP_WAKEUP_UNDEFINED ? 0 : (1u << (uint32_t)cause);
#endif
}

esp_err_t power_manager_light_sleep_timed(uint32_t ms, uint32_t *slept_ms, uint32_t *wake_causes) {
    if (ms == 0) return ESP_ERR_INVALID_ARG;
    run_prepare_hooks(POWER_SLEEP_LIGHT);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    uint32_t causes = power_manager_wake_causes();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    run_restore_hooks(POWER_SLEEP_LIGHT);
    if (slept_ms) *slept_ms = (uint32_t)dt;
    if (wake_causes) *wake_causes = causes;
    return err;
}

bool power_manager_pm_compiled(void) {
#if CONFIG_PM_ENABLE
    return true;
#else
    return false;
#endif
}

bool power_manager_light_sleep_validated(void) {
#ifdef CONFIG_POWER_LIGHT_SLEEP_VALIDATED
    return true;
#else
    return false;
#endif
}

void power_manager_log_boot_wake(void) {
    uint32_t causes = power_manager_wake_causes();
    ESP_LOGI(TAG, "boot: reset reason %d, sleep wake causes 0x%x%s", (int)esp_reset_reason(),
             (unsigned)causes, causes == 0 ? " (cold boot)" : "");
    // Holds latched for deep sleep persist across the wake reset until released.
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    if (CONFIG_POWER_RAIL_GPIO >= 0) gpio_hold_dis((gpio_num_t)CONFIG_POWER_RAIL_GPIO);
}
