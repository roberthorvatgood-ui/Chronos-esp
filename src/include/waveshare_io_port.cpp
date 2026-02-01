/*****
 * waveshare_io_port.cpp
 * Waveshare EXIO Test Logic
 * [Created: 2026-02-01]
 *
 * Wiring instructions:
 * - Connect Button A: EXIO0 -----/ pushbutton /----- GND   (Gate A)
 * - Connect Button B: EXIO5 -----/ pushbutton /----- GND   (Gate B)
 * - Provide pull-ups (prefer expander internal pull-ups if supported,
 *   otherwise external 10 kΩ pull-ups to 3.3V).
 * - Do NOT connect 5V to EXIO pins.
 *****/
#include "waveshare_io_port.h"
#include <Arduino.h>
#include "../drivers/hal_panel.h"

// DO0 and DO1 outputs for visual feedback (expander bits 6 and 7)
#define DO0      6
#define DO0_mask (1u << DO0)
#define DO1      7
#define DO1_mask (1u << DO1)

// C API forward declarations (if not already included)
#if defined(__has_include)
  #if __has_include(<port/esp_io_expander.h>)
    #include <port/esp_io_expander.h>
  #elif __has_include(<esp_io_expander.h>)
    #include <esp_io_expander.h>
  #endif
#endif

// Forward declare get_level if not in headers
#ifndef ESP_IO_EXPANDER_H
extern "C" {
  esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t handle, uint32_t pin_mask, uint32_t *level);
}
#endif

void waveshare_io_test() {
  Serial.println("[EXIO Test] Starting Gate A/B pushbutton test...");
  Serial.println("[EXIO Test] Wiring: EXIO0 (Gate A) and EXIO5 (Gate B) to GND via pushbuttons");
  Serial.println("[EXIO Test] Buttons are active-low (pressed when bit == 0)");
  
  // Wait for CH422G expander to be ready
  if (!hal::expander_wait_ready(1000)) {
    Serial.println("[EXIO Test] ERROR: CH422G expander not ready");
    return;
  }
  
  // Get the expander handle
  esp_io_expander_handle_t expander = hal::expander_get_handle();
  if (!expander) {
    Serial.println("[EXIO Test] ERROR: Cannot get expander handle");
    return;
  }
  
  // Configure DI0 (EXIO0 / Gate A) and DI1 (EXIO5 / Gate B) as inputs
  // Note: esp_io_expander_set_dir uses IO_EXPANDER_INPUT for inputs
  if (esp_io_expander_set_dir(expander, PIN_GATE_A_mask | PIN_GATE_B_mask, IO_EXPANDER_INPUT) != ESP_OK) {
    Serial.println("[EXIO Test] ERROR: Failed to configure Gate A/B as inputs");
    return;
  }
  
  // Configure DO0 and DO1 as outputs for visual feedback
  if (esp_io_expander_set_dir(expander, DO0_mask | DO1_mask, IO_EXPANDER_OUTPUT) != ESP_OK) {
    Serial.println("[EXIO Test] ERROR: Failed to configure DO0/DO1 as outputs");
    return;
  }
  
  // Initialize outputs to LOW
  esp_io_expander_set_level(expander, DO0_mask | DO1_mask, 0);
  
  Serial.println("[EXIO Test] Configuration complete. Press Gate A and Gate B buttons...");
  
  // Events counter (bit 0 = Gate A pressed, bit 1 = Gate B pressed)
  uint8_t events = 0;
  
  while ((events & 0x03) != 0x03) { // Exit when both gates have been pressed
    // Read both gate inputs at once
    uint32_t input_state = 0;
    if (esp_io_expander_get_level(expander, PIN_GATE_A_mask | PIN_GATE_B_mask, &input_state) != ESP_OK) {
      Serial.println("[EXIO Test] ERROR: Failed to read inputs");
      delay(100);
      continue;
    }
    
    // Active-low detection: pressed when bit == 0
    bool gate_a_pressed = !(input_state & PIN_GATE_A_mask);
    bool gate_b_pressed = !(input_state & PIN_GATE_B_mask);
    
    // Detect new presses and update events
    if (gate_a_pressed && !(events & 0x01)) {
      events |= 0x01;
      Serial.println("[EXIO Test] Gate A pressed!");
      // Set DO0 HIGH as visual feedback
      esp_io_expander_set_level(expander, DO0_mask, 1);
    }
    
    if (gate_b_pressed && !(events & 0x02)) {
      events |= 0x02;
      Serial.println("[EXIO Test] Gate B pressed!");
      // Set DO1 HIGH as visual feedback
      esp_io_expander_set_level(expander, DO1_mask, 1);
    }
    
    delay(50); // Debounce delay
  }
  
  Serial.println("[EXIO Test] Both gates detected! Test complete.");
  Serial.println("[EXIO Test] DO0 and DO1 should now be HIGH.");
  
  // Keep outputs on for 2 seconds as confirmation
  delay(2000);
  
  // Turn off outputs
  esp_io_expander_set_level(expander, DO0_mask | DO1_mask, 0);
  Serial.println("[EXIO Test] Test finished.");
}
