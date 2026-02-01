#pragma once

#include <stdint.h>
#include <stdbool.h>

// Hardware Abstraction Layer (HAL) for panel expander
// Public APIs for controlling and reading external IO (EXIO) lines provided
// by the CH422G / esp_io_expander backend.

/** Configure an EXIO pin direction: output=true, input=false. */
bool expander_pinMode(uint8_t exio, bool output);

/** Set an EXIO pin level: high=true, low=false. */
bool expander_digitalWrite(uint8_t exio, bool high);

/*
    Read a single EXIO pin.
    Returns true on success and writes level (true=HIGH) into out_level.
    Note: this wrapper uses the esp_io_expander backend if available.
    If the backend read API is not present in your environment, this
    function returns false so callers can fall back or report the error.
*/
bool expander_digitalRead(uint8_t exio, bool &out_level);

// Optional helper that waits for the platform HAL to attach/initialize
// the expander. Implementations may provide it; callers should guard with
// #ifdef HAL_PANEL_HAS_EXPANDER_WAIT when relying on it.

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAL_PANEL_HAS_EXPANDER_WAIT
bool expander_wait_ready(uint32_t timeout_ms);
#endif

#ifdef __cplusplus
}
#endif
