/*****
 * waveshare_io_port.h
 * Waveshare EXIO Mapping and Gate Definitions
 * [Created: 2026-02-01]
 *
 * EXIO0 -> DI0 and EXIO5 -> DI1 mapping verified from Waveshare schematic crop.
 *****/
#pragma once
#include <stdint.h>

// CH422G expander pin indices (from Waveshare schematic)
// EXIO0 corresponds to expander bit 0
#define DI0      0
#define DI0_mask (1u << DI0)

// EXIO5 corresponds to expander bit 5
#define DI1      5
#define DI1_mask (1u << DI1)

// Human-friendly gate macros that map expander indices to gate names
#define PIN_GATE_A_EXP  DI0
#define PIN_GATE_A_mask DI0_mask
#define PIN_GATE_B_EXP  DI1
#define PIN_GATE_B_mask DI1_mask

// Test function declaration
void waveshare_io_test();
