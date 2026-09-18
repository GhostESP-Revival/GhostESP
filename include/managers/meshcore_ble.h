// meshcore_ble.h
// NimBLE GATT transport for the MeshCore companion protocol.
//
// MeshCore apps use a Nordic-UART-style service (one frame per characteristic
// write/notification). Verified against firmware v1.17.1
// src/helpers/esp32/SerialBLEInterface.cpp and docs.meshcore.io.
//
//   Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX      6E400002-...  (app -> firmware, WRITE)
//   TX      6E400003-...  (firmware -> app, NOTIFY)

#ifndef MESHCORE_BLE_H
#define MESHCORE_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool mc_ble_start(void);
void mc_ble_stop(void);
bool mc_ble_is_advertising(void);
bool mc_ble_is_connected(void);
bool mc_ble_is_linked(void); // TX characteristic subscribed

// Send one companion frame to the app (no-op returns false when not linked).
bool mc_ble_notify(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_BLE_H
