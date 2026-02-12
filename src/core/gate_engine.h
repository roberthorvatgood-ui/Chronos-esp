#pragma once
#include <stdint.h>

/**
 * Gate Engine Header
 * ------------------
 * Provides:
 * - GateID enum for A/B/C
 * - Legacy single timestamp API (gate_trigger / gate_timestamp)
 * - Block/unblock range capture per gate (for two‑gate UA)
 * - Inline simulation helpers for UI testing
 * - Gate state query for GUI animation
 */

// Identify gates by index
enum GateID : uint8_t {
  GATE_A = 0, // First gate
  GATE_B = 1, // Second gate
  GATE_C = 2  // (unused for UA two‑gate; kept for other modes)
};

// ---- Lifecycle ----
void     gate_engine_init(void);                // full reset (all timestamps + block ranges + flags)

// ---- Targeted clears (safe to call mid-experiment) ----
void     gate_clear_trigger_timestamps(void);   // clears only simple timestamps (CV, Photogate, FreeFall)
void     gate_clear_block_ranges(void);         // clears only block start/end (UA, Incline)

// ---- Legacy simple trigger API (kept for CV/Photogate) ----
void     gate_trigger(GateID id);             // single timestamp (microseconds since boot)
uint64_t gate_timestamp(GateID id);           // read last single timestamp

// ---- Block/unblock capture API for two‑gate UA ----
// Call at the light beam "block" (front edge)
void     gate_block_start(GateID id);
// Call at the light beam "unblock" (rear edge)
void     gate_block_end(GateID id);

// Returns true if a valid start exists; out = start us
bool     gate_get_last_block_start_us(GateID gate, uint64_t& t_us);

// Returns true if a valid (start,end) pair exists with end > start
bool     gate_get_last_block_range_us(GateID gate, uint64_t& t_start_us, uint64_t& t_end_us);

// ---- Gate state query for GUI animation ----
// Query if gate is currently blocked (ISR-safe, read-only)
bool     gate_is_blocked(GateID id);

// ---- Simulation helpers for UI testing ----
inline void gate_simulate_gate_a() { gate_trigger(GATE_A); }
inline void gate_simulate_gate_b() { gate_trigger(GATE_B); }
inline void gate_simulate_gate_c() { gate_trigger(GATE_C); }

// For UA press/hold simulation (A/B separate)
inline void gate_simulate_block_a()   { gate_block_start(GATE_A); }
inline void gate_simulate_unblock_a() { gate_block_end  (GATE_A); }
inline void gate_simulate_block_b()   { gate_block_start(GATE_B); }
inline void gate_simulate_unblock_b() { gate_block_end  (GATE_B); }

// Photogate legacy mapping (kept for that screen):
// Treat Block as Gate A, Unblock as Gate B
inline void gate_simulate_block()   { gate_trigger(GATE_A); }
inline void gate_simulate_unblock() { gate_trigger(GATE_B); }