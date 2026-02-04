
/*****
 * Chronos – Main Application (.ino)
 * Integrated SD Export + Web UI
 * [Updated: 2026-01-25 13:30 CET]
 * 
 * ⚠️ IMPORTANT: I2C THREAD SAFETY
 * ──────────────────────────────────────────────────────────────────────────
 * All I2C operations (expander, RTC, SD CS control) are protected by a FreeRTOS
 * mutex (hal::i2c_lock/unlock) to prevent bus contention.
 * 
 * LVGL CALLBACK WARNING:
 * - DO NOT perform blocking I2C operations directly in LVGL event callbacks
 * - DO NOT call hal::i2c_lock from LVGL callbacks running on CPU1
 * - If you need I2C access from GUI, defer to a task or use async notification
 * 
 * For safe I2C access in application code, use:
 *   if (hal::i2c_lock(timeout_ms)) {
 *     // ... perform I2C operations ...
 *     hal::i2c_unlock();
 *   }
 *****/

#include <Arduino.h>
#include <Preferences.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif

#include "src/drivers/hal_panel.h"
#include "src/gui/gui.h"
#include "src/net/app_network.h"
#include "src/core/event_bus.h"
#include "src/core/app_controller.h"
#include "src/io/input.h"
#include "src/intl/i18n.h"
#include "src/core/gate_engine.h"
#include "src/experiments/experiments.h"
#include "src/gui/screensaver.h"
#include "src/core/rtc_manager.h"
#include "src/core/pcf85063a_hooks.h"

// ── Export system (SD + Web UI) ────────────────────────────────────────────────
#include "src/export/export_fs.h"
#include "src/export/chronos_sd.h"
#include "src/export/web_export.h"

static unsigned long gLastExpanderDebugMs = 0;

// [2026-01-24 22:58 CET] ADD (Copilot): query if AP web is in FS critical section
namespace chronos { bool apweb_fs_busy(); }

EventBus gBus;
AppController gApp;

uint32_t gScreenSaverTimeoutS = 60;     // 0 disables screensaver
unsigned long gLastUserActivityMs = 0;  // last activity timestamp
bool gScreenSaverActive = false;        // current screensaver state
volatile bool gWakeFromSaverPending = false;
volatile bool gWakeInProgress = false;  // avoid double wake

extern uint64_t gui_get_stopwatch_us() {
  return gApp.sw.elapsed_us();
}

// Mark any bus event as user activity + dispatch to controller
static void onBusEvent(const Event& e) {
  gui_note_user_activity();
  gApp.on_event(e);
}

// ---- General prefs cache (Wi‑Fi ON/OFF) ----
static bool load_wifi_pref_on() {
  Preferences prefs;
  bool on = false;
  if (prefs.begin("general", true)) {
    on = prefs.getBool("wifi_on", false);  // default OFF
    prefs.end();
  }
  return on;
}

// ---- Load screensaver timeout (seconds) ----
static void load_screensaver_timeout() {
  Preferences prefs;
  if (prefs.begin("display", true)) {
    gScreenSaverTimeoutS = prefs.getUInt("ss_to_s", 60);  // 0 disables
    prefs.end();
  }
  Serial.printf("[Setup] Screensaver timeout (s): %lu\n", (unsigned long)gScreenSaverTimeoutS);
}

