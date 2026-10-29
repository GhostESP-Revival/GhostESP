#ifndef ATTAKY_KEYBOARD_H
#define ATTAKY_KEYBOARD_H

/* Control: Keyboard (Bottom) 1.0 driver.
 *
 * The module is one 48-key QWERTY unit split electrically into two 5x5
 * matrices, each scanned by its own AW9523B expander (left 0x5A, right 0x5B).
 * Rows are P10-P14 (driven low one at a time), columns are P00-P04 (read,
 * pressed = low), and each half's RGB status LED hangs off P15/P16/P17.
 *
 * Key events are pushed onto the shared input queue as INPUT_TYPE_KEYBOARD,
 * exactly like the Cardputer / T-Deck keyboard paths, so no view code changes.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the matrix scan task. Must be called after display_manager has created
 * the shared input queue. Returns ESP_OK when at least one half was found;
 * ESP_ERR_NOT_FOUND when the module is absent (not an error for the caller -
 * a Core-only device has no keyboard). */
esp_err_t attaky_keyboard_init(void);

/* True once the scan task is running. */
bool attaky_keyboard_active(void);

#ifdef __cplusplus
}
#endif

#endif /* ATTAKY_KEYBOARD_H */
