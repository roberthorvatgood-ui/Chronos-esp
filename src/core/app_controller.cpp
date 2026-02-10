
#include <Arduino.h>
#include "app_controller.h"

// MODE_ROUTES — central routing table for controller-driven navigation.
// The order here defines the indices used throughout the app.
static void (*const MODE_ROUTES[])() = {
  gui_show_stopwatch,   // 0: Stopwatch
  gui_show_cv,          // 1: Linear Motion (CV)
  gui_show_photogate,   // 2: Photogate
  gui_show_ua,          // 3: Uniform Acceleration (two-gate)
  gui_show_freefall,    // 4: Free Fall
  gui_show_incline,     // 5: Inclined Plane
  gui_show_tacho        // 6: Tachometer
};

static constexpr uint8_t MODE_COUNT =
    (uint8_t)(sizeof(MODE_ROUTES) / sizeof(MODE_ROUTES[0]));

void AppController::enter_mode() {
  uint8_t idx = startIndex;
  if (idx >= MODE_COUNT) idx = 0;
  Serial.printf("[AppController] enter_mode -> idx=%u, fn=%p\n",
                (unsigned)idx, (void*)MODE_ROUTES[idx]);
  MODE_ROUTES[idx]();  // call the function pointer
}

void AppController::next_mode() {
  startIndex = (uint8_t)((startIndex + 1) % MODE_COUNT);
  MODE_ROUTES[startIndex]();  // call it
}

void AppController::prev_mode() {
  startIndex = (uint8_t)((startIndex + MODE_COUNT - 1) % MODE_COUNT);
  MODE_ROUTES[startIndex]();  // call it
}

void AppController::on_event(const Event& /*e*/) {
  // Gate events for experiments are handled by gui_poll_real_gate_experiments()
  // No special handling needed here.
}
