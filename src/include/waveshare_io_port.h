#ifndef __WAVESHARE_IO_PORT_H
#define __WAVESHARE_IO_PORT_H

// ──────────────────────────────────────────────────────────────────────────────
// Waveshare ESP32-S3-Touch-LCD-5 EXIO Pin Definitions
// ──────────────────────────────────────────────────────────────────────────────
// The CH422G I/O expander provides digital I/O pins labeled EXIO0..EXIO7.
// From the Waveshare schematic:
//   - EXIO0 → DI0 (digital input index 0, physically EXIO0)
//   - EXIO5 → DI1 (digital input index 1, physically EXIO5)
// ──────────────────────────────────────────────────────────────────────────────

// Digital input pin indices on the CH422G expander
#define DI0 0
#define DI1 5

// Bitmasks for multiDigitalRead operations
#define DI0_mask (1u << DI0)
#define DI1_mask (1u << DI1)

// Human-friendly gate macros mapping expander indices to gate names
#define PIN_GATE_A_EXP   DI0
#define PIN_GATE_A_mask  DI0_mask
#define PIN_GATE_B_EXP   DI1
#define PIN_GATE_B_mask  DI1_mask

// LCD control pins (existing from schematic)
#define TP_RST  1  // Touch screen reset pin
#define LCD_BL  2  // LCD backlight pinout
#define LCD_RST 3  // LCD reset pin
#define SD_CS   4  // SD card select pin
// Note: USB_SEL was previously pin 5, but EXIO5 is now dedicated to Gate B (DI1).
// If USB selection is needed, use a different EXIO pin or verify priority.

// Test function for validating gate inputs
void waveshare_io_test();

#endif // __WAVESHARE_IO_PORT_H
