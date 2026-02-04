
/*****
 * hal_i2c_manager.h
 * Minimal HAL I²C manager providing mutex-based locking for I²C access
 * to avoid contention between tasks when accessing expander/SD/RTC
 * [Created: 2026-02-04]
 *****/
#pragma once

#include <stdint.h>

namespace hal {

/**
 * Lock the I²C bus with a timeout.
 * @param timeout_ms Maximum time to wait for the lock (milliseconds)
 * @return true if lock acquired, false if timeout occurred
 * 
 * NOTE: Must NOT be called from ISR context. Use task notification pattern
 * to move I²C operations from ISR to task context.
 */
bool i2c_lock(uint32_t timeout_ms = 100);

/**
 * Unlock the I²C bus.
 * Must be called after i2c_lock() to release the mutex.
 */
void i2c_unlock();

/**
 * Initialize the I²C mutex.
 * Called once during startup.
 */
void i2c_init();

} // namespace hal

