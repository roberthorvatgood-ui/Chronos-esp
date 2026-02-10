
#include <Arduino.h>
#include "app_controller.h"
#include "event_bus.h"
#include "../gui/gui.h"

// Stopwatch gate mode values (must match SwGateMode enum in gui.cpp)
// Defined here as constants to avoid header coupling.
// Values from gui.cpp: enum class SwGateMode : uint8_t { None = 0, GateA = 1, GateAB = 2 };
static constexpr uint8_t SW_MODE_NONE  = 0;
static constexpr uint8_t SW_MODE_GATE_A = 1;
static constexpr uint8_t SW_MODE_GATE_AB = 2;

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

void AppController::on_event(const Event& e) {
  // Guard against processing gate events during screen transitions to prevent access to freed LVGL objects
  extern volatile bool g_screen_transition_active;
  if (g_screen_transition_active) return;
  
  // Only handle gate events for stopwatch when on stopwatch screen and armed
  if (!gui_is_stopwatch_screen()) return;
  
  uint8_t mode = gui_get_stopwatch_mode();
  if (mode == SW_MODE_NONE) return;
  
  bool armed = gui_is_armed();
  if (!armed) return;
  
  // Handle gate events based on stopwatch mode
  switch (e.type) {
    case EVT_GATE_A_FALL:
      // Gate A falling edge (beam blocked)
      if (mode == SW_MODE_GATE_A) {
        // Gate A mode: toggle start/stop on each Gate A trigger
        if (!this->sw.running()) {
          this->sw.start();
          gui_sw_record_start();
          Serial.println("[Stopwatch] Gate A: Started");
        } else {
          this->sw.stop();
          gui_sw_record_stop();
          Serial.println("[Stopwatch] Gate A: Stopped");
        }
      } else if (mode == SW_MODE_GATE_AB) {
        // Gate A+B mode: start on Gate A
        if (!this->sw.running()) {
          this->sw.start();
          gui_sw_record_start();
          Serial.println("[Stopwatch] Gate A: Started (AB mode)");
        } else {
          // If already running, record a lap
          gui_sw_record_lap();
          Serial.println("[Stopwatch] Gate A: Lap (AB mode)");
        }
      }
      break;
      
    case EVT_GATE_B_FALL:
      // Gate B falling edge (beam blocked)
      if (mode == SW_MODE_GATE_A) {
        // In Gate A mode, Gate B triggers a lap only when running
        if (this->sw.running()) {
          gui_sw_record_lap();
          Serial.println("[Stopwatch] Gate B: Lap (A mode)");
        }
      } else if (mode == SW_MODE_GATE_AB) {
        // Gate A+B mode: stop on Gate B only when running
        if (this->sw.running()) {
          this->sw.stop();
          gui_sw_record_stop();
          Serial.println("[Stopwatch] Gate B: Stopped (AB mode)");
        }
        // If not running, ignore (user must start with Gate A first)
      }
      break;
      
    case EVT_GATE_A_RISE:
    case EVT_GATE_B_RISE:
      // Rising edges (beam unblocked) - currently not used for stopwatch control
      break;
      
    default:
      break;
  }
}
