
/*****
 * hal_panel.h
 * Waveshare ESP32‑S3‑Touch‑LCD‑5 – HAL wrapper
 * [Updated: 2026-01-19 23:58 CET]
 *  - Uses C API creator for CH422G (no adapter/C++ wrapper dependency)
 *  - Adds expander_wait_ready(timeout_ms) and expander_get_handle()
 *****/
#pragma once
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include "lvgl_v8_port.h"

// Prefer the "port" header if present (your log shows files under /port/)
#if defined(__has_include)
  #if __has_include(<port/esp_io_expander.h>)
    #include <port/esp_io_expander.h>
  #elif __has_include(<esp_io_expander.h>)
    #include <esp_io_expander.h>
  #else
    // Fallback types if headers are missing (keeps buildable stubs)
    typedef void*  esp_io_expander_handle_t;
    extern "C" {
      typedef int32_t esp_err_t;
      enum io_expander_dir_t { IO_EXPANDER_INPUT = 0, IO_EXPANDER_OUTPUT = 1 };
      esp_err_t esp_io_expander_set_dir  (esp_io_expander_handle_t h, uint32_t pin_mask, io_expander_dir_t dir);
      esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t h, uint32_t pin_mask, int level);
    }
  #endif
#else
  typedef void*  esp_io_expander_handle_t;
  extern "C" {
    typedef int32_t esp_err_t;
    enum io_expander_dir_t { IO_EXPANDER_INPUT = 0, IO_EXPANDER_OUTPUT = 1 };
    esp_err_t esp_io_expander_set_dir  (esp_io_expander_handle_t h, uint32_t pin_mask, io_expander_dir_t dir);
    esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t h, uint32_t pin_mask, int level);
  }
#endif

namespace hal {

// ── Panel lifecycle ────────────────────────────────────────────────────────
bool init();
bool begin(bool start_backlight_on = false);
bool lvgl_init();

namespace drivers = esp_panel::drivers;
drivers::LCD*   lcd();
drivers::Touch* touch();

// ── Backlight control ──────────────────────────────────────────────────────
void backlight_on();
void backlight_off();
/** 0..100% (requires wiring AP3032 CTRL to a GPIO for real dimming). */
void backlight_set(uint8_t percent);

// ── IO Expander (CH422G) helpers ───────────────────────────────────────────
/** True once the panel stack created & attached its IO expander handle. */
bool expander_ready();
/** Block until IO expander reports ready (0=don't wait). Returns true if ready. */
bool expander_wait_ready(uint32_t timeout_ms = 800);
/** Configure an EXIO pin direction: output=true, input=false. */
bool expander_pinMode(uint8_t exio, bool output);
/** Set an EXIO pin level: high=true, low=false. */
bool expander_digitalWrite(uint8_t exio, bool high);
/** Attach an existing expander handle (manual override). */
void expander_attach(esp_io_expander_handle_t h);
/** Return the raw IO expander handle (nullable; advanced uses). */
esp_io_expander_handle_t expander_get_handle();

} // namespace hal

#define HAS_HAL_BACKLIGHT_SET 1

