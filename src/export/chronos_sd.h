
/*****
 * chronos_sd.h
 * Chronos – SD bring-up over SPI (Waveshare ESP32-S3 Touch LCD 5")
 * [Updated: 2026-01-19 22:55 CET] ADD: SD CS guard (CH422G), select/deselect API
 *****/
#pragma once
#include <Arduino.h>

/** Compatibility stub: kept for callers; does nothing now. */
bool chronos_sd_preinit();

/** Mount SD (SPI CLK=12, MISO=13, MOSI=11). SD_CS is on CH422G EXIO via HAL. */
bool chronos_sd_begin();

/** True once SD mounted. */
bool chronos_sd_is_ready();

/** Manual control of SD chip-select via CH422G. */
void chronos_sd_select();    // CS LOW (active)
void chronos_sd_deselect();  // CS HIGH (idle)

/** RAII guard: keeps CS LOW for scope lifetime, de-selects on destruction. */
struct ChronosSdSelectGuard {
  ChronosSdSelectGuard();
  ~ChronosSdSelectGuard();
  // Nested guards are safe (reference-counted select).
};
