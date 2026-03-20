#ifndef BLE_BRIDGE_MANAGER_H
#define BLE_BRIDGE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BLE_BRIDGE_MODE_DISABLED = 0,
    BLE_BRIDGE_MODE_PERIPHERAL = 1,
} ble_bridge_mode_t;

typedef enum {
    BLE_BRIDGE_STATE_IDLE = 0,
    BLE_BRIDGE_STATE_ADVERTISING,
    BLE_BRIDGE_STATE_CONNECTED,
    BLE_BRIDGE_STATE_BONDED,
    BLE_BRIDGE_STATE_PEER_CONNECTED,
} ble_bridge_state_t;

void ble_bridge_init(void);
void ble_bridge_deinit(void);

bool ble_bridge_start(ble_bridge_mode_t mode);
void ble_bridge_stop(void);
ble_bridge_state_t ble_bridge_get_state(void);
bool ble_bridge_is_active(void);

void ble_bridge_set_name(const char *name);
const char *ble_bridge_get_name(void);

void ble_bridge_set_bonding_required(bool required);
bool ble_bridge_is_bonding_required(void);

void ble_bridge_forget_bonds(void);
void ble_bridge_open_pairing_window(uint32_t duration_ms);

void ble_bridge_on_ghostlink_output(const uint8_t *data, size_t len);
void ble_bridge_on_app_input(const uint8_t *data, size_t len);

#endif // BLE_BRIDGE_MANAGER_H
