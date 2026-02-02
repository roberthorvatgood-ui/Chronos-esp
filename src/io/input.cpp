

#include <Arduino.h>
#include "input.h"                // local
#include "../core/event_bus.h"    // sibling (if you publish events)
#include "../core/config.h"       // sibling


static unsigned long downPressStartMs = 0;
static unsigned long bothPressStartMs = 0;
static bool bothHeld = false;

void input_init() {
  pinMode(BUTTON_SELECT, INPUT_PULLUP);
  pinMode(BUTTON_DOWN,   INPUT_PULLUP);
}

void input_read(Buttons &btns, int &currentSelect, int &currentDown){
  currentSelect = digitalRead(BUTTON_SELECT);
  currentDown   = digitalRead(BUTTON_DOWN);
}
bool input_edge_falling(int prev, int curr){ return prev == HIGH && curr == LOW; }

// Publish events (works with real buttons later)
void input_poll_and_publish(Buttons &btns) {
  const int sel = digitalRead(BUTTON_SELECT);
  const int dwn = digitalRead(BUTTON_DOWN);
  const unsigned long now = millis();

  if (btns.prevSelect == HIGH && sel == LOW) gBus.publish(EVT_SELECT_FALL, now);
  if (btns.prevDown   == HIGH && dwn == LOW) gBus.publish(EVT_DOWN_FALL,   now);

  if (dwn == LOW) {
    if (downPressStartMs == 0) downPressStartMs = now;
    if ((now - downPressStartMs) >= LONG_PRESS_MS) {
      gBus.publish(EVT_DOWN_LONG, now);
      downPressStartMs = 0;
    }
  } else {
    downPressStartMs = 0;
  }

  const bool bothPressed = (sel == LOW) && (dwn == LOW);
  if (bothPressed) {
    bothHeld = true;
    if (bothPressStartMs == 0) bothPressStartMs = now;
    if ((now - bothPressStartMs) >= ENTER_SETTINGS_LONG_PRESS_MS) {
      gBus.publish(EVT_BOTH_LONG, now);
      bothPressStartMs = 0;
    }
  } else {
    bothPressStartMs = 0;
    if (bothHeld && sel == HIGH && dwn == HIGH) {
      gBus.publish(EVT_BOTH_RELEASED, now);
      bothHeld = false;
    }
  }

  btns.prevSelect = sel;
  btns.prevDown   = dwn;
}
