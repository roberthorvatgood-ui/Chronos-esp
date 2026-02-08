/*
 * gate_input.h
 * Gate input polling control
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Pause gate input polling (call during screensaver/AP-web to reduce I²C contention)
void gate_input_pause();

// Resume gate input polling
void gate_input_resume();

// Check if polling is currently paused
bool gate_input_is_paused();

#ifdef __cplusplus
}
#endif