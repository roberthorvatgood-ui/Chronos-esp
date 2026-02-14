#include "gate_engine.h"
#include <Arduino.h>
#include <esp_timer.h>

/**
 * Gate Engine Implementation
 * --------------------------
 * 1) Legacy single-timestamp path (A/B/C) used by CV screen only.
 * 2) Per-gate block range capture (start/end) used by Photogate, FreeFall, UA, and Incline.
 * 3) Gate state tracking for GUI animation (ISR-safe flags).
 *
 * IMPORTANT: experiments_clear_timestamps() used to call gate_engine_init()
 * which nuked everything. Now experiments should call the targeted clears:
 *   - gate_clear_trigger_timestamps()  for CV
 *   - gate_clear_block_ranges()        for Photogate/FreeFall/UA/Incline
 * gate_engine_init() is reserved for full system reset only.
 */

// --- Legacy simple timestamps (A/B/C) ---
static uint64_t timestamps[3] = {0, 0, 0};

// --- Block/unblock capture state ---
static uint64_t last_block_start[3] = {0,0,0};
static uint64_t last_block_end  [3] = {0,0,0};

// --- GUI animation state (ISR-safe flags) ---
static volatile bool gate_blocked[3] = {false, false, false};

void gate_engine_init()
{
  for (int i = 0; i < 3; ++i) timestamps[i] = 0;
  for (int i = 0; i < 3; ++i) {
    last_block_start[i] = 0;
    last_block_end  [i] = 0;
    gate_blocked[i] = false;
  }
}

void gate_clear_trigger_timestamps()
{
  for (int i = 0; i < 3; ++i) timestamps[i] = 0;
}

void gate_clear_block_ranges()
{
  for (int i = 0; i < 3; ++i) {
    last_block_start[i] = 0;
    last_block_end  [i] = 0;
    // NOTE: do NOT touch gate_blocked[] — that tracks live hardware state
  }
}

void gate_trigger(GateID id)
{
  if (id > GATE_C) return;
  timestamps[id] = esp_timer_get_time(); // microseconds since boot
  Serial.printf("[Gate] Trigger %d at %llu us\n", id, (unsigned long long)timestamps[id]);
}

uint64_t gate_timestamp(GateID id)
{
  return (id <= GATE_C) ? timestamps[id] : 0;
}

void gate_block_start(GateID id)
{
  if (id > GATE_C) return;
  last_block_start[id] = esp_timer_get_time();
  // When a new block starts, clear any stale end if it's older than start
  if (last_block_end[id] <= last_block_start[id]) {
    last_block_end[id] = 0;
  }
  Serial.printf("[Gate] Block START %d at %llu us\n", id, (unsigned long long)last_block_start[id]);
  
  // Set flag for GUI animation (ISR-safe)
  gate_blocked[id] = true;
}

void gate_block_end(GateID id)
{
  if (id > GATE_C) return;
  const uint64_t now = esp_timer_get_time();
  // Only set an end if there was a start and end > start
  if (last_block_start[id] > 0 && now > last_block_start[id]) {
    last_block_end[id] = now;
    Serial.printf("[Gate] Block END   %d at %llu us (dur %llu us)\n",
                  id,
                  (unsigned long long)last_block_end[id],
                  (unsigned long long)(last_block_end[id] - last_block_start[id]));
  } else {
    // No start or invalid ordering; ignore
    Serial.printf("[Gate] Block END   %d ignored (no valid start)\n", id);
  }
  
  // Clear flag for GUI animation (ISR-safe)
  gate_blocked[id] = false;
}

bool gate_get_last_block_start_us(GateID gate, uint64_t& t_us)
{
  if (gate > GATE_C) return false;
  if (last_block_start[gate] == 0) return false;
  t_us = last_block_start[gate];
  return true;
}

bool gate_get_last_block_range_us(GateID gate, uint64_t& t_start_us, uint64_t& t_end_us)
{
  if (gate > GATE_C) return false;
  if (last_block_start[gate] == 0 || last_block_end[gate] == 0) return false;
  if (last_block_end[gate] <= last_block_start[gate]) return false;
  t_start_us = last_block_start[gate];
  t_end_us   = last_block_end  [gate];
  return true;
}

bool gate_is_blocked(GateID id)
{
  if (id > GATE_C) return false;
  return gate_blocked[id];
}