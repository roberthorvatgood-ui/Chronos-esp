#include "hal_panel.h"
#include <stdio.h>
#include <esp_log.h>
#include <esp_io_expander.hpp>

static const char *TAG = "hal_panel";

// extern or static handle to the platform expander instance
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