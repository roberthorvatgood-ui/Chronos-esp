
// src/core/config.h
#pragma once
// ====== Display (Waveshare 5.0" RGB) ======
inline constexpr int SCREEN_WIDTH  = 800;
inline constexpr int SCREEN_HEIGHT = 480;

// ====== Timing ======
inline constexpr unsigned long LONG_PRESS_MS = 2000;
inline constexpr unsigned long ENTER_SETTINGS_LONG_PRESS_MS = 10000;
inline constexpr unsigned long SCREENSAVER_TIMEOUT_MS = 300000; // 5 min

// ====== Menu items (updated) ======
inline constexpr const char* MODES[] = {
  "Stopwatch",
  "Linear Motion (CV)",
  "Photogate Speed",
  "Uniform Acceleration (UA)",
};
inline constexpr int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

// ====== Gate Input Buttons (EXIO0 and EXIO5 on Waveshare ESP32-S3-Touch-LCD-5) ======
// TODO: Set the actual GPIO numbers for EXIO0 and EXIO5 from the Waveshare schematic.
// Consult the Waveshare ESP32-S3-Touch-LCD-5 product documentation to identify
// the GPIO pins mapped to EXIO0 and EXIO5 on the EXIO header.
// Example placeholder values below (verify with your board's pinout):
//   EXIO0 might map to GPIO 15 (verify!)
//   EXIO5 might map to GPIO 16 (verify!)
#define BUTTON_GATE_A 15  // EXIO0 - Gate A input (formerly BUTTON_DOWN)
#define BUTTON_GATE_B 16  // EXIO5 - Gate B input (formerly BUTTON_SELECT)

// Backward compatibility aliases (DEPRECATED - will be removed in future release)
// These allow existing code to continue working during transition
#define BUTTON_DOWN   BUTTON_GATE_A
#define BUTTON_SELECT BUTTON_GATE_B
