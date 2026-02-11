/*****
 * input.cpp
 * Handles photogate inputs (CH422G expander) and optional pushbutton-to-gate mapping.
 * I2C expander reads are executed via the central hal_i2c_executor to avoid blocking core1/ISRs.
 *
 * FIX (2026-02-07): Ensure CH422G is in input mode and use correct read sequence
 *****/

#include <Arduino.h>
#include "esp_err.h"

#include "../drivers/hal_panel.h"       // hal::expander_get_handle()
#include "../drivers/hal_i2c_executor.h"// hal_i2c_exec_sync
#include "../drivers/hal_i2c_manager.h" // fallback mutex
#include "../core/gate_engine.h"
#include "../core/event_bus.h"          // EventBus for gate events
#include "input.h"

// External event bus (defined in main .ino)
extern EventBus gBus;

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

// Adjust these for testing
static constexpr uint8_t STABLE_COUNT = 2;      // Require 2 stable samples (40ms total)
static const unsigned long POLL_PERIOD_MS = 20; // Poll every 20ms (50Hz, safe for I²C)

// Throttling
static unsigned long sLastPollMs = 0;

// Gate input polling pause control
static bool gInputPaused = false;

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

// ----------------- Executor-backed expander read ---------------------------

struct expander_read_ctx {
  esp_io_expander_handle_t h;
  uint32_t mask;
  uint32_t out;
};

// Read the CH422G input pins using get_level (which reads inputs when pins are configured as INPUT)
static esp_err_t expander_read_op(void* vctx) {
  if (!vctx) return ESP_ERR_INVALID_ARG;
  expander_read_ctx* ctx = static_cast<expander_read_ctx*>(vctx);
  if (!ctx->h) return ESP_ERR_INVALID_ARG;
  
  uint32_t val = 0;
  esp_err_t r = esp_io_expander_get_level(ctx->h, ctx->mask, &val);
  if (r == ESP_OK) ctx->out = val;
  return r;
}

static uint8_t read_inputs_raw()
{
    esp_io_expander_handle_t h = hal::expander_get_handle();
    if (!h) {
        // Not ready yet; treat as all HIGH (open)
        return 0xFF;
    }

    // Strategy: Temporarily enable all-input mode, read, then restore outputs
    // This is necessary because CH422G doesn't support reliable mixed-mode I/O
    
    // Step 1: Enable all-input mode for reading
    esp_err_t err = esp_io_expander_ch422g_set_all_input(h);
    if (err != ESP_OK) {
        Serial.printf("[Input] Failed to enable input mode for read: 0x%x\n", err);
        return 0xFF;
    }
    
    // Step 2: Read the input pins
    expander_read_ctx ctx;
    ctx.h = h;
    ctx.mask = 0xFF; // Read all 8 pins
    ctx.out = 0xFF;  // Default to all high

    err = hal_i2c_exec_sync(expander_read_op, &ctx, 300);
    uint8_t snapshot = 0xFF;
    
    if (err == ESP_OK) {
        snapshot = static_cast<uint8_t>(ctx.out & 0xFF);
    } else {
        // Fallback: direct read with mutex
        if (hal::i2c_lock(50)) {
            uint32_t v = 0;
            esp_err_t r = esp_io_expander_get_level(h, 0xFF, &v);
            hal::i2c_unlock();
            if (r == ESP_OK) {
                snapshot = static_cast<uint8_t>(v & 0xFF);
            }
        }
    }
    
    // Step 3: IMMEDIATELY restore output pins (critical for SD and LCD)
    const uint32_t output_pins = (1u << 2) | (1u << 4);  // IO2=LCD_BL, IO4=SD_CS
    
    err = esp_io_expander_set_dir(h, output_pins, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        Serial.printf("[Input] Warning: Failed to restore outputs: 0x%x\n", err);
    }
    
    // Step 4: Set SD_CS high (deselect SD card)
    err = esp_io_expander_set_level(h, (1u << 4), 1);
    if (err != ESP_OK) {
        Serial.printf("[Input] Warning: Failed to set SD_CS: 0x%x\n", err);
    }
    
    return snapshot;
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

// Executor operation to set CH422G to all-input mode
struct ch422g_set_input_ctx {
  esp_io_expander_handle_t h;
};

static bool ensure_ch422g_input_mode() {
  if (s_ch422g_input_mode_set) return true;
  
  esp_io_expander_handle_t h = hal::expander_get_handle();
  if (!h) return false;
  
  // We'll handle input mode switching on each read, so just mark as initialized
  s_ch422g_input_mode_set = true;
  Serial.println("[Input] CH422G initialized for dynamic input/output switching");
  return true;
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
    Serial.printf("[Input] Init complete. GateA=%d, GateB=%d\n", 
                  gGateALevel, gGateBLevel);
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
    // Check if polling is paused (e.g., during screensaver or AP-web)
    if (gInputPaused) {
        btns.prevSelect = lvl_to_digital(gGateALevel);
        btns.prevDown   = lvl_to_digital(gGateBLevel);
        return;
    }

    unsigned long now = millis();
    if (now - sLastPollMs < POLL_PERIOD_MS) {
        btns.prevSelect = lvl_to_digital(gGateALevel);
        btns.prevDown   = lvl_to_digital(gGateBLevel);
        return;
    }
    sLastPollMs = now;

    uint8_t snap = read_inputs_raw();

    // Determine raw samples for gates
    bool sampleA_raw = (snap & GATE_A_MASK) != 0;
    bool sampleB_raw = (snap & GATE_B_MASK) != 0;

    // If pushbutton mapped for gate A
    if (s_btn_gateA_exio >= 0) {
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

    // If pushbutton mapped for gate B
    if (s_btn_gateB_exio >= 0) {
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

            if (input_edge_falling(prev_d, curr_d)) {
                gate_trigger(GATE_A);
                gate_block_start(GATE_A);
                gBus.publish(EVT_GATE_A_FALL, (uint32_t)millis());
                Serial.printf("[GATE DBG] Gate A FALLING (BLOCK): prev=%d curr=%d\n", prev_d, curr_d);
            } else if (input_edge_rising(prev_d, curr_d)) {
                gate_block_end(GATE_A);
                gBus.publish(EVT_GATE_A_RISE, (uint32_t)millis());
                Serial.printf("[GATE DBG] Gate A RISING (UNBLOCK): prev=%d curr=%d\n", prev_d, curr_d);
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
                gBus.publish(EVT_GATE_B_FALL, (uint32_t)millis());
                Serial.printf("[GATE DBG] Gate B FALLING (BLOCK): prev=%d curr=%d\n", prev_d, curr_d);
            } else if (input_edge_rising(prev_d, curr_d)) {
                gate_block_end(GATE_B);
                gBus.publish(EVT_GATE_B_RISE, (uint32_t)millis());
                Serial.printf("[GATE DBG] Gate B RISING (UNBLOCK): prev=%d curr=%d\n", prev_d, curr_d);
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

// Gate input polling pause control functions
void input_pause() {
    if (!gInputPaused) {
        gInputPaused = true;
        Serial.println("[Input] Polling PAUSED");
    }
}

void input_resume() {
    if (gInputPaused) {
        gInputPaused = false;
        Serial.println("[Input] Polling RESUMED");
    }
}

bool input_is_paused() {
    return gInputPaused;
}