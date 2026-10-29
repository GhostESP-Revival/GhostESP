#ifndef ATTAKY_GAMEPAD_H
#define ATTAKY_GAMEPAD_H

/* Control: Gamepad (Bottom) 1.0 driver.
 *
 * Two TCA9555 expanders, one per half: left 0x22 (D-pad, L1/L2, FN_L1/FN_L2)
 * and right 0x26 (A/B/X/Y, R1/R2, FN_R1/FN_R2). All buttons are active-low
 * with the expander's internal pull-ups.
 *
 * The D-pad and A ride the existing joystick navigation path (pressed/released
 * events on the shared input queue, the same shape the AtomS3R single-button
 * path uses); B/X/Y/R1 are delivered as keyboard events. Both halves are
 * polled, so the module's shared R_INT line (wired together with the
 * keyboard's right half on the FPC bus) never needs to be demultiplexed.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the button poll task. Must be called after display_manager has created
 * the shared input queue. ESP_ERR_NOT_FOUND when the module is absent. */
esp_err_t attaky_gamepad_init(void);

bool attaky_gamepad_active(void);

#ifdef __cplusplus
}
#endif

#endif /* ATTAKY_GAMEPAD_H */
