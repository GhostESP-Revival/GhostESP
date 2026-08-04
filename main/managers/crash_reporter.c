#include "managers/crash_reporter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#ifdef CONFIG_WITH_SCREEN
#include "lvgl.h"
#include "gui/design_tokens.h"
#include "gui/popup.h"
#include "managers/display_manager.h"
#endif

#define TAG "CrashReporter"

typedef struct {
    char reason[192];
    volatile bool pending;
#ifdef CONFIG_WITH_SCREEN
    bool poll_scheduled;
    int poll_ticks;
    popup_confirm_t *popup;
#endif
} crash_reporter_state_t;

/* Allocated only after a coredump is found; this pointer is the reporter's
 * only BSS state. */
static crash_reporter_state_t *s_state = NULL;

#ifdef CONFIG_WITH_SCREEN
static void crash_popup_schedule_cb(void *arg);
#endif

void crash_reporter_set_boot_crash(const char *panic_reason) {
    if (!s_state) {
        s_state = calloc(1, sizeof(*s_state));
        if (!s_state) {
            ESP_LOGE(TAG, "Failed to allocate crash report state");
            return;
        }
    }

    if (panic_reason && panic_reason[0]) {
        snprintf(s_state->reason, sizeof(s_state->reason), "%s", panic_reason);
    }
    s_state->pending = true;
    ESP_LOGI(TAG, "Boot crash recorded: %s", s_state->reason[0] ? s_state->reason : "(no reason)");
#ifdef CONFIG_WITH_SCREEN
    if (!s_state->poll_scheduled) {
        s_state->poll_scheduled = true;
        display_manager_run_on_lvgl(crash_popup_schedule_cb, NULL);
    }
#endif
}

#ifdef CONFIG_WITH_SCREEN

static void crash_popup_dismiss_cb(void *arg) {
    crash_reporter_state_t *state = arg;
    if (s_state == state) {
        s_state = NULL;
        free(state);
    }
}

static void crash_popup_show(void) {
    crash_reporter_state_t *state = s_state;
    if (!state || state->popup) return;
    const char *crash_type = state->reason[0] ? state->reason : "Details unavailable";
    state->popup = popup_confirm_show(&state->popup, NULL, "Crash Detected", crash_type,
                                      "OK", NULL, crash_popup_dismiss_cb, state);
    ESP_LOGW(TAG, "Crash popup shown (reason: %s)", crash_type);
}

bool crash_reporter_handle_input(const void *event_data) {
    const InputEvent *event = event_data;
    crash_reporter_state_t *state = s_state;
    if (!event || !state || !popup_confirm_is_open(state->popup)) return false;
    if (event->type == INPUT_TYPE_TOUCH) {
        return popup_confirm_handle_touch(&state->popup, &event->data.touch_data);
    }

    bool dismiss = event->type == INPUT_TYPE_EXIT_BUTTON ||
                   (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed &&
                    event->data.joystick_index == 1) ||
                   (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) ||
                   (event->type == INPUT_TYPE_KEYBOARD && !event->is_touch_move &&
                    (event->data.key_value == 10 || event->data.key_value == 13 ||
                     event->data.key_value == LV_KEY_ENTER));
    if (dismiss) popup_confirm_select(&state->popup);
    return true;
}

static void crash_popup_poll_cb(lv_timer_t *timer) {
    crash_reporter_state_t *state = s_state;
    if (!state) {
        lv_timer_del(timer);
        return;
    }
    View *current = display_manager_get_current_view();
    bool boot_screen_still_up = current && current->name &&
                                (strcmp(current->name, "Splash Screen") == 0 ||
                                 strcmp(current->name, "T-Dongle Status") == 0);
    if (state->pending && !boot_screen_still_up) {
        crash_popup_show();
        state->poll_scheduled = false;
        lv_timer_del(timer);
        return;
    }
    if (++state->poll_ticks > 30) {
        if (state->pending) crash_popup_show();
        state->poll_scheduled = false;
        lv_timer_del(timer);
    }
}

static void crash_popup_schedule_cb(void *arg) {
    (void)arg;
    crash_reporter_state_t *state = s_state;
    if (!state || !state->pending) return;
    if (!lv_timer_create(crash_popup_poll_cb, 400, NULL)) {
        state->poll_scheduled = false;
    }
}

#else

bool crash_reporter_handle_input(const void *event_data) {
    (void)event_data;
    return false;
}

#endif

void crash_reporter_init(void) {
#ifdef CONFIG_WITH_SCREEN
    if (!s_state) return;
    if (s_state->pending && !s_state->poll_scheduled) {
        s_state->poll_scheduled = true;
        display_manager_run_on_lvgl(crash_popup_schedule_cb, NULL);
    }
#else
    if (s_state && s_state->pending) {
        ESP_LOGW(TAG, "Previous boot crashed (reason: %s); coredump saved during boot",
                 s_state->reason[0] ? s_state->reason : "(no reason)");
    }
#endif
}
