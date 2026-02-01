/*
 * waveshare_io_port.cpp
 * 
 * Test code for Gate A and Gate B pushbutton inputs via Waveshare EXIO extender.
 * 
 * WIRING INSTRUCTIONS:
 * ====================
 * Connect pushbuttons between EXIO pins and GND with pull-ups enabled:
 * 
 *   Gate A: Pushbutton between EXIO0 and GND
 *   Gate B: Pushbutton between EXIO5 and GND
 * 
 * Use the CH422G internal pull-ups (recommended) or external 10kΩ resistors to 3.3V.
 * 
 * ASCII Wiring Diagram:
 * 
 *     3.3V (via internal pull-up or external 10kΩ)
 *       ↓
 *     EXIO0 ──┬── [Pushbutton A] ── GND
 *             │
 *           (to CH422G)
 * 
 *     3.3V (via internal pull-up or external 10kΩ)
 *       ↓
 *     EXIO5 ──┬── [Pushbutton B] ── GND
 *             │
 *           (to CH422G)
 * 
 * IMPORTANT: Do NOT connect 5V to EXIO pins. The CH422G operates at 3.3V logic levels.
 * 
 * The buttons are detected as active-low (pressed when bit == 0).
 */

#include "waveshare_io_port.h"
#include "../drivers/hal_panel.h"
#include <Arduino.h>

// Include IO expander C API for low-level access
#if defined(__has_include)
  #if __has_include(<port/esp_io_expander.h>)
    #include <port/esp_io_expander.h>
  #elif __has_include(<esp_io_expander.h>)
    #include <esp_io_expander.h>
  #endif
#endif

// Declare the get_level function if not already available
extern "C" {
  #ifndef ESP_OK
    #define ESP_OK 0
  #endif
  
  // Standard ESP IO Expander C API function for reading pin levels
  // This function is typically provided by the esp_io_expander library
  esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t handle, 
                                      uint32_t pin_mask, 
                                      uint32_t *level);
}

// Output pins for visual feedback (DO0 and DO1)
#define DO0 6
#define DO1 7

/**
 * waveshare_io_test()
 * 
 * Interactive test for Gate A and Gate B pushbuttons.
 * Detects button presses (active-low) and toggles output pins as visual feedback.
 * Exits when both gates have been pressed at least once.
 */
void waveshare_io_test() {
  Serial.println("\n=== Waveshare IO Port Test ===");
  Serial.println("Testing Gate A (EXIO0) and Gate B (EXIO5) pushbuttons");
  Serial.println("Press both buttons to complete the test.\n");
  
  // Wait for IO expander to be ready
  if (!hal::expander_wait_ready(1000)) {
    Serial.println("ERROR: CH422G expander not ready!");
    return;
  }
  
  // Get the raw expander handle
  esp_io_expander_handle_t expander = hal::expander_get_handle();
  if (!expander) {
    Serial.println("ERROR: Failed to get CH422G handle!");
    return;
  }
  
  Serial.println("CH422G expander ready.");
  
  // Configure Gate A and Gate B as inputs with pull-ups
  Serial.println("Configuring EXIO0 (Gate A) and EXIO5 (Gate B) as inputs...");
  
  // Set DI0 and DI1 as inputs
  if (!hal::expander_pinMode(PIN_GATE_A_EXP, false)) {  // false = input
    Serial.println("ERROR: Failed to configure Gate A as input!");
    return;
  }
  if (!hal::expander_pinMode(PIN_GATE_B_EXP, false)) {  // false = input
    Serial.println("ERROR: Failed to configure Gate B as input!");
    return;
  }
  
  // Configure DO0 and DO1 as outputs for visual feedback
  Serial.println("Configuring DO0 and DO1 as outputs for visual feedback...");
  if (!hal::expander_pinMode(DO0, true)) {  // true = output
    Serial.println("ERROR: Failed to configure DO0 as output!");
    return;
  }
  if (!hal::expander_pinMode(DO1, true)) {
    Serial.println("ERROR: Failed to configure DO1 as output!");
    return;
  }
  
  // Initialize outputs to LOW
  hal::expander_digitalWrite(DO0, false);
  hal::expander_digitalWrite(DO1, false);
  
  Serial.println("Configuration complete. Waiting for button presses...\n");
  
  // Event detection flags
  uint8_t events = 0;  // bit 0 = Gate A detected, bit 1 = Gate B detected
  const uint8_t EVENT_GATE_A = 0x01;
  const uint8_t EVENT_GATE_B = 0x02;
  const uint8_t EVENT_BOTH = EVENT_GATE_A | EVENT_GATE_B;
  
  // Main test loop
  while ((events & EVENT_BOTH) != EVENT_BOTH) {
    // Read both gate inputs using the C API
    uint32_t gate_states = 0;
    uint32_t pin_mask = PIN_GATE_A_mask | PIN_GATE_B_mask;
    
    // Read the state of both pins using esp_io_expander_get_level
    if (esp_io_expander_get_level(expander, pin_mask, &gate_states) == ESP_OK) {
      // Active-low detection: button pressed when bit is 0
      bool gate_a_pressed = !(gate_states & PIN_GATE_A_mask);
      bool gate_b_pressed = !(gate_states & PIN_GATE_B_mask);
      
      // Detect and report Gate A press
      if (gate_a_pressed && !(events & EVENT_GATE_A)) {
        events |= EVENT_GATE_A;
        Serial.println("✓ Gate A pressed (EXIO0)");
        hal::expander_digitalWrite(DO0, true);  // Turn on DO0
      }
      
      // Detect and report Gate B press
      if (gate_b_pressed && !(events & EVENT_GATE_B)) {
        events |= EVENT_GATE_B;
        Serial.println("✓ Gate B pressed (EXIO5)");
        hal::expander_digitalWrite(DO1, true);  // Turn on DO1
      }
    } else {
      // Read error - print debug message with troubleshooting hints
      Serial.println("Warning: Failed to read gate states from CH422G");
      Serial.println("  Possible causes: I2C communication failure, wiring issues,");
      Serial.println("  or expander not responding. Check connections and power.");
    }
    
    delay(50);  // Debounce delay
  }
  
  // Test complete
  Serial.println("\n=== Test Complete ===");
  Serial.println("Both Gate A and Gate B have been detected.");
  Serial.println("DO0 and DO1 outputs should be HIGH.");
  Serial.println("Check the hardware LEDs or use a multimeter to verify.\n");
  
  // Flash outputs to indicate completion
  for (int i = 0; i < 3; i++) {
    hal::expander_digitalWrite(DO0, false);
    hal::expander_digitalWrite(DO1, false);
    delay(200);
    hal::expander_digitalWrite(DO0, true);
    hal::expander_digitalWrite(DO1, true);
    delay(200);
  }
  
  Serial.println("Test finished. You can now use Gate A and Gate B in your application.");
}
