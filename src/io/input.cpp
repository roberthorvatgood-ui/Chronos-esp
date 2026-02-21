/*****
 * input.cpp
 * Handles photogate inputs (CH422G expander) and optional pushbutton-to-gate mapping.
 * ALL I²C expander operations are executed via the central hal_i2c_executor
 * as a single atomic operation to prevent bus contention with GT911 touch.
 *
 * FIX (2026-02-07): Ensure CH422G is in input mode and use correct read sequence
 * FIX (2026-02-11): Wrap entire set_all_input→read→restore cycle in one executor op
 * FIX (2026-02-11): Reduce poll period to 5ms and debounce to 1 for fast gate response
 * FIX (2026-02-11): Atomic full_read_op — eliminates GT911 touch interleaving
 * FIX (2026-02-14): Only record gate events when experiment is ARMED or RUNNING
 *****/

#include <Arduino.h>
#include "esp_err.h"

#include "../drivers/hal_panel.h"       // hal::expander_get_handle()
#include "../drivers/hal_i2c_executor.h"// hal_i2c_exec_sync
#include "../drivers/hal_i2c_manager.h" // fallback mutex
#include "../core/gate_engine.h"
#include "../experiments/experiments.h" // experiment_should_poll_gates()
#include "../export/app_log.h"
#include "input.h"

// Include the correct header
#if __has_include(<port/esp_io_expander.h>)
  #include <port/esp_io_expander.h>
#elif __has_include(<esp_io_expander.h>)
  #include <esp_io_expander.h>
#endif

// CH422G-specific function to set all pins as inputs (from your diagnostic code)
extern "C" {
  esp_err_t esp_io_expander_ch422g_set_all_input(esp_io_expander_handle_t handle);
}

// Debounced logical levels: true = HIGH (beam OPEN), false = LOW (beam BLOCKED)
static bool gGateALevel = true;
static bool gGateBLevel = true;

// Debounce state for gates
static uint8_t sA_count = 0, sB_count = 0;
static bool    sA_last  = true;
static bool    sB_last  = true;

// ── Tuning ──────────────────────────────────────────────────────────────
// STABLE_COUNT = 1  → react on the very first changed sample (no debounce).
//   Optical gate signals are clean; mechanical bounce doesn't apply.
// POLL_PERIOD_MS = 5 → 200 Hz I²C reads. Fast enough to catch a 10ms gate
//   pulse while still leaving headroom for GT911 touch on Core 1.
// ────────────────────────────────────────────────────────────────────────
static constexpr uint8_t STABLE_COUNT = 1;
static const unsigned long POLL_PERIOD_MS = 5; // 200 Hz — sufficient for optical gates, reduces I²C load by 60%

// Pause flag
static bool sPaused = false;

// Throttling
static unsigned long sLastPollMs = 0;

// Gate indices/masks
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

// ── Atomic executor-backed expander read ────────────────────────────────────
//
// CRITICAL: The entire set_all_input → read → restore_outputs → set_SD_CS
// sequence MUST run as a single atomic operation inside the executor task.
// If any step is done outside the executor, the GT911 touch driver can
// interleave an I²C read between steps, leaving the CH422G stuck in input
// mode and corrupting both the gate read and the touch read.
// ────────────────────────────────────────────────────────────────────────────

struct full_read_ctx {
  esp_io_expander_handle_t h;
  uint8_t snapshot;
};

