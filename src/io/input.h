
// src/io/input.h
#pragma once
#include "../core/config.h"     // pins & timing constants
#include "../core/event_bus.h"  // Event types if you publish from input.cpp

struct Buttons {
  int prevGateB = HIGH;  // Gate B button state (formerly prevSelect)
  int prevGateA = HIGH;  // Gate A button state (formerly prevDown)
};

void input_init();
void input_poll_and_publish(Buttons &btns);

// helpers (if you wire physical buttons)
void input_read(Buttons &btns, int &currentGateB, int &currentGateA);
bool input_edge_falling(int prev, int curr);
