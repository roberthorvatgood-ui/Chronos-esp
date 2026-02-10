#include "gate_input.h"
#include "../experiments/experiments.h"
#include "../drivers/hal_panel.h"
#include "../core/gate_engine.h"
#include "../core/event_bus.h"
#include <Arduino.h>

extern EventBus gBus;

// ═════════════════���═════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════

#define GATE_A_PIN  0    // CH422G IO0
#define GATE_B_PIN  5    // CH422G IO5
#define ACTIVE_LOW  1    // Gates are active low

// Polling throttle (ms between I²C reads)
#define POLL_INTERVAL_MS  20

// ═══════════════════════════════════════════════════════════════════════
// State
// ═══════════════════════════════════════════════════════════════════════

static bool gPaused = false;
static bool gInitialized = false;

static uint8_t gGateA_last = 1;
static uint8_t gGateB_last = 1;
static uint32_t gLastPollMs = 0;

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════���═

void gate_input_init() {
  if (gInitialized) {
    Serial.println("[GateInput] Already initialized");
    return;
  }
  
  Serial.println("[Setup] Configuring CH422G pushbuttons for gate inputs...");
  
  // Wait for expander to be ready
  if (!hal::expander_wait_ready(1000)) {
    Serial.println("[GateInput] ERROR: Expander not ready!");
    return;
  }
  
  // Configure pins as inputs
  hal::expander_pinMode(GATE_A_PIN, false);  // INPUT
  hal::expander_pinMode(GATE_B_PIN, false);  // INPUT
  
  // Read initial states
  uint8_t initialA = 1, initialB = 1;
  hal::expander_digitalRead(GATE_A_PIN, &initialA);
  hal::expander_digitalRead(GATE_B_PIN, &initialB);
  
  gGateA_last = initialA;
  gGateB_last = initialB;
  
  Serial.printf("[Input] Configured pushbuttons: A=IO%d, B=IO%d, active_low=%d\n", 
                GATE_A_PIN, GATE_B_PIN, ACTIVE_LOW);
  Serial.printf("[Input] Initial snapshot: GateA=%d, GateB=%d\n", 
                initialA, initialB);
  
  gInitialized = true;
  Serial.println("[Input] Init complete");
}

void gate_input_poll() {
  if (!gInitialized) return;
  
  // Check pause flag (screensaver, etc.)
  if (gPaused) return;

  // NEW: Don't poll during screen transitions (CRITICAL for stability)
  extern volatile bool g_screen_transition_active;
  if (g_screen_transition_active) return;
  
  // Check experiment state - only poll when armed or running
  if (!experiment_should_poll_gates()) {
    return;
  }
  
  // Throttle polling to reduce I²C bus load
  uint32_t now = millis();
  if (now - gLastPollMs < POLL_INTERVAL_MS) {
    return;
  }
  gLastPollMs = now;
  
  // Read current gate states
  uint8_t gateA = 1, gateB = 1;
  
  if (!hal::expander_digitalRead(GATE_A_PIN, &gateA)) {
    Serial.println("[GateInput] Failed to read Gate A");
    return;
  }
  
  if (!hal::expander_digitalRead(GATE_B_PIN, &gateB)) {
    Serial.println("[GateInput] Failed to read Gate B");
    return;
  }
  
  // Detect changes
  bool gateA_changed = (gateA != gGateA_last);
  bool gateB_changed = (gateB != gGateB_last);
  
  if (gateA_changed) {
    Serial.printf("[Gate] A: %d → %d\n", gGateA_last, gateA);
    gGateA_last = gateA;
    
    // Active low gates: 0 = blocked (LOW), 1 = unblocked (HIGH)
    if (gateA == 0) {
      // Falling edge (beam blocked)
      gate_trigger(GATE_A);
      gate_block_start(GATE_A);
      gBus.publish(EVT_GATE_A_FALL, (uint32_t)millis());
      Serial.println("[GateInput] Gate A FALL (blocked)");
    } else {
      // Rising edge (beam unblocked)
      gate_block_end(GATE_A);
      gBus.publish(EVT_GATE_A_RISE, (uint32_t)millis());
      Serial.println("[GateInput] Gate A RISE (unblocked)");
    }
  }
  
  if (gateB_changed) {
    Serial.printf("[Gate] B: %d → %d\n", gGateB_last, gateB);
    gGateB_last = gateB;
    
    // Active low gates: 0 = blocked (LOW), 1 = unblocked (HIGH)
    if (gateB == 0) {
      // Falling edge (beam blocked)
      gate_trigger(GATE_B);
      gate_block_start(GATE_B);
      gBus.publish(EVT_GATE_B_FALL, (uint32_t)millis());
      Serial.println("[GateInput] Gate B FALL (blocked)");
    } else {
      // Rising edge (beam unblocked)
      gate_block_end(GATE_B);
      gBus.publish(EVT_GATE_B_RISE, (uint32_t)millis());
      Serial.println("[GateInput] Gate B RISE (unblocked)");
    }
  }
}

void gate_input_pause() {
  if (!gPaused) {
    gPaused = true;
    Serial.println("[GateInput] Polling PAUSED");
  }
}

void gate_input_resume() {
  if (gPaused) {
    gPaused = false;
    Serial.println("[GateInput] Polling RESUMED");
  }
}

bool gate_input_is_paused() {
  return gPaused;
}

bool gate_input_get_a() {
  return gGateA_last;
}

bool gate_input_get_b() {
  return gGateB_last;
}