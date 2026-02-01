#include "hal_panel.h"
#include <stdio.h>
#include <esp_log.h>
#include <esp_io_expander.hpp>

static const char *TAG = "hal_panel";

// Platform expander handle - must be initialized by the platform HAL layer
// before calling expander_pinMode/digitalWrite/digitalRead functions.
// Expected initialization: Platform HAL should create the CH422G expander instance
// and assign it to s_hal_expander (e.g., via a setter function or direct assignment).
// TODO: Add public setter function like expander_set_handle() or integrate with
// platform-specific initialization code that creates the esp_io_expander instance.
static esp_io_expander_handle_t s_hal_expander = nullptr;

bool expander_pinMode(uint8_t exio, bool output) {
    if (!s_hal_expander) return false;
    const uint32_t mask = (1u << exio);
    return esp_io_expander_set_dir(
        s_hal_expander,
        mask,
        output ? IO_EXPANDER_OUTPUT : IO_EXPANDER_INPUT
    ) == ESP_OK;
}

bool expander_digitalWrite(uint8_t exio, bool high) {
    if (!s_hal_expander) return false;
    const uint32_t mask = (1u << exio);
    return esp_io_expander_set_level(s_hal_expander, mask, high ? 1 : 0) == ESP_OK;
}

bool expander_digitalRead(uint8_t exio, bool &out_level) {
    if (!s_hal_expander) return false;
    const uint32_t mask = (1u << exio);

#if defined(ESP_IO_EXPANDER_HAS_GET_LEVEL)
    int level = 0;
    if (esp_io_expander_get_level(s_hal_expander, mask, &level) == ESP_OK) {
        out_level = (level != 0);
        return true;
    }
    return false;
#else
    uint8_t port_val = 0;
    if (esp_io_expander_read_port(s_hal_expander, &port_val) == ESP_OK) {
        out_level = ((port_val & mask) != 0);
        return true;
    }
    return false;
#endif
}