// [2026-01-18 22:20 CET] UPDATED: setup() order – apply Wi‑Fi preference before AP start
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  // --- Make CH422G the first owner of the I²C bus (prevents ESP_FAIL) ---
  if (!chronos_sd_preinit()) {
    Serial.println("[BOOT] CH422G preinit failed (will retry on export)");  // non-fatal
  }

  esp_reset_reason_t r = esp_reset_reason();
  Serial.printf("\n[BOOT] reset_reason=%d\n", (int)r);
  delay(1000);

  Serial.println("SETUP START");

  i18n_init();
  i18n_load_saved();
  Serial.printf("[i18n] Boot language: %s\n", i18n_get_lang_code());
  load_screensaver_timeout();

  if (!hal::init())  Serial.println("[HAL] init() failed");
  if (!hal::begin()) Serial.println("[HAL] begin() failed");
  if (!hal::lvgl_init()) Serial.println("[HAL] lvgl_init() failed");

  // --- Network bring-up honoring saved General Settings (apply first) ---
  const bool wifi_on = load_wifi_pref_on();
  network_set_enabled(wifi_on);

  if (wifi_on) {
    network_init();
    network_set_state_callback(gui_on_net_state);
    Serial.println("[Setup] Wi‑Fi preference: ON  -> use Wi‑Fi Settings to connect");
  } else {
#ifdef ARDUINO_ARCH_ESP32
    WiFi.mode(WIFI_OFF);  // master OFF: radio off (AP can re-enable later)
#endif
    Serial.println("[Setup] Wi‑Fi preference: OFF -> radio powered down");
  }

  // ---- Export system: mount SD & start Web UI (AP mode) -----------------
  hal::expander_wait_ready(800);

    // DEBUG: one-time CH422G read
  {
      esp_io_expander_handle_t h = hal::expander_get_handle();
      if (h) {
          uint32_t val = 0;
          if (esp_io_expander_get_level(h, 0xFF, &val) == ESP_OK) {
              Serial.printf("[DEBUG] CH422G initial IN=0x%02X\n", (unsigned)(val & 0xFF));
          } else {
              Serial.println("[DEBUG] CH422G read failed");
          }
      } else {
          Serial.println("[DEBUG] CH422G handle not ready");
      }
  }

  if (!chronos_sd_begin()) {
    Serial.println("[Export] SD init failed — export UI will still start, but listing will be empty.");
  }
  // Start AP + Web UI (AP+STA).
  chronos::apweb_begin("Chronos-AP", "chronos123");

  // ---- Splash (optional) ------------------------------------------------
#if SHOW_SPLASH
  gui_show_splash_embedded();
#else
  gui_show_main_menu();
#endif

  input_init();

  gBus.subscribe(&onBusEvent);

  // Attach hardware RTC (PCF85063A on SDA=8, SCL=9) via hooks
  init_pcf_hooks();
  delay(50);

  rtc_init();
  test_pcf_battery();

  struct tm t {};
  rtc_get_time(t);
  if ((t.tm_year + 1900) >= 2024) {
    bool os = true;
    if (pcf_rtc_get_os_flag(os) && os) {
      pcf_rtc_clear_os_flag();
      Serial.println("[PCF] Cleared OS/VL flag automatically.");
    }
  }

  gate_engine_init();
  experiments_init();

  gLastUserActivityMs = millis();
}

// [2026-01-25 13:30 CET] UPDATED (Robert + Copilot):
// - AP web now holds the screensaver via screensaver_set_apweb_hold().
// - We no longer force-hide saver from apweb_fs_busy() here to avoid CH422G races.
void loop() {
  // Keep the web server responsive first; AP web also drives the saver hold gate.
  chronos::apweb_loop();

  // ---- Normal application path ----
  // Drive Wi‑Fi portal FSM (scan/connect retries, etc.)
  network_process_portal();

  // Poll physical buttons, publish events, and dispatch them
  static Buttons gBtns;
  input_poll_and_publish(gBtns);
  gBus.dispatch();

  // ---- Deferred screensaver wake-up (safe LVGL context) ----
  if (gScreenSaverActive && gWakeFromSaverPending && !gWakeInProgress) {
    gWakeInProgress = true;
    screensaver_hide_async();  // run hide in LVGL async context
    gScreenSaverActive = false;
    gWakeFromSaverPending = false;
    gWakeInProgress = false;
    Serial.println("[Screensaver] Woke on user activity (deferred)");
  }

  // ---- Screensaver logic ----
  const unsigned long now = millis();
  const bool measuring        = gApp.sw.running();
  const bool experimentsArmed = gui_is_armed();
  const bool portalRunning    = network_portal_running();  // (your stub returns false in AP-only)
  const bool blockSaver       = measuring || experimentsArmed || portalRunning;

  if (blockSaver) {
    gLastUserActivityMs = now;
    if (gScreenSaverActive) {
      hal::backlight_on();
      gScreenSaverActive = false;
    }
  } else if (gScreenSaverTimeoutS > 0) {
    const unsigned long timeout_ms = (unsigned long)gScreenSaverTimeoutS * 1000UL;
    if (!gScreenSaverActive && (now - gLastUserActivityMs >= timeout_ms)) {
      screensaver_show();
      gScreenSaverActive = true;
    }
  }

  // LVGL heartbeat (animations, screen loads, timers)
  lv_timer_handler();
  delay(2);
}
