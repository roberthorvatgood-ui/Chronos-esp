
/*****
 * hal_i2c_manager.cpp
 * Minimal I2C bus mutex implementation using FreeRTOS
 * [Created: 2026-02-04]
 *****/
#include "hal_i2c_manager.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>

static const char* TAG = "hal_i2c";

namespace hal {

namespace {
    SemaphoreHandle_t s_i2c_mutex = nullptr;
    
    void ensure_mutex_created() {
        if (!s_i2c_mutex) {
            s_i2c_mutex = xSemaphoreCreateMutex();
            if (s_i2c_mutex) {
                ESP_LOGI(TAG, "I2C mutex created");
            } else {
                ESP_LOGE(TAG, "Failed to create I2C mutex!");
            }
        }
    }
} // anonymous namespace

bool i2c_lock(uint32_t timeout_ms) {
    ensure_mutex_created();
    
    if (!s_i2c_mutex) {
        ESP_LOGW(TAG, "i2c_lock: mutex not available");
        return false;
    }
    
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    
    if (xSemaphoreTake(s_i2c_mutex, ticks) == pdTRUE) {
        return true;
    } else {
        ESP_LOGW(TAG, "i2c_lock timeout after %lu ms", (unsigned long)timeout_ms);
        return false;
    }
}

void i2c_unlock() {
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

} // namespace hal
