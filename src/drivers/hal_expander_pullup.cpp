#include "src/drivers/hal_panel.h"
#include <Arduino.h>

#if defined(__has_include)
  #if __has_include(<port/esp_io_expander.h>)
    #include <port/esp_io_expander.h>
  #elif __has_include(<esp_io_expander.h>)
    #include <esp_io_expander.h>
  #endif
#endif

// Declare candidate C API functions as weak symbols (no typedef redefinitions).
// Signatures we attempt to call (common variants across ports):
//   esp_err_t esp_io_expander_set_pull(handle, pin_mask, pull_mode)
//   esp_err_t esp_io_expander_set_pull_mode(handle, pin_mask, pull_mode)
//   esp_err_t esp_io_expander_enable_pullup(handle, pin_mask, enable)
//   esp_err_t esp_io_expander_set_input_pull(handle, pin_mask, mode)
//
// We treat return == 0 as success (ESP_OK).

extern "C" {
  extern int esp_io_expander_set_pull(esp_io_expander_handle_t, uint32_t, int) __attribute__((weak));
  extern int esp_io_expander_set_pull_mode(esp_io_expander_handle_t, uint32_t, int) __attribute__((weak));
  extern int esp_io_expander_enable_pullup(esp_io_expander_handle_t, uint32_t, int) __attribute__((weak));
  extern int esp_io_expander_set_input_pull(esp_io_expander_handle_t, uint32_t, int) __attribute__((weak));
}

namespace hal {

bool expander_enablePullup(uint8_t exio, bool enable) {
  esp_io_expander_handle_t h = expander_get_handle();
  if (!h) {
    Serial.println("[HAL_PULL] No expander handle (not attached)");
    return false;
  }

  uint32_t mask = (1u << exio);
  int arg = enable ? 1 : 0;

  // Try the common candidate names. Treat 0 as success.
  if (esp_io_expander_set_pull) {
    int r = esp_io_expander_set_pull(h, mask, arg);
    Serial.printf("[HAL_PULL] tried set_pull -> res=%d\n", r);
    return (r == 0);
  }
  if (esp_io_expander_set_pull_mode) {
    int r = esp_io_expander_set_pull_mode(h, mask, arg);
    Serial.printf("[HAL_PULL] tried set_pull_mode -> res=%d\n", r);
    return (r == 0);
  }
  if (esp_io_expander_enable_pullup) {
    int r = esp_io_expander_enable_pullup(h, mask, arg);
    Serial.printf("[HAL_PULL] tried enable_pullup -> res=%d\n", r);
    return (r == 0);
  }
  if (esp_io_expander_set_input_pull) {
    int r = esp_io_expander_set_input_pull(h, mask, arg);
    Serial.printf("[HAL_PULL] tried set_input_pull -> res=%d\n", r);
    return (r == 0);
  }

  Serial.println("[HAL_PULL] No pull-up API detected in expander port");
  return false;
}

} // namespace hal