#include "../../src/drivers/waveshare_io_port.h"

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
    if (waveshare_gateA_closed()) {
      Serial.println("GATE A: CLOSED");
    } else {
      Serial.println("GATE A: OPEN");
    }

    if (waveshare_gateB_closed()) {
      Serial.println("GATE B: CLOSED");
    } else {
      Serial.println("GATE B: OPEN");
    }

    delay(1000); // keep serial output readable
}
