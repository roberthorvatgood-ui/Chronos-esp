
/*****
 * hal_panel.cpp
 * Waveshare ESP32‑S3‑Touch‑LCD‑5 – HAL wrapper
 * [Updated: 2026-01-19 23:58 CET]
 *  - Creates CH422G via C API: esp_io_expander_new_i2c_ch422g(I2C_NUM_0, addr, &handle)
 *  - Bounded wait for expander readiness
 *  - Honor begin(start_backlight_on)
 *  - IO‑expander helpers use pin mask (1u<<exio)
 *****/
#include "hal_panel.h"
#include "hal_i2c_manager.h"
#include <Arduino.h>
#include "driver/ledc.h"

// Your installed expander C API (as seen in your error header path)
#if defined(__has_include)
  #if __has_include(<port/esp_io_expander_ch422g.h>)
    #include <port/esp_io_expander_ch422g.h>
  #elif __has_include(<esp_io_expander_ch422g.h>)
    #include <esp_io_expander_ch422g.h>
  #endif
#endif

using namespace esp_panel::drivers;

namespace {
// Optional PWM dimmer parameters (for AP3032 CTRL wire).
// Waveshare board ships with CH422G DISP (EXIO2) for on/off.
// For REAL dimming, wire an ESP32 GPIO to AP3032 CTRL and set BL_GPIO.
constexpr int  BL_GPIO        = -1;          // <— set to your wired GPIO, or keep -1
constexpr int  BL_PWM_CHANNEL = 3;
constexpr int  BL_PWM_TIMER   = 0;
constexpr auto BL_PWM_FREQ_HZ = 20000;       // 20 kHz
constexpr auto BL_PWM_RES     = LEDC_TIMER_10_BIT; // 0..1023
constexpr auto BL_SPEED_MODE  = LEDC_LOW_SPEED_MODE;

ESP_Panel* g_panel = nullptr;
static bool s_ledc_timer_inited = false;
static bool s_ledc_chan_inited  = false;

// Single expander handle owned by the panel/board stack.
static esp_io_expander_handle_t s_hal_expander = nullptr;

static void ledc_init_timer_if_needed() {
  if (s_ledc_timer_inited || BL_GPIO < 0) return;
  ledc_timer_config_t tcfg{};
  tcfg.speed_mode       = BL_SPEED_MODE;
  tcfg.timer_num        = static_cast<ledc_timer_t>(BL_PWM_TIMER);
  tcfg.duty_resolution  = BL_PWM_RES;
  tcfg.freq_hz          = BL_PWM_FREQ_HZ;
  tcfg.clk_cfg          = LEDC_AUTO_CLK;
  if (ledc_timer_config(&tcfg) == ESP_OK) s_ledc_timer_inited = true;
}
static void ledc_init_channel_if_needed() {
  if (s_ledc_chan_inited || BL_GPIO < 0) return;
  ledc_channel_config_t ccfg{};
  ccfg.speed_mode = BL_SPEED_MODE;
  ccfg.channel    = static_cast<ledc_channel_t>(BL_PWM_CHANNEL);
  ccfg.timer_sel  = static_cast<ledc_timer_t>(BL_PWM_TIMER);
  ccfg.intr_type  = LEDC_INTR_DISABLE;
  ccfg.gpio_num   = static_cast<gpio_num_t>(BL_GPIO);
  ccfg.duty       = 0;
  ccfg.hpoint     = 0;
  if (ledc_channel_config(&ccfg) == ESP_OK) s_ledc_chan_inited = true;
}
static void ledc_set_percent(uint8_t percent) {
  if (BL_GPIO < 0) return;                // not wired
  if (!s_ledc_timer_inited) ledc_init_timer_if_needed();
  if (!s_ledc_chan_inited ) ledc_init_channel_if_needed();
  if (!s_ledc_chan_inited ) return;
  if (percent > 100) percent = 100;
  const uint32_t maxDuty = (1u << BL_PWM_RES) - 1u;
  const uint32_t duty    = (percent * maxDuty) / 100u;
  ledc_set_duty(BL_SPEED_MODE, static_cast<ledc_channel_t>(BL_PWM_CHANNEL), duty);
  ledc_update_duty(BL_SPEED_MODE, static_cast<ledc_channel_t>(BL_PWM_CHANNEL));
}

// Our board uses I2C0 (SDA=8, SCL=9) already initialized by ESP_Panel.
// The C API creator only needs the I2C port and the I2C address.
#ifdef ESP_IO_EXPANDER_I2C_CH422G_ADDRESS
  static constexpr uint32_t CH422G_ADDR = ESP_IO_EXPANDER_I2C_CH422G_ADDRESS;
#else
  static constexpr uint32_t CH422G_ADDR = 0x24; // typical CH422G address
#endif

static void create_and_attach_ch422g_if_needed() {
  if (s_hal_expander) return;

  // Try C API creation (matches your installed header signature)
  esp_io_expander_handle_t h = nullptr;

  // Function signature from: port/esp_io_expander_ch422g.h
  //   esp_err_t esp_io_expander_new_i2c_ch422g(i2c_port_t i2c_num,
  //                                            uint32_t i2c_address,
  //                                            esp_io_expander_handle_t *handle);
  if (esp_io_expander_new_i2c_ch422g(I2C_NUM_0, CH422G_ADDR, &h) == ESP_OK && h) {
    s_hal_expander = h;
    Serial.println("[HAL] CH422G handle created via C API (I2C0)");
  } else {
    Serial.println("[HAL] Failed to create CH422G via C API");
  }
}

} // namespace

