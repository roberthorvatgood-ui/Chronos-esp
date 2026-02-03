#include "../../src/drivers/waveshare_io_port.h"

// Track previous states to detect changes
static bool prevGateA = false;
static bool prevGateB = false;

void setup()
{
    Serial.begin(115200); // Initialize serial communication
    // Initialize the expander and gate sampling
    if (!waveshare_io_init()) {
      Serial.println("Failed to initialize IO expander");
    } else {
      Serial.println("IO expander initialized, starting gate monitor (non-blocking)...");
    }
    Serial.println("IO test example ready");
}

void loop()
{
    // Call the sampler frequently from your main loop (cheap; it only does work every SAMPLE_PERIOD_MS)
    waveshare_io_sample();

    // Query the debounced state of the gates.
    // waveshare_gate*_closed() returns true when the gate is CLOSED (active-LOW).
    bool gateA = waveshare_gateA_closed();
    bool gateB = waveshare_gateB_closed();

    // Only print when state changes
    if (gateA != prevGateA) {
      Serial.print("GATE A: ");
      Serial.println(gateA ? "CLOSED" : "OPEN");
      prevGateA = gateA;
    }

    if (gateB != prevGateB) {
      Serial.print("GATE B: ");
      Serial.println(gateB ? "CLOSED" : "OPEN");
      prevGateB = gateB;
    }

    delay(10); // Short delay to keep CPU usage reasonable
}
