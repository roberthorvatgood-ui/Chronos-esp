
#include "gate_engine.h"
#include <Arduino.h>
#include <esp_timer.h>

/**
 * Gate Engine Implementation
 * --------------------------
 * 1) Legacy single-timestamp path (A/B/C) used by CV/Photogate screens.
 * 2) Per-gate block range capture (start/end) used by two‑gate UA.
 */

// --- Legacy simple timestamps (A/B/C) ---
static uint64_t timestamps[3] = {0, 0, 0};

// --- Block/unblock capture state ---
static uint64_t last_block_start[3] = {0,0,0};
static uint64_t last_block_end  [3] = {0,0,0};

void gate_engine_init()
{
  for (int i = 0; i < 3; ++i) timestamps[i] = 0;
  for (int i = 0; i < 3; ++i) {
    last_block_start[i] = 0;
    last_block_end  [i] = 0;
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
