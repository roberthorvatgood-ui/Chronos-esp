/*
 * gate_input.cpp
 * Gate input polling with pause/resume support for screensaver coordination
 * [Created: 2026-02-08]
 */

#include <Arduino.h>
#include "gate_input.h"

static bool s_gate_polling_enabled = true;

void gate_input_pause() {
  s_gate_polling_enabled = false;
  Serial.println("[GateInput] Polling PAUSED");
}

void gate_input_resume() {
  s_gate_polling_enabled = true;
  Serial.println("[GateInput] Polling RESUMED");
}

bool gate_input_is_paused() {
  return !s_gate_polling_enabled;
}