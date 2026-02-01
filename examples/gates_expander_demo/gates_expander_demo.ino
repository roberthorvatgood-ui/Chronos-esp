/*
    gates_expander_demo.ino
    Demo: read DI0/DI1 on CH422G expander via HAL wrapper and print timestamps.
    Assumes expander_wait_ready() will become true once panel HAL attached expander.
    Uses single-port reads (via expander_digitalRead) and a short debounce.
    Default settings target low-latency optical gates when using the expander only.
    
    NOTE: This example uses relative paths (../) to access the project headers.
    It should be placed in the examples/gates_expander_demo/ directory of the
    Chronos-esp project to resolve the includes correctly.
*/

#include <Arduino.h>
#include "../src/drivers/hal_panel.h"
#include "../src/export/waveshare_sd_card.h"
#include <esp_timer.h>
#include <inttypes.h>

static const uint32_t DEBOUNCE_MS = 2; // short debounce for optical gates

void setup() {
    Serial.begin(115200);
    Serial.println("\n[gates_expander_demo] Starting");

#ifdef HAL_PANEL_HAS_EXPANDER_WAIT
    if (!expander_wait_ready(800)) {
        Serial.println("[gates_expander_demo] Expander not ready (timeout).");
    } else {
        Serial.println("[gates_expander_demo] Expander ready.");
    }
#else
    Serial.println("[gates_expander_demo] Note: expander_wait_ready not available, proceeding.");
#endif

    // Configure EXIO pins as inputs
    if (!expander_pinMode(EXIO_DI0, false)) {
        Serial.println("[gates_expander_demo] Warning: failed to set EXIO_DI0 as input.");
    }
    if (!expander_pinMode(EXIO_DI1, false)) {
        Serial.println("[gates_expander_demo] Warning: failed to set EXIO_DI1 as input.");
    }
}

void loop() {
    static bool prev0 = true; // assume pull-ups/high
    static bool prev1 = true;
    bool level0 = true, level1 = true;

    int64_t t_before = esp_timer_get_time();
    bool ok0 = expander_digitalRead(EXIO_DI0, level0);
    bool ok1 = expander_digitalRead(EXIO_DI1, level1);
    int64_t t_after = esp_timer_get_time();
    // Approximate timestamp (midpoint). Note: I2C latencies may vary between reads.
    int64_t t_read = (t_before + t_after) / 2;

    if (!ok0 || !ok1) {
        Serial.println("[gates_expander_demo] Expander read failed (is HAL attached?).");
        delay(200);
        return;
    }

    // Inputs are active-low (typical); detect falling edge (HIGH -> LOW)
    if (prev0 && !level0) {
        // Debounce: wait a short time and re-read to confirm
        delay(DEBOUNCE_MS);
        bool confirm = false;
        if (expander_digitalRead(EXIO_DI0, confirm) && !confirm) {
            Serial.printf("[GATE] DI0 FALL at %" PRIi64 " us (approx)\n", t_read);
        }
    }
    if (prev1 && !level1) {
        delay(DEBOUNCE_MS);
        bool confirm = false;
        if (expander_digitalRead(EXIO_DI1, confirm) && !confirm) {
            Serial.printf("[GATE] DI1 FALL at %" PRIi64 " us (approx)\n", t_read);
        }
    }

    prev0 = level0;
    prev1 = level1;

    // Poll fast enough for the latency target while giving CPU time to other tasks.
    delay(3); // ~3ms poll interval -> typical reaction time target ~2..8 ms
}