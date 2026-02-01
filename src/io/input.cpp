

#include <Arduino.h>
#include "input.h"                // local
#include "../core/event_bus.h"    // sibling (if you publish events)
#include "../core/config.h"       // sibling


static unsigned long gateAPressStartMs = 0;  // Gate A press tracking (formerly downPressStartMs)
static unsigned long bothPressStartMs = 0;
static bool bothHeld = false;

void input_init() {
  pinMode(BUTTON_GATE_B, INPUT_PULLUP);  // Gate B on EXIO5
  pinMode(BUTTON_GATE_A, INPUT_PULLUP);  // Gate A on EXIO0
}

void input_read(Buttons &btns, int &currentGateB, int &currentGateA){
  currentGateB = digitalRead(BUTTON_GATE_B);
  currentGateA = digitalRead(BUTTON_GATE_A);
}
bool input_edge_falling(int prev, int curr){ return prev == HIGH && curr == LOW; }

// Publish events (works with real buttons later)
void input_poll_and_publish(Buttons &btns) {
  const int gateB = digitalRead(BUTTON_GATE_B);
  const int gateA = digitalRead(BUTTON_GATE_A);
  const unsigned long now = millis();

  if (btns.prevGateB == HIGH && gateB == LOW) gBus.publish(EVT_GATE_B_FALL, now);
  if (btns.prevGateA == HIGH && gateA == LOW) gBus.publish(EVT_GATE_A_FALL, now);

  if (gateA == LOW) {
    if (gateAPressStartMs == 0) gateAPressStartMs = now;
    if ((now - gateAPressStartMs) >= LONG_PRESS_MS) {
      gBus.publish(EVT_GATE_A_LONG, now);
      gateAPressStartMs = 0;
    }
  } else {
    gateAPressStartMs = 0;
  }

  const bool bothPressed = (gateB == LOW) && (gateA == LOW);
  if (bothPressed) {
    bothHeld = true;
    if (bothPressStartMs == 0) bothPressStartMs = now;
    if ((now - bothPressStartMs) >= ENTER_SETTINGS_LONG_PRESS_MS) {
      gBus.publish(EVT_BOTH_LONG, now);
      bothPressStartMs = 0;
    }
  } else {
    bothPressStartMs = 0;
    if (bothHeld && gateB == HIGH && gateA == HIGH) {
      gBus.publish(EVT_BOTH_RELEASED, now);
      bothHeld = false;
    }
  }

  btns.prevGateB = gateB;
  btns.prevGateA = gateA;
}
