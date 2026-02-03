#include "waveshare_io_port.h"

// 2026-02-03 20:48 CET — Robert H. / Copilot
// Implement non-blocking, debounced IO gate sampling for CH422G.
// Exposes an init + periodic sample API and query functions for Gate A (IO0) and Gate B (IO1).

#include <Arduino.h>
#include <esp_io_expander.hpp>   // Provided by your project

static esp_expander::CH422G* expander = nullptr;

static constexpr uint16_t SAMPLE_PERIOD_MS = 200; // sampling period
static constexpr uint8_t  STABLE_COUNT      = 3;   // debounce count

// Internal debouncing state:
static uint8_t stableA_samples = 0, stableB_samples = 0;
static bool lastA_sample = true, lastB_sample = true; // last raw sample (true = HIGH)
static bool gateA_debounced = true, gateB_debounced = true; // debounced (true = HIGH/open)
static uint32_t lastSampleMs = 0;

// Read the whole input bank once (8 bits). Some libs ignore masks and return full byte anyway.
static inline uint8_t readInputsRaw() {
  if (!expander) return 0xFF; // consider inputs high if not initialized
  return expander->multiDigitalRead(0xFF);
}

// Initialize the expander (safe to call multiple times)
bool waveshare_io_init() {
  if (expander) return true;

  expander = new esp_expander::CH422G(EXAMPLE_I2C_SCL_PIN, EXAMPLE_I2C_SDA_PIN, EXAMPLE_I2C_ADDR);
  if (!expander) {
    Serial.println(F("[CH422G] ERROR: Allocation failed"));
    return false;
  }
  expander->init();
  expander->begin();

  // Configure IO direction
  expander->enableOC_PushPull();   // harmless even if OC pins unused
  expander->enableAllIO_Input();   // IO0..IO7 as inputs

  // Initialize debounce state from a snapshot
  uint8_t snap = readInputsRaw();
  lastA_sample = gateA_debounced = (snap & DI0_mask) != 0;
  lastB_sample = gateB_debounced = (snap & DI1_mask) != 0;
  stableA_samples = STABLE_COUNT;
  stableB_samples = STABLE_COUNT;
  lastSampleMs = millis();

  Serial.println(F("[CH422G] IO expander initialized for Gate A (IO0) / Gate B (IO1)."));
  return true;
}

// Call periodically from loop() — this function is cheap and will only sample every SAMPLE_PERIOD_MS
void waveshare_io_sample() {
  if (!expander) return; // nothing to do

  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_PERIOD_MS) return;
  lastSampleMs = now;

  uint8_t snap = readInputsRaw();

  // Debounce A (IO0)
  bool sampleA = (snap & DI0_mask) != 0;
  if (sampleA == lastA_sample) {
    if (stableA_samples < 255) stableA_samples++;
  } else {
    stableA_samples = 1;
    lastA_sample = sampleA;
  }
  if (stableA_samples >= STABLE_COUNT) gateA_debounced = sampleA;

  // Debounce B (IO1)
  bool sampleB = (snap & DI1_mask) != 0;
  if (sampleB == lastB_sample) {
    if (stableB_samples < 255) stableB_samples++;
  } else {
    stableB_samples = 1;
    lastB_sample = sampleB;
  }
  if (stableB_samples >= STABLE_COUNT) gateB_debounced = sampleB;
}

// Query functions:
// The CH422G inputs are active-LOW (buttons/gates pull to GND when active).
// These functions return true when the gate is CLOSED (i.e. input reads LOW).
bool waveshare_gateA_closed() {
  // If not initialized try to init on first use (convenience)
  if (!expander) waveshare_io_init();
  // debounced state: true means HIGH/open; invert to produce CLOSED on LOW
  return !gateA_debounced;
}
bool waveshare_gateB_closed() {
  if (!expander) waveshare_io_init();
  return !gateB_debounced;
}

// Legacy blocking test kept for compatibility with your old test sketch.
// It will block (infinite loop) and print states every SAMPLE_PERIOD_MS.
// You can keep it, but prefer using waveshare_io_init() + waveshare_io_sample() in production.
void waveshare_io_test() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("[CH422G] GATE monitor starting (IO0=Gate A, IO1=Gate B)..."));

  // Ensure init done
  if (!waveshare_io_init()) {
    Serial.println(F("[CH422G] ERROR: init failed"));
    return;
  }

  // Continuous blocking monitor
  for (;;) {
    waveshare_io_sample();
    Serial.print(F("GATE A: "));
    Serial.print(gateA_debounced ? F("OPEN") : F("CLOSED"));
    Serial.print(F("    |    GATE B: "));
    Serial.println(gateB_debounced ? F("OPEN") : F("CLOSED"));
    delay(SAMPLE_PERIOD_MS);
  }
}
