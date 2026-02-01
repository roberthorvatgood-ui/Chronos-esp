/**
 * Gate Input Example for Chronos ESP32-S3
 * 
 * Demonstrates how to use Gate A and Gate B inputs with:
 * - Hardware interrupts (attachInterrupt)
 * - Microsecond-precision timestamps (micros())
 * - Simple software debounce
 * - IRAM_ATTR for ESP32 ISR compatibility
 * 
 * Hardware Setup:
 * - Connect normally-open pushbutton between EXIO0 and GND for Gate A
 * - Connect normally-open pushbutton between EXIO5 and GND for Gate B
 * - Uses internal pull-up resistors (INPUT_PULLUP)
 * - No external pull-ups required
 * - DO NOT connect 5V to the buttons
 * 
 * Pin Mapping (verify with Waveshare ESP32-S3-Touch-LCD-5 schematic):
 * - EXIO0 -> GPIO (see BUTTON_GATE_A in config.h)
 * - EXIO5 -> GPIO (see BUTTON_GATE_B in config.h)
 */

#include <Arduino.h>

// Pin definitions - update these to match your actual GPIO mappings
// Consult the Waveshare ESP32-S3-Touch-LCD-5 documentation
#define PIN_GATE_A 15  // EXIO0 - verify this GPIO number!
#define PIN_GATE_B 16  // EXIO5 - verify this GPIO number!

// Debounce settings
#define DEBOUNCE_MICROS 50000  // 50000us (50ms) debounce time

// Volatile variables for ISR (Interrupt Service Routine)
volatile uint64_t lastGateA_us = 0;
volatile uint64_t lastGateB_us = 0;
volatile uint32_t gateA_count = 0;
volatile uint32_t gateB_count = 0;

// Last debounced timestamp to avoid multiple triggers
volatile uint64_t lastGateA_debounced_us = 0;
volatile uint64_t lastGateB_debounced_us = 0;

/**
 * ISR for Gate A falling edge (button press)
 * IRAM_ATTR ensures the function is in IRAM for fast interrupt response on ESP32
 */
void IRAM_ATTR gateA_ISR() {
  uint64_t now = micros();
  
  // Simple debounce: ignore if less than DEBOUNCE_MICROS since last trigger
  if (now - lastGateA_debounced_us > DEBOUNCE_MICROS) {
    lastGateA_us = now;
    lastGateA_debounced_us = now;
    gateA_count++;
  }
}

/**
 * ISR for Gate B falling edge (button press)
 * IRAM_ATTR ensures the function is in IRAM for fast interrupt response on ESP32
 */
void IRAM_ATTR gateB_ISR() {
  uint64_t now = micros();
  
  // Simple debounce: ignore if less than DEBOUNCE_MICROS since last trigger
  if (now - lastGateB_debounced_us > DEBOUNCE_MICROS) {
    lastGateB_us = now;
    lastGateB_debounced_us = now;
    gateB_count++;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== Chronos Gate Input Example ===");
  Serial.println("Gate A: EXIO0 (pushbutton to GND)");
  Serial.println("Gate B: EXIO5 (pushbutton to GND)");
  Serial.println("Using internal pull-ups, interrupts on FALLING edge\n");
  
  // Configure pins with internal pull-up resistors
  pinMode(PIN_GATE_A, INPUT_PULLUP);
  pinMode(PIN_GATE_B, INPUT_PULLUP);
  
  // Attach interrupts on FALLING edge (button press, active LOW)
  attachInterrupt(digitalPinToInterrupt(PIN_GATE_A), gateA_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_GATE_B), gateB_ISR, FALLING);
  
  Serial.println("Setup complete. Press the buttons to test...\n");
}

void loop() {
  static uint32_t lastReportMs = 0;
  uint32_t now = millis();
  
  // Report status every 2 seconds
  if (now - lastReportMs >= 2000) {
    lastReportMs = now;
    
    // Create local copies of volatile variables for safe printing
    uint64_t gateA_ts = lastGateA_us;
    uint64_t gateB_ts = lastGateB_us;
    uint32_t countA = gateA_count;
    uint32_t countB = gateB_count;
    
    Serial.println("--- Status Report ---");
    Serial.printf("Gate A: count=%lu, last_us=%llu\n", countA, gateA_ts);
    Serial.printf("Gate B: count=%lu, last_us=%llu\n", countB, gateB_ts);
    
    // Calculate time interval between last A and B triggers
    if (gateA_ts > 0 && gateB_ts > 0) {
      int64_t interval_us = (int64_t)gateB_ts - (int64_t)gateA_ts;
      Serial.printf("Interval (B-A): %lld us (%.3f ms)\n", 
                    interval_us, interval_us / 1000.0);
    }
    Serial.println();
  }
  
  delay(10);
}

/**
 * WIRING INSTRUCTIONS FOR WAVESHARE ESP32-S3-Touch-LCD-5
 * 
 * The EXIO header provides extended GPIO pins. Typical layout:
 * 
 *   EXIO Header (verify with your board):
 *   [EXIO0] [EXIO1] [EXIO2] [EXIO3] [EXIO4] [EXIO5] ... [GND] [3V3]
 * 
 * To wire pushbuttons:
 * 1. Connect one terminal of pushbutton to EXIO0
 * 2. Connect other terminal of pushbutton to GND on EXIO header
 * 3. Repeat for Gate B using EXIO5
 * 
 * Important notes:
 * - Use normally-open (NO) pushbuttons
 * - DO NOT connect 5V - only use GND
 * - Internal pull-ups are enabled in code (no external resistors needed)
 * - Verify EXIO0 and EXIO5 GPIO numbers from Waveshare documentation
 * - Check board schematic for actual GPIO mapping
 * 
 * To find GPIO numbers:
 * 1. Visit Waveshare ESP32-S3-Touch-LCD-5 product page
 * 2. Download the schematic PDF or pinout diagram
 * 3. Locate EXIO0 and EXIO5 in the schematic
 * 4. Note the GPIO numbers they connect to
 * 5. Update PIN_GATE_A and PIN_GATE_B above
 * 
 * Example schematic check:
 * - Look for "EXIO0" label -> might show "GPIO_15" or similar
 * - Look for "EXIO5" label -> might show "GPIO_16" or similar
 * - Update the #define statements to match actual values
 */
