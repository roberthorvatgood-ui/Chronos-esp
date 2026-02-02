#include "src/drivers/hal_panel.h"
#include <Arduino.h>

#if defined(__has_include)
  #if __has_include(<port/esp_io_expander.h>)
    #include <port/esp_io_expander.h>
  #elif __has_include(<esp_io_expander.h>)
    #include <esp_io_expander.h>
  #endif
#endif

// Declare candidate C API read functions as weak symbols. Signatures match the
// esp_io_expander port header used in this project:
//   esp_err_t esp_io_expander_get_level(handle, pin_num_mask, level_mask_out)
//
// Do NOT redefine esp_err_t here (it is already provided by ESP-IDF headers).

extern "C" {
  extern esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t, uint32_t, uint32_t*) __attribute__((weak));
  extern esp_err_t esp_io_expander_read_levels(esp_io_expander_handle_t, uint32_t, uint32_t*) __attribute__((weak));
  extern esp_err_t esp_io_expander_read(esp_io_expander_handle_t, uint32_t, uint32_t*) __attribute__((weak));
  extern esp_err_t esp_io_expander_get_levels(esp_io_expander_handle_t, uint32_t, uint32_t*) __attribute__((weak));
  extern esp_err_t esp_io_expander_get_input_level(esp_io_expander_handle_t, uint32_t, uint32_t*) __attribute__((weak));
}

namespace hal {

// Try available C API read functions and return masked bits (1 == HIGH).
// Return 0 on failure (caller treats 0 as a safe abort/diagnostic).
uint32_t expander_multiDigitalRead(uint32_t mask) {
  esp_io_expander_handle_t h = expander_get_handle();
  if (!h) return 0;

  uint32_t levels = 0;

  if (esp_io_expander_get_level) {
    if (esp_io_expander_get_level(h, mask, &levels) == ESP_OK) return (levels & mask);
  }
  if (esp_io_expander_read_levels) {
    if (esp_io_expander_read_levels(h, mask, &levels) == ESP_OK) return (levels & mask);
  }
  if (esp_io_expander_read) {
    if (esp_io_expander_read(h, mask, &levels) == ESP_OK) return (levels & mask);
  }
  if (esp_io_expander_get_levels) {
    if (esp_io_expander_get_levels(h, mask, &levels) == ESP_OK) return (levels & mask);
  }
  if (esp_io_expander_get_input_level) {
    if (esp_io_expander_get_input_level(h, mask, &levels) == ESP_OK) return (levels & mask);
  }

  // No usable API present or all calls failed.
  return 0;
}

} // namespace hal