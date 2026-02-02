#pragma once
#include <stdint.h>

/**
 * Toggle two expander outputs (DO0/DO1) for testing.
 *
 * expander_toggle_terminal_do01_blocking(period_ms, cycles, exio_do0, exio_do1)
 *   - blocking test helper (runs on calling thread).
 *   - period_ms: toggle period in milliseconds.
 *   - cycles: number of toggles; 0 => run forever until reset.
 *   - exio_do0/exio_do1: EXIO pin indexes to drive (defaults used if omitted).
 *
 * expander_toggle_terminal_do01_start(period_ms, cycles, exio_do0, exio_do1)
 *   - starts the same test in a FreeRTOS task (non-blocking).
 *   - returns true if the task was created successfully.
 */
void expander_toggle_terminal_do01_blocking(uint32_t period_ms = 3000,
                                            uint32_t cycles = 10,
                                            uint8_t exio_do0 = 0,
                                            uint8_t exio_do1 = 1);

bool expander_toggle_terminal_do01_start(uint32_t period_ms = 3000,
                                         uint32_t cycles = 0,
                                         uint8_t exio_do0 = 0,
                                         uint8_t exio_do1 = 1);