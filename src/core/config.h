
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

// ====== Buttons (if you wire physical buttons) ======
#define BUTTON_DOWN   15
#define BUTTON_SELECT 16
