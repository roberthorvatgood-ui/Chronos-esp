#include "waveshare_io_port.h"
#include <Arduino.h>
#include <cstdint>
#include "src/drivers/hal_panel.h"

static const char* TAG = "waveshare_io_probe_swpu";

// Software "weak" pull-up read:
// - Drive the pin as output HIGH briefly, switch back to input, then read the pin.
// - Returns true if pin reads HIGH, false if LOW.
// WARNING: this toggles pin direction and drives the pin HIGH for a short time.
// Do NOT use during SD transactions or when the pin is used for other active functions.
// This is only a temporary test aid if no hardware pull-ups are present.
static bool expander_read_with_soft_pullup(uint8_t exio, uint32_t read_delay_ms = 2) {
    const uint32_t mask = (1u << exio);

    // Drive HIGH
    hal::expander_pinMode(exio, /*output*/ true);
    hal::expander_digitalWrite(exio, /*high*/ true);

    // Allow level to settle
    delay(read_delay_ms);

    // Switch back to input
    hal::expander_pinMode(exio, /*output*/ false);

    // Short settle
    delay(read_delay_ms);

    // Read single bit
    uint32_t v = hal::expander_multiDigitalRead(mask);
    // v == 0 => all-low (failure or low); v & mask != 0 => HIGH
    return (v & mask) != 0;
}

// Blocking test entry (keeps compatibility)
void waveshare_io_test(void)
{
  waveshare_io_test_debug();
}

// Verbose probe: attempt to soft-pull both gate pins before reading
void waveshare_io_test_debug(void)
{
  Serial.println("\n--- Waveshare IO debug probe (software weak pull-up) ---");

  Serial.printf("[Probe] HAL expander handle = %p\n", (void*)hal::expander_get_handle());

  if (!hal::expander_wait_ready(800)) {
    Serial.println("[ERROR] HAL expander not ready (expander_wait_ready timed out). Aborting probe.");
    return;
  }

  // Ensure pins are inputs initially
  (void)hal::expander_pinMode(PIN_GATE_A, false);
  (void)hal::expander_pinMode(PIN_GATE_B, false);

  const uint32_t full_mask = 0xFFu;
  uint32_t last = hal::expander_multiDigitalRead(full_mask) & 0xFFu;
  Serial.printf("[Probe] initial full-read: 0x%02X  (A_bit=%d B_bit=%d)\n",
                (unsigned)(last & 0xFFu),
                (last & PIN_GATE_A_mask) ? 1 : 0,
                (last & PIN_GATE_B_mask) ? 1 : 0);

  Serial.println("[Probe] Now polling for 60s. While running:");
  Serial.println(" - Short EXIO0 to GND -> Gate A");
  Serial.println(" - Short EXIO5 to GND -> Gate B");
  Serial.println(" - If B doesn't respond, this probe will use a brief software pull-up to sample its level.");

  unsigned long t0 = millis();
  while (millis() - t0 < 60000UL) {
    // First do normal raw read
    uint32_t v = hal::expander_multiDigitalRead(full_mask) & 0xFFu;

    // If Gate B's bit is not changing as expected, try a soft pull-up read for that pin
    // (we do it every loop to capture presses without requiring hardware resistors).
    // Use the soft method only for the Gate pins (not every pin).
    bool b_bit = (v & PIN_GATE_B_mask) ? true : false;
    bool a_bit = (v & PIN_GATE_A_mask) ? true : false;

    // If B reads HIGH but you expect it to go LOW on shorting, also try soft-read to confirm.
    // (This covers the "no pull-up" case where the line floats).
    // Soft-read returns HIGH/LOW; convert to bit state.
    bool soft_b = b_bit;
    soft_b = expander_read_with_soft_pullup(PIN_GATE_B, 2);

    // Update v's B bit with the software-read result to surface it in the printed output
    if (soft_b)
      v |= PIN_GATE_B_mask;
    else
      v &= ~PIN_GATE_B_mask;

    // Similarly, do a soft read for A as well to be consistent (cheap and quick)
    bool soft_a = expander_read_with_soft_pullup(PIN_GATE_A, 2);
    if (soft_a)
      v |= PIN_GATE_A_mask;
    else
      v &= ~PIN_GATE_A_mask;

    if (v != last) {
      Serial.printf("[Probe] full-read -> 0x%02X  bits:", (unsigned)v);
      for (int b = 7; b >= 0; --b) {
        Serial.printf("%d", (v >> b) & 1);
        if (b == 4) Serial.print(' ');
      }
      Serial.printf("  (A_bit=%d B_bit=%d)\n",
                    (v & PIN_GATE_A_mask) ? 1 : 0,
                    (v & PIN_GATE_B_mask) ? 1 : 0);
      last = v;
    }

    delay(80);
  }

  Serial.println("[Probe] Finished (timeout). Diagnostics:");
  Serial.println(" - This used a brief software-drive-high then input-read technique for A and B.");
  Serial.println(" - If B now flips to 0 when you short EXIO5 -> GND, hardware pull-ups are still recommended.");
  Serial.println(" - Permanent fix: add 10 kΩ pull-ups from EXIO pins to 3.3V, or enable expander pull-ups if supported.");
  Serial.println(" - If you see no change, confirm you're shorting the correct physical pin and common GND exists.");
}