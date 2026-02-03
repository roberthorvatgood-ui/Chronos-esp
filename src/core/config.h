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

// ====== Optical gates (CH422G IO0/IO1) ======
// Active‑LOW: LOW = beam blocked (gate CLOSED), HIGH = beam open.
#define GATE_A_DI_INDEX   0   // IO0 on CH422G
#define GATE_B_DI_INDEX   5   // IO1 on CH422G (bit 5)

#define GATE_A_MASK       (1ULL << GATE_A_DI_INDEX)
#define GATE_B_MASK       (1ULL << GATE_B_DI_INDEX)

// ====== IO Expander (CH422G) for gates ======

// I²C configuration (copied from your IO test)
#define EXAMPLE_I2C_ADDR    (ESP_IO_EXPANDER_I2C_CH422G_ADDRESS)
#define EXAMPLE_I2C_SDA_PIN 8   // I2C data line pin
#define EXAMPLE_I2C_SCL_PIN 9   // I2C clock line pin

// Gate input indices on CH422G
#define GATE_A_DI_INDEX   0     // IO0
#define GATE_B_DI_INDEX   5     // IO1 (bit 5)

// Bit masks for the inputs (these correspond to DI0_mask and DI1_mask)
#define GATE_A_MASK       (1U << GATE_A_DI_INDEX)
#define GATE_B_MASK       (1U << GATE_B_DI_INDEX)