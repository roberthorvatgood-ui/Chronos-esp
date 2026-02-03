# Non-blocking Gate Handling Implementation

## Summary
This implementation adds debounced, non-blocking optical gate handling for IO0 (GATE A) and IO1 (GATE B) using the CH422G IO expander.

## Files Added

### src/drivers/waveshare_io_port.h
Header file defining the CH422G IO expander API and LCD configuration constants.

**Key API Functions:**
- `bool waveshare_io_init()` - Initialize the CH422G expander (idempotent)
- `void waveshare_io_sample()` - Non-blocking periodic sampling with debouncing
- `bool waveshare_gateA_closed()` - Query if Gate A is closed (active-LOW)
- `bool waveshare_gateB_closed()` - Query if Gate B is closed (active-LOW)
- `void waveshare_io_test()` - Legacy blocking test (preserved for compatibility)

### src/drivers/waveshare_io_port.cpp
Implementation of the non-blocking, debounced gate sampling logic.

**Features:**
- Debouncing with 200ms sample period and 3 stable samples required
- Active-LOW input handling (gates closed = LOW)
- Non-blocking design - only samples when period elapses
- Safe initialization (can be called multiple times)

### examples/05_IO_Test/05_IO_Test.ino
Example Arduino sketch demonstrating the new non-blocking API.

**Features:**
- Prints gate state changes to serial output
- Only prints when state actually changes (reduces serial overhead)
- Non-blocking loop design
- 10ms delay for reasonable CPU usage

## Usage

### In your Arduino sketch:

```cpp
#include "src/drivers/waveshare_io_port.h"

void setup() {
    Serial.begin(115200);
    if (!waveshare_io_init()) {
        Serial.println("Failed to initialize IO expander");
    }
}

void loop() {
    // Call frequently - it's cheap, only samples every 200ms
    waveshare_io_sample();
    
    // Query gate states
    if (waveshare_gateA_closed()) {
        // Gate A is closed
    }
    if (waveshare_gateB_closed()) {
        // Gate B is closed
    }
}
```

## Configuration
Tunable parameters in `waveshare_io_port.cpp`:
- `SAMPLE_PERIOD_MS` (200ms) - How often to sample inputs
- `STABLE_COUNT` (3) - Number of stable samples required for debouncing

## Technical Details

### Pin Mapping
- IO0 (DI0) - Gate A input
- IO1 (DI1) - Gate B input
- I2C SDA: Pin 8
- I2C SCL: Pin 9
- CH422G Address: ESP_IO_EXPANDER_I2C_CH422G_ADDRESS

### Active-LOW Logic
The CH422G inputs are active-LOW (gates/buttons pull to GND when active). The query functions return `true` when the gate is CLOSED (i.e., input reads LOW) to match typical "pressed/active" semantics.

## Code Review Improvements
The following improvements were made based on code review feedback:
1. Removed redundant header guard (kept `#pragma once`)
2. Fixed legacy test function to use public API for consistency
3. Updated example to only print on state changes (reduces serial output)
