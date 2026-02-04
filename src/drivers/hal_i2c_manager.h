/*****
 * hal_i2c_manager.h
 * HAL I2C Manager – FreeRTOS mutex for I2C bus access serialization
 * [Created: 2026-02-04]
 * Prevents contention between expander, RTC, and SD operations
 *****/
#pragma once

#include <stdint.h>

namespace hal {

/**
 * Initialize the I2C manager (creates the FreeRTOS mutex).
 * Should be called once during system initialization.
 */
void i2c_manager_init();

/**
 * Acquire the I2C bus lock.
 * @param timeout_ms Maximum time to wait for the lock (milliseconds)
 * @return true if lock acquired, false if timeout
 * 
 * WARNING: Do NOT call from ISR context!
 */
bool i2c_lock(uint32_t timeout_ms = 100);

/**
 * Release the I2C bus lock.
 * Must be called after i2c_lock() to release the mutex.
 */
void i2c_unlock();

} // namespace hal
