
/*****
 * hal_i2c_manager.h
 * Minimal I2C bus mutex to serialize access and prevent contention
 * [Created: 2026-02-04]
 *****/
#pragma once

#include <cstdint>

namespace hal {

/**
 * Acquire the I2C bus lock (mutex).
 * 
 * @param timeout_ms Maximum time to wait for the lock (default: 50ms)
 * @return true if lock acquired, false if timeout
 * 
 * @note NOT safe to call from ISR context. ISRs should notify tasks instead.
 */
bool i2c_lock(uint32_t timeout_ms = 50);

/**
 * Release the I2C bus lock.
 * Must be called from the same task that acquired the lock.
 */
void i2c_unlock();

} // namespace hal
