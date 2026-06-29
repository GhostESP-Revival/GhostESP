#ifndef SPLASH_SCREEN_H
#define SPLASH_SCREEN_H

#include "lvgl.h"
#include "managers/display_manager.h"

/**
 * @brief Creates the splash screen view.
 */
void splash_create(void);

/**
 * @brief Destroys the splash screen view.
 */
void splash_destroy(void);

/**
 * @brief Splash screen view object.
 */
extern View splash_view;

/**
 * @brief Update the boot progress bar and status label.
 *
 * Thread-safe. Internally marshals to the LVGL task.
 *
 * @param pct   0.0 - 100.0 to set the bar value. Pass a negative value
 *              to enable an indeterminate pulse animation. Respects the
 *              reduced-motion setting (no pulse when enabled).
 * @param label Short status string displayed above the bar. NULL keeps the
 *              previous label. Truncated to 31 characters.
 */
void splash_set_progress(float pct, const char *label);

/**
 * @brief Signal that all boot-time background work is complete.
 *
 * The splash will fade out once the minimum on-screen hold has elapsed.
 * A hard timeout also triggers fade-out even when this is not called, so
 * a stuck background step never leaves the splash up indefinitely.
 */
void splash_signal_completion(void);

#endif /* SPLASH_SCREEN_H */