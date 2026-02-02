/*****
 * waveshare_io_port.h
 * Minimal Waveshare IO test header — exposes the test functions and pin masks.
 *
 * NOTE:
 * - This header intentionally keeps things small so the test .cpp doesn't
 *   depend on example LCD macros that may not be visible at compile time.
 * - PIN_GATE_A and PIN_GATE_B reflect EXIO0 and EXIO5 (common Waveshare mapping).
 *****/
#pragma once

#include <stdint.h>
#include "src/drivers/hal_panel.h"

// EXIO pin indexes (on the CH422G expander)
#define PIN_GATE_A        0   // EXIO0 -> Gate A (DI0)
#define PIN_GATE_B        5   // EXIO5 -> Gate B (DI1)

// Masks for multiDigitalRead() — 1 = HIGH, 0 = LOW
#define PIN_GATE_A_mask   (1u << PIN_GATE_A)
#define PIN_GATE_B_mask   (1u << PIN_GATE_B)

// Public API
#ifdef __cplusplus
extern "C" {
#endif

// One-off blocking IO test: waits for both gates to be pressed (active-low).
// Returns when completed or aborts early if expander read fails.
void waveshare_io_test(void);

// Debug variant: prints raw mask changes for 60s (useful to validate wiring/pull-ups).
void waveshare_io_test_debug(void);

#ifdef __cplusplus
}
#endif