#ifndef BLE_BRIDGE_MANAGER_H
#define BLE_BRIDGE_MANAGER_H

#include <stdbool.h>

void ble_bridge_handle_command(int argc, char **argv);
bool ble_bridge_start(void);
bool ble_bridge_is_running(void);
bool ble_bridge_get_enabled(void);
bool ble_bridge_set_enabled(bool enabled);
void ble_bridge_apply_saved_enabled(void);
void ble_bridge_stop(void);

#endif // BLE_BRIDGE_MANAGER_H
