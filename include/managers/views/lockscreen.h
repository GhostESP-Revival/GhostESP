#ifndef LOCKSCREEN_H
#define LOCKSCREEN_H

#include "managers/display_manager.h"
#include <stdbool.h>

/**
 * @brief Creates the lockscreen view.
 */
void lockscreen_create(void);

/**
 * @brief Destroys the lockscreen view.
 */
void lockscreen_destroy(void);

/**
 * @brief Reset lockscreen input state (call before switching to it).
 */
void lockscreen_reset_input(void);

/**
 * @brief Force the lockscreen into PIN setup mode on next create.
 */
void lockscreen_enter_setup(void);

/**
 * @brief Select whether the next lockscreen_create() builds as a floating
 * overlay (on lv_layer_top(), leaving the current view alive underneath) or as
 * a normal full-screen view. Overlay mode is used by the wake / auto-lock flow
 * so that an active capture (wardriving, sniffing, ...) keeps running while the
 * screen is locked. Cleared automatically on unlock.
 */
void lockscreen_set_overlay_mode(bool on);

/**
 * @brief Check if a PIN has been configured.
 */
bool lockscreen_is_configured(void);

/**
 * @brief Lockscreen view object.
 */
extern View lockscreen_view;

#endif // LOCKSCREEN_H
