#include "hal_i2c_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"   // for xPortInIsrContext() / ISR detection
#include "esp_log.h"

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static const char *TAG = "hal:i2c";

namespace hal {

bool i2c_lock(uint32_t timeout_ms) {
  // Safety check: refuse to block when called from ISR context.
  // This prevents accidental deadlocks / WDT when code calls i2c_lock from within an ISR.
  #if defined(xPortInIsrContext)
    if (xPortInIsrContext()) {
      ESP_LOGE(TAG, "i2c_lock called from ISR context! Refusing to block.");
      return false;
    }
  #endif

  if (!s_i2c_mutex) {
    s_i2c_mutex = xSemaphoreCreateMutex();
    if (!s_i2c_mutex) {
      ESP_LOGE(TAG, "failed to create i2c mutex");
      return false;
    }
  }
  const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(s_i2c_mutex, ticks) == pdTRUE) return true;
  ESP_LOGW(TAG, "i2c_lock timeout (%u ms)", (unsigned)timeout_ms);
  return false;
}

void i2c_unlock() {
  if (s_i2c_mutex) xSemaphoreGive(s_i2c_mutex);
}

} // namespace hal