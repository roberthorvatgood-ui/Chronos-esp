
/*****
 * hal_i2c_manager.cpp
 * Minimal HAL I²C manager implementation with FreeRTOS mutex
 * [Created: 2026-02-04]
 *****/
#include "hal_i2c_manager.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hal {

static SemaphoreHandle_t s_i2c_mutex = nullptr;

void i2c_init() {
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (!s_i2c_mutex) {
            Serial.println("[HAL][I2C] ERROR: Failed to create I2C mutex!");
        } else {
            Serial.println("[HAL][I2C] Mutex initialized");
        }
    }
}

bool i2c_lock(uint32_t timeout_ms) {
    if (!s_i2c_mutex) {
        Serial.println("[HAL][I2C] WARNING: Mutex not initialized, proceeding without lock");
        return true; // Fail-safe: allow operation to proceed
    }

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_i2c_mutex, ticks) == pdTRUE) {
        return true;
    }

    // Timeout occurred
    Serial.printf("[HAL][I2C] WARNING: Lock timeout after %lu ms\n", (unsigned long)timeout_ms);
    return false;
}

void i2c_unlock() {
    if (!s_i2c_mutex) {
        return; // No mutex to unlock
    }

    if (xSemaphoreGive(s_i2c_mutex) != pdTRUE) {
        Serial.println("[HAL][I2C] ERROR: Failed to release mutex (not held?)");
    }
}

} // namespace hal

