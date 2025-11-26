#pragma once

#include <stdbool.h>

// Acquire a shared mutex for the given I2C port if the port is 0.
// Returns true if a mutex was taken and must be released via i2c_bus_unlock.
bool i2c_bus_lock(int port, int timeout_ms);

// Release the shared mutex previously taken by i2c_bus_lock.
void i2c_bus_unlock(int port);

// Return a pointer to the shared mutex array so other I2C clients can follow the same locking scheme.
void *i2c_bus_get_lock_handle(void);


