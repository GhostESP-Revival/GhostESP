#ifndef ATTAKY_BOARD_H
#define ATTAKY_BOARD_H

/* Attaky board bring-up for GhostESP.
 *
 * Owns everything that is true of the Attaky Core regardless of which
 * modules are attached: the shared I2C bus, the Core's two AW9523B expanders,
 * the capacitive-touch reset line that lives on one of them, and a presence
 * probe for each optional module. Module drivers (keyboard, gamepad) configure
 * their own expanders; this layer only probes them.
 *
 * Hardware facts come from the Attaky Cyber Deck Plus datasheet and the
 * public hardware catalog; see source/ATTAKY-PORT.md. */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Port identity, used by the chain-integration boot banner. */
#define ATTAKY_PORT_FIRMWARE_NAME "GhostESP"
#define ATTAKY_PORT_VERSION       "2.2.2-A003"
#define ATTAKY_PORT_BUILD_SKU     "Cyber Deck Plus 1.0"
#define ATTAKY_PORT_TOOLCHAIN     "ESP-IDF"

/* Probed modules. The Core's own devices are always expected; the rest are
 * optional and their absence is normal on a Core-only device. */
typedef enum {
    ATTAKY_MODULE_TOUCH = 0,     /* Core FT6636 capacitive touch @ 0x38 */
    ATTAKY_MODULE_EXPANDER_AGG,  /* Core AW9523 interrupt aggregator @ 0x58 */
    ATTAKY_MODULE_EXPANDER_BTN,  /* Core AW9523 buttons/LED/CTP reset @ 0x59 */
    ATTAKY_MODULE_FUEL_GAUGE,    /* Power: Standard Cell MAX17048 @ 0x36 */
    ATTAKY_MODULE_KEYBOARD,      /* Control: Keyboard AW9523 pair @ 0x5A/0x5B */
    ATTAKY_MODULE_GAMEPAD,       /* Control: Gamepad TCA9555 pair @ 0x22/0x26 */
    ATTAKY_MODULE_COUNT
} attaky_module_t;

/* Bring up the shared bus and Core expanders, release the touch reset, and
 * probe for optional modules. Safe to call once, early in app_main, before the
 * display manager initialises the panel and touch controller.
 *
 * Returns ESP_OK when the Core's own expanders answered; module absence never
 * makes this fail (the device is expected to run Core-only). */
esp_err_t attaky_board_init(void);

/* True when the module answered its presence probe. Always false before
 * attaky_board_init() has run. */
bool attaky_board_module_present(attaky_module_t module);

/* The shared main-bus handle (IO1/IO2), or NULL before init. Module drivers
 * use it instead of opening a second bus. */
void *attaky_board_i2c_bus(void);

/* Emit the boot banner pair plus the milestone marker on the console. Called
 * once from app_main after the serial console is up. */
void attaky_board_emit_banner(void);

/* Log which modules answered, in one line. */
void attaky_board_emit_presence(void);

/* Short name of milestone 1..8, for the marker line and logs. */
const char *attaky_milestone_name(int milestone);

#ifdef __cplusplus
}
#endif

#endif /* ATTAKY_BOARD_H */
