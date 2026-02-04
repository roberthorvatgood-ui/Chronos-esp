/*****
 * hal_i2c_manager.cpp
 * HAL I2C Manager – FreeRTOS mutex for I2C bus access serialization
 * [Created: 2026-02-04]
 * Prevents contention between expander, RTC, and SD operations
 *****/

#include "hal_i2c_manager.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hal {

// FreeRTOS mutex for I2C bus access
static SemaphoreHandle_t s_i2c_mutex = nullptr;

void i2c_manager_init() {
  if (s_i2c_mutex) return; // Already initialized
  
  s_i2c_mutex = xSemaphoreCreateMutex();
  if (s_i2c_mutex) {
    Serial.println("[HAL][I2C] Mutex initialized");
  } else {
    Serial.println("[HAL][I2C] ERROR: Failed to create mutex!");
  }
}

bool i2c_lock(uint32_t timeout_ms) {
  if (!s_i2c_mutex) {
    Serial.println("[HAL][I2C] WARNING: Mutex not initialized, call i2c_manager_init()");
    return false;
  }
  
  const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(s_i2c_mutex, ticks) == pdTRUE) {
    return true;
  }
  
  // Timeout occurred
  Serial.printf("[HAL][I2C] WARNING: Lock timeout after %lu ms\n", (unsigned long)timeout_ms);
  return false;
}

void i2c_unlock() {
  if (!s_i2c_mutex) {
    Serial.println("[HAL][I2C] WARNING: Attempting to unlock uninitialized mutex");
    return;
  }
  
  xSemaphoreGive(s_i2c_mutex);
}

} // namespace hal