namespace hal {

bool init() {
  if (g_panel) return true;
  
  // Initialize I2C manager first
  i2c_manager_init();
  
  g_panel = new ESP_Panel();
  if (!g_panel) return false;

  // Minimal init for LCD + Touch (no backlight policy yet).
  g_panel->init();
  if (g_panel->getLcd()  ) g_panel->getLcd()->init();
  if (g_panel->getTouch()) g_panel->getTouch()->init();
  return true;
}

bool begin(bool start_backlight_on) {
  if (!g_panel) return false;

  // Bring up board/buses; ESP‑Panel sets up I2C/CH422G at the board level.
  g_panel->begin();

  // Create CH422G locally via C API and attach
  create_and_attach_ch422g_if_needed();

  // BOUNDED wait so first users (e.g., SD) can rely on readiness
  (void)expander_wait_ready(800); // <= 0.8 s max

  // Honor requested initial backlight state
  if (start_backlight_on) backlight_on();
  return true;
}

drivers::LCD*   lcd()   { return g_panel ? g_panel->getLcd()   : nullptr; }
drivers::Touch* touch() { return g_panel ? g_panel->getTouch() : nullptr; }

bool lvgl_init() {
  return lvgl_port_init(lcd(), touch());
}

// ── Backlight ──────────────────────────────────────────────────────────────
void backlight_on()  {
  if (g_panel && g_panel->getBacklight()) g_panel->getBacklight()->on();
  else                                    ledc_set_percent(100);
}
void backlight_off() {
  if (g_panel && g_panel->getBacklight()) g_panel->getBacklight()->off();
  else                                    ledc_set_percent(0);
}
void backlight_set(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (g_panel && g_panel->getBacklight()) {
    if (percent == 0) g_panel->getBacklight()->off();
    else              g_panel->getBacklight()->on();
  }
  ledc_set_percent(percent); // no‑op unless BL_GPIO >= 0
}

// ── IO Expander helpers ────────────────────────────────────────────────────
bool expander_ready() {
  return s_hal_expander != nullptr;
}
bool expander_wait_ready(uint32_t timeout_ms) {
  if (s_hal_expander) return true;
  if (timeout_ms == 0) return false;
  const uint32_t t0 = millis();
  while (!s_hal_expander && (millis() - t0) < timeout_ms) {
    delay(10);
  }
  return s_hal_expander != nullptr;
}

bool expander_pinMode(uint8_t exio, bool output) {
  if (!s_hal_expander) return false;
  const uint32_t mask = (1u << exio);
  return esp_io_expander_set_dir(
           s_hal_expander, mask,
           output ? IO_EXPANDER_OUTPUT : IO_EXPANDER_INPUT) == ESP_OK;
}

bool expander_digitalWrite(uint8_t exio, bool high) {
  if (!s_hal_expander) return false;
  const uint32_t mask = (1u << exio);
  return esp_io_expander_set_level(s_hal_expander, mask, high ? 1 : 0) == ESP_OK;
}

void expander_attach(esp_io_expander_handle_t h) {
  s_hal_expander = h;
  if (s_hal_expander) {
    Serial.println("[HAL] IO expander handle attached (manual/local)");
  }
}

esp_io_expander_handle_t expander_get_handle() {
  return s_hal_expander;
}

} // namespace hal
