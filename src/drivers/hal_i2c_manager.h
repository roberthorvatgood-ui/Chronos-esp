#pragma once
#include <stdint.h>

namespace hal {
  // Acquire HAL I2C mutex (blocks up to timeout_ms). Returns true if lock acquired.
  // NOTE: Do NOT call from ISR context.
  bool i2c_lock(uint32_t timeout_ms = 50);

  // Release HAL I2C mutex.
  void i2c_unlock();
}