static esp_err_t full_read_op(void* vctx) {
  full_read_ctx* ctx = static_cast<full_read_ctx*>(vctx);
  if (!ctx || !ctx->h) return ESP_ERR_INVALID_ARG;

  ctx->snapshot = 0xFF;

  // Step 1: Enable all-input mode for reading
  esp_err_t err = esp_io_expander_ch422g_set_all_input(ctx->h);
  if (err != ESP_OK) {
    const uint32_t output_pins = (1u << 2) | (1u << 4);
    esp_io_expander_set_dir(ctx->h, output_pins, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(ctx->h, (1u << 4), 1);
    return err;
  }

  // ── NEW: Let input pins settle after mode switch ──────────────
  ets_delay_us(50);  // 50µs — enough for CH422G input latch

  // Step 2: Read all pins
  uint32_t val = 0;
  err = esp_io_expander_get_level(ctx->h, 0xFF, &val);
  if (err == ESP_OK) {
    ctx->snapshot = static_cast<uint8_t>(val & 0xFF);
  }

  // Steps 3-4 unchanged...
  const uint32_t output_pins = (1u << 2) | (1u << 4);
  esp_err_t restore_err = esp_io_expander_set_dir(ctx->h, output_pins, IO_EXPANDER_OUTPUT);
  if (restore_err != ESP_OK) {
    Serial.printf("[Input] Warning: Failed to restore outputs: 0x%x\n", restore_err);
  }

  restore_err = esp_io_expander_set_level(ctx->h, (1u << 4), 1);
  if (restore_err != ESP_OK) {
    Serial.printf("[Input] Warning: Failed to set SD_CS: 0x%x\n", restore_err);
  }

  return err;
}

static uint8_t read_inputs_raw()
{
    esp_io_expander_handle_t h = hal::expander_get_handle();
    if (!h) {
        return 0xFF;
    }

    full_read_ctx ctx;
    ctx.h = h;
    ctx.snapshot = 0xFF;

    esp_err_t err = hal_i2c_exec_sync(full_read_op, &ctx, 500);

    if (err != ESP_OK) {
        // Fallback: direct read with mutex (last resort)
        if (hal::i2c_lock(50)) {
            full_read_op(&ctx);  // reuse same atomic logic, just under mutex
            hal::i2c_unlock();
        }
    }

    return ctx.snapshot;
}

// ----------------- Pushbutton-to-gate mapping ------------------------------
static int s_btn_gateA_exio = -1;
static int s_btn_gateB_exio = -1;
static bool s_btn_active_low = true;

// Debounce state for pushbuttons
static uint8_t sBtnA_count = STABLE_COUNT, sBtnB_count = STABLE_COUNT;
static bool    sBtnA_last  = true, sBtnB_last = true;

// Flag to track if we've set CH422G to input mode
static bool s_ch422g_input_mode_set = false;

void input_configure_pushbuttons(int gateA_exio, int gateB_exio, bool active_low)
{
  s_btn_gateA_exio = gateA_exio;
  s_btn_gateB_exio = gateB_exio;
  s_btn_active_low = active_low;

  Serial.printf("[Input] Configured pushbuttons: A=IO%d, B=IO%d, active_low=%d\n",
                gateA_exio, gateB_exio, active_low);

  // Seed last states
  uint8_t snap = read_inputs_raw();
  if (s_btn_gateA_exio >= 0) {
    sBtnA_last = (snap & (1u << s_btn_gateA_exio)) != 0;
    sBtnA_count = STABLE_COUNT;
  }
  if (s_btn_gateB_exio >= 0) {
    sBtnB_last = (snap & (1u << s_btn_gateB_exio)) != 0;
    sBtnB_count = STABLE_COUNT;
  }
  
  Serial.printf("[Input] Initial snapshot: 0x%02X (btnA_last=%d, btnB_last=%d)\n",
                snap, sBtnA_last, sBtnB_last);
}

// ----------------- CH422G input mode setup ---------------------------------

static bool ensure_ch422g_input_mode() {
  if (s_ch422g_input_mode_set) return true;
  
  esp_io_expander_handle_t h = hal::expander_get_handle();
  if (!h) return false;
  
  // We'll handle input mode switching on each read, so just mark as initialized
  s_ch422g_input_mode_set = true;
  Serial.println("[Input] CH422G initialized for dynamic input/output switching");
  return true;
}

// ----------------- Pause/Resume --------------------------------------------

void input_pause()
{
  if (!sPaused) {
    sPaused = true;
    Serial.println("[Input] Polling PAUSED");
  }
}

void input_resume()
{
  if (sPaused) {
    sPaused = false;
    Serial.println("[Input] Polling RESUMED");
  }
}

bool input_is_paused()
{
  return sPaused;
}

// ----------------- Existing API functions ----------------------------------

void input_init()
{
    Serial.println("[Input] Initializing...");
    
    // CRITICAL: Ensure CH422G is in input mode before first read
    if (!ensure_ch422g_input_mode()) {
      Serial.println("[Input] WARNING: Failed to set CH422G input mode!");
    }
    
    // Small delay after mode change
    delay(10);
    
    uint8_t snap = read_inputs_raw();
    Serial.printf("[Input] Initial raw snapshot: 0x%02X\n", snap);

    gGateALevel  = (snap & GATE_A_MASK) != 0;
    gGateBLevel  = (snap & GATE_B_MASK) != 0;

    sA_last  = gGateALevel;
    sB_last  = gGateBLevel;
    sA_count = STABLE_COUNT;
    sB_count = STABLE_COUNT;

    if (s_btn_gateA_exio >= 0) {
      sBtnA_last = (snap & (1u << s_btn_gateA_exio)) != 0;
      sBtnA_count = STABLE_COUNT;
    }
    if (s_btn_gateB_exio >= 0) {
      sBtnB_last = (snap & (1u << s_btn_gateB_exio)) != 0;
      sBtnB_count = STABLE_COUNT;
    }

    gate_engine_init();
    Serial.printf("[Input] Init complete. GateA=%d, GateB=%d (poll=%lums, debounce=%u)\n", 
                  gGateALevel, gGateBLevel, POLL_PERIOD_MS, STABLE_COUNT);
}

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
        btns.prevSelect = lvl_to_digital(gGateALevel);
        btns.prevDown   = lvl_to_digital(gGateBLevel);
        return;
    }
    sLastPollMs = now;

    uint8_t snap = read_inputs_raw();

    // Debug: only log when snapshot changes to reduce serial spam
    static uint8_t last_logged_snap = 0xFF;
    static bool first_log = true;
    if (snap != last_logged_snap || first_log) {
        Serial.printf("[DBG] snap=0x%02X btnA_last=%d cntA=%u btnB_last=%d cntB=%u gA=%d gB=%d\n",
                      snap,
                      sBtnA_last, sBtnA_count,
                      sBtnB_last, sBtnB_count,
                      gGateALevel, gGateBLevel);
        last_logged_snap = snap;
        first_log = false;
    }

    // Determine raw samples for gates
    bool sampleA_raw = (snap & GATE_A_MASK) != 0;
    bool sampleB_raw = (snap & GATE_B_MASK) != 0;

    // If pushbutton mapped for gate A — skip if same pin as gate
    if (s_btn_gateA_exio >= 0 && s_btn_gateA_exio != GATE_A_DI_INDEX) {
      bool btn_sample = (snap & (1u << s_btn_gateA_exio)) != 0;
      if (btn_sample == sBtnA_last) {
        if (sBtnA_count < 255) sBtnA_count++;
      } else {
        sBtnA_count = 1;
        sBtnA_last = btn_sample;
      }
      if (sBtnA_count >= STABLE_COUNT) {
        bool pressed = s_btn_active_low ? !sBtnA_last : sBtnA_last;
        sampleA_raw = !pressed; // pressed -> BLOCKED (false), released -> OPEN (true)
      }
    }

    // If pushbutton mapped for gate B — skip if same pin as gate
    if (s_btn_gateB_exio >= 0 && s_btn_gateB_exio != GATE_B_DI_INDEX) {
      bool btn_sample = (snap & (1u << s_btn_gateB_exio)) != 0;
      if (btn_sample == sBtnB_last) {
        if (sBtnB_count < 255) sBtnB_count++;
      } else {
        sBtnB_count = 1;
        sBtnB_last = btn_sample;
      }
      if (sBtnB_count >= STABLE_COUNT) {
        bool pressed = s_btn_active_low ? !sBtnB_last : sBtnB_last;
        sampleB_raw = !pressed;
      }
    }

    bool sampleA = sampleA_raw;
    bool sampleB = sampleB_raw;

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
            
            LOG_D("GATE", "A: %s", gGateALevel ? "OPEN" : "BLOCKED");

            // Only record gate events when experiment is ARMED or RUNNING
            if (experiment_should_poll_gates()) {
                if (input_edge_falling(prev_d, curr_d)) {
                    gate_trigger(GATE_A);
                    gate_block_start(GATE_A);
                    Serial.printf("[GATE DBG] Gate A FALLING (BLOCK): prev=%d curr=%d\n", prev_d, curr_d);
                } else if (input_edge_rising(prev_d, curr_d)) {
                    gate_block_end(GATE_A);
                    Serial.printf("[GATE DBG] Gate A RISING (UNBLOCK): prev=%d curr=%d\n", prev_d, curr_d);
                }
            } else {
                // Log ignored events for debugging
                if (input_edge_falling(prev_d, curr_d)) {
                    Serial.printf("[GATE DBG] Gate A FALLING (BLOCK) - IGNORED (not armed): prev=%d curr=%d\n", prev_d, curr_d);
                } else if (input_edge_rising(prev_d, curr_d)) {
                    Serial.printf("[GATE DBG] Gate A RISING (UNBLOCK) - IGNORED (not armed): prev=%d curr=%d\n", prev_d, curr_d);
                }
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
            
            LOG_D("GATE", "B: %s", gGateBLevel ? "OPEN" : "BLOCKED");

            // Only record gate events when experiment is ARMED or RUNNING
            if (experiment_should_poll_gates()) {
                if (input_edge_falling(prev_d, curr_d)) {
                    gate_trigger(GATE_B);
                    gate_block_start(GATE_B);
                    Serial.printf("[GATE DBG] Gate B FALLING (BLOCK): prev=%d curr=%d\n", prev_d, curr_d);
                } else if (input_edge_rising(prev_d, curr_d)) {
                    gate_block_end(GATE_B);
                    Serial.printf("[GATE DBG] Gate B RISING (UNBLOCK): prev=%d curr=%d\n", prev_d, curr_d);
                }
            } else {
                // Log ignored events for debugging
                if (input_edge_falling(prev_d, curr_d)) {
                    Serial.printf("[GATE DBG] Gate B FALLING (BLOCK) - IGNORED (not armed): prev=%d curr=%d\n", prev_d, curr_d);
                } else if (input_edge_rising(prev_d, curr_d)) {
                    Serial.printf("[GATE DBG] Gate B RISING (UNBLOCK) - IGNORED (not armed): prev=%d curr=%d\n", prev_d, curr_d);
                }
            }
        }
    }

    btns.prevSelect = lvl_to_digital(gGateALevel);
    btns.prevDown   = lvl_to_digital(gGateBLevel);
}

// Stub for optional callback (if header declares it)
static input_button_cb_t s_button_cb = nullptr;
void input_set_button_callback(input_button_cb_t cb) {
    s_button_cb = cb;
}