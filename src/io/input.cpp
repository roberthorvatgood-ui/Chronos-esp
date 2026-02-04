#include <Arduino.h>

#include "input.h"
#include "../core/config.h"
#include "../core/gate_engine.h"
#include "../drivers/hal_panel.h"   // hal::expander_get_handle(), esp_io_expander_get_level
#include "../drivers/hal_i2c_executor.h"  // hal_i2c_exec_sync

// Debounced logical levels: true = HIGH (beam OPEN), false = LOW (beam BLOCKED)
static bool gGateALevel = true;
static bool gGateBLevel = true;

// Debounce state
static uint8_t sA_count = 0, sB_count = 0;
static bool    sA_last  = true;
static bool    sB_last  = true;
static constexpr uint8_t STABLE_COUNT = 3;

// Throttling: do not hit I2C every loop iteration
static unsigned long sLastPollMs = 0;
static const unsigned long POLL_PERIOD_MS = 10;   // 10 ms; increase if needed

// Gate indices/masks (IO0 and IO1/bit5)
#ifndef GATE_A_DI_INDEX
#define GATE_A_DI_INDEX   0
#endif
#ifndef GATE_B_DI_INDEX
#define GATE_B_DI_INDEX   5
#endif
#ifndef GATE_A_MASK
#define GATE_A_MASK       (1U << GATE_A_DI_INDEX)
#endif
#ifndef GATE_B_MASK
#define GATE_B_MASK       (1U << GATE_B_DI_INDEX)
#endif

bool input_edge_falling(int prev, int curr) { return prev == HIGH && curr == LOW; }
bool input_edge_rising (int prev, int curr) { return prev == LOW  && curr == HIGH; }
static inline int lvl_to_digital(bool v)    { return v ? HIGH : LOW; }

// Context for expander read operation
struct expander_read_ctx {
  esp_io_expander_handle_t h;
  uint32_t mask;
  uint32_t* out;
};

// Executor operation: read expander levels
static esp_err_t expander_read_op(void* vctx) {
  expander_read_ctx* ctx = static_cast<expander_read_ctx*>(vctx);
  if (!ctx || !ctx->h || !ctx->out) {
    return ESP_ERR_INVALID_ARG;
  }
  return esp_io_expander_get_level(ctx->h, ctx->mask, ctx->out);
}

// Read CH422G input bits via HAL's expander handle using I2C executor
static uint8_t read_inputs_raw()
{
    esp_io_expander_handle_t h = hal::expander_get_handle();
    if (!h) {
        // Not ready yet; treat as all HIGH (open)
        return 0xFF;
    }

    uint32_t val = 0;
    const uint32_t mask = 0xFF;
    
    // Use I2C executor for thread-safe I2C read
    expander_read_ctx ctx = { h, mask, &val };
    if (hal::hal_i2c_exec_sync(expander_read_op, &ctx, 100) != ESP_OK) {
        // On error, keep previous state; return all HIGH so we don't spurious-trigger
        return 0xFF;
    }
    return static_cast<uint8_t>(val & 0xFF);
}

// Init: seed state and reset gate engine, no I2C setup here
void input_init()
{
    uint8_t snap = read_inputs_raw();

    gGateALevel  = (snap & GATE_A_MASK) != 0;
    gGateBLevel  = (snap & GATE_B_MASK) != 0;

    sA_last  = gGateALevel;
    sB_last  = gGateBLevel;
    sA_count = STABLE_COUNT;
    sB_count = STABLE_COUNT;

    gate_engine_init();
}

// Legacy read helper
void input_read(Buttons& btns, int& currentA, int& currentB)
{
    currentA        = lvl_to_digital(gGateALevel);
    currentB        = lvl_to_digital(gGateBLevel);
    btns.prevSelect = currentA;
    btns.prevDown   = currentB;
}

// Main poll — called from loop()
void input_poll_and_publish(Buttons& btns)
{
    unsigned long now = millis();
    if (now - sLastPollMs < POLL_PERIOD_MS) {
        // Too soon; just update Buttons with current debounced state
        btns.prevSelect = lvl_to_digital(gGateALevel);
        btns.prevDown   = lvl_to_digital(gGateBLevel);
        return;
    }
    sLastPollMs = now;

    uint8_t snap = read_inputs_raw();

    bool sampleA = (snap & GATE_A_MASK) != 0;
    bool sampleB = (snap & GATE_B_MASK) != 0;

    // ----- Debounce Gate A -----
    if (sampleA == sA_last) {
        if (sA_count < 255) sA_count++;
    } else {
        sA_count = 1;
        sA_last  = sampleA;
    }

    if (sA_count >= STABLE_COUNT) {
        if (gGateALevel != sampleA) {
            bool prev = gGateALevel;
            gGateALevel = sampleA;

            int prev_d = lvl_to_digital(prev);
            int curr_d = lvl_to_digital(sampleA);

            if (input_edge_falling(prev_d, curr_d)) {
                // Beam went from OPEN (HIGH) to BLOCKED (LOW): front edge
                gate_trigger(GATE_A);        // single timestamp for CV/PG/FF/Tacho
                gate_block_start(GATE_A);    // front edge for UA/Incline
            } else if (input_edge_rising(prev_d, curr_d)) {
                // Beam went from BLOCKED (LOW) to OPEN (HIGH): rear edge
                gate_block_end(GATE_A);
            }
        }
    }

    // ----- Debounce Gate B -----
    if (sampleB == sB_last) {
        if (sB_count < 255) sB_count++;
    } else {
        sB_count = 1;
        sB_last  = sampleB;
    }

    if (sB_count >= STABLE_COUNT) {
        if (gGateBLevel != sampleB) {
            bool prev = gGateBLevel;
            gGateBLevel = sampleB;

            int prev_d = lvl_to_digital(prev);
            int curr_d = lvl_to_digital(sampleB);

            if (input_edge_falling(prev_d, curr_d)) {
                gate_trigger(GATE_B);
                gate_block_start(GATE_B);
            } else if (input_edge_rising(prev_d, curr_d)) {
                gate_block_end(GATE_B);
            }
        }
    }

    // Keep Buttons struct updated
    btns.prevSelect = lvl_to_digital(gGateALevel);
    btns.prevDown   = lvl_to_digital(gGateBLevel);
}