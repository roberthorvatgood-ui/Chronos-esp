
#pragma once
#include <stdint.h>
#include <esp_timer.h> // for esp_timer_get_time() on ESP32

// Forward declarations for GUI entry points (implemented in gui.cpp)
void gui_show_main_menu();
void gui_show_stopwatch();
void gui_show_cv();
void gui_show_photogate();
void gui_show_ua();
void gui_show_freefall();
void gui_show_incline();
void gui_show_tacho();
void gui_show_settings();       // optional, for header button
void gui_show_wifi_settings();  // optional, for header button
void gui_show_stopwatch_settings(); // if used from GUI
void gui_show_cv_settings();
void gui_show_pg_settings();
void gui_show_ua_settings();
void gui_show_freefall_settings();
void gui_show_incline_settings();
void gui_show_tacho_settings();

// ---- Inline Stopwatch implementation ----
class Stopwatch {
public:
  Stopwatch() : running_(false), start_us_(0), accum_us_(0) {}
  void start() {
    if (!running_) {
      start_us_ = esp_timer_get_time();
      running_ = true;
    }
  }
  void stop() {
    if (running_) {
      const uint64_t now = esp_timer_get_time();
      if (now > start_us_) accum_us_ += (now - start_us_);
      running_ = false;
      start_us_ = 0;
    }
  }
  void reset() { running_ = false; start_us_ = 0; accum_us_ = 0; }
  void toggle() { if (running_) stop(); else start(); }
  bool running() const { return running_; }
  // Returns total elapsed microseconds (includes current running segment if running)
  uint64_t elapsed_us() const {
    if (running_) {
      const uint64_t now = esp_timer_get_time();
      if (now > start_us_) return accum_us_ + (now - start_us_);
      else return accum_us_;
    }
    return accum_us_;
  }
private:
  bool     running_;
  uint64_t start_us_;
  uint64_t accum_us_;
};

// ---- App modes (keep indices in sync with MODE_ROUTES[] order in app_controller.cpp) ----
enum class AppMode : uint8_t {
  Stopwatch = 0,
  CV        = 1,
  Photogate = 2,
  UA        = 3,
  FreeFall  = 4,
  Incline   = 5,
  Tacho     = 6
};

// Forward-declare Event so on_event signature compiles without including EventBus headers here
struct Event;

struct AppController {
  // Public so GUI can set startIndex from menu
  uint8_t startIndex = (uint8_t)AppMode::Stopwatch;

  // Stopwatch instance
  Stopwatch sw;

  // Enter current mode based on startIndex
  void enter_mode();

  // Optional cycling helpers
  void next_mode();
  void prev_mode();

  // App-wide event handling (kept for compatibility with your EventBus)
  void on_event(const Event& e);
};
