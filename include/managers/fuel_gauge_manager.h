#ifndef FUEL_GAUGE_MANAGER_H
#define FUEL_GAUGE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t percentage;
    uint16_t voltage_mv;
    int16_t current_ma;
    uint16_t capacity_mah;
    uint16_t remaining_capacity_mah;
    bool is_charging;
    bool is_initialized;
} fuel_gauge_data_t;

/**
 * @brief Initialize the fuel gauge manager
 * @return true if initialization successful, false otherwise
 */
bool fuel_gauge_manager_init(void);

/**
 * @brief Get current fuel gauge data
 * @param data Pointer to fuel_gauge_data_t structure to fill
 * @return true if data read successfully, false otherwise
 */
bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data);

/**
 * @brief Get battery percentage
 * @return Battery percentage (0-100), -1 if error
 */
int fuel_gauge_manager_get_percentage(void);

/**
 * @brief Check if battery is charging
 * @return true if charging, false otherwise
 */
bool fuel_gauge_manager_is_charging(void);

/**
 * @brief Get battery voltage in millivolts
 * @return Voltage in mV, 0 if error
 */
uint16_t fuel_gauge_manager_get_voltage_mv(void);

/**
 * @brief Deinitialize the fuel gauge manager
 */
void fuel_gauge_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // FUEL_GAUGE_MANAGER_H