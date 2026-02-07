/*****
 * Chronos – Main Application (.ino)
 * Integrated SD Export + Web UI + CH422G Pushbutton Gates
 * [Updated: 2026-02-07] Production build with working CH422G input reading
 *****/

#include <Arduino.h>
#include <Preferences.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif

#include "src/drivers/hal_panel.h"
#include "src/drivers/hal_i2c_executor.h" // hal_i2c_exec_sync
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

// Export system (SD + Web UI)
#include "src/export/export_fs.h"
#include "src/export/chronos_sd.h"
#include "src/export/web_export.h"

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

// [2026-02-07] Main setup with CH422G pushbutton gate support
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  delay(50);
  Serial.println("\n=== CHRONOS BOOT ===");

  // Localization
  i18n_init();
  i18n_load_saved();
  Serial.printf("[i18n] Boot language: %s\n", i18n_get_lang_code());
  
  load_screensaver_timeout();

  // HAL: panel, LCD, touch, backlight
  if (!hal::init())  Serial.println("[HAL] init() failed");
  if (!hal::begin()) Serial.println("[HAL] begin() failed");
  if (!hal::lvgl_init()) Serial.println("[HAL] lvgl_init() failed");

  // Start the central I2C executor (runs on core 0, required for thread-safe I²C ops)
  if (!hal_i2c_executor_init(16)) {
    Serial.println("[HAL] I2C executor init failed");
  }

  // Wait for CH422G expander to be ready
  hal::expander_wait_ready(800);

  // --- Network bring-up honoring saved General Settings ---
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
  if (!chronos_sd_begin()) {
    Serial.println("[Export] SD init failed — export UI will still start, but listing will be empty.");
  }
  
  // Start AP + Web UI
  chronos::apweb_begin("Chronos-AP", "chronos123");

  // ---- Splash screen (optional) ----------------------------------------
#if SHOW_SPLASH
  gui_show_splash_embedded();
#else
  gui_show_main_menu();
#endif

  // ---- Configure CH422G pushbuttons as gate inputs ---------------------
  // Map pushbuttons: IO0 -> Gate A, IO5 -> Gate B (active_low = true)
  Serial.println("[Setup] Configuring CH422G pushbuttons for gate inputs...");
  input_configure_pushbuttons(0, 5, true);

  // Initialize input system (will set CH422G to input mode and configure debounce)
  input_init();

  // Subscribe to event bus
  gBus.subscribe(&onBusEvent);

  // ---- Attach hardware RTC (PCF85063A on SDA=8, SCL=9) -----------------
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

  // ---- Initialize gate engine and experiments --------------------------
  gate_engine_init();
  experiments_init();

  gLastUserActivityMs = millis();
  
  Serial.println("=== CHRONOS READY ===\n");
}

// [2026-02-07] Main loop with CH422G input polling
void loop() {
  // Keep the web server responsive
  chronos::apweb_loop();

  // Drive Wi‑Fi portal FSM (scan/connect retries, etc.)
  network_process_portal();

  // Poll CH422G pushbutton inputs and publish gate events
  static Buttons gBtns;
  input_poll_and_publish(gBtns);
  
  // Dispatch all queued events to app controller
  gBus.dispatch();

  // ---- Screensaver wake handling (deferred to avoid I²C contention) ----
  if (gScreenSaverActive && gWakeFromSaverPending && !gWakeInProgress) {
    gWakeInProgress = true;
    screensaver_hide_async();
    gScreenSaverActive = false;
    gWakeFromSaverPending = false;
    gWakeInProgress = false;
    Serial.println("[Screensaver] Woke on user activity (deferred)");
  }

  // ---- Screensaver timeout logic ---------------------------------------
  const unsigned long now = millis();
  const bool measuring        = gApp.sw.running();
  const bool experimentsArmed = gui_is_armed();
  const bool portalRunning    = network_portal_running();
  const bool blockSaver       = measuring || experimentsArmed || portalRunning;

  if (blockSaver) {
    // Activity detected: reset timer and ensure screen is on
    gLastUserActivityMs = now;
    if (gScreenSaverActive) {
      hal::backlight_on();
      gScreenSaverActive = false;
    }
  } else if (gScreenSaverTimeoutS > 0) {
    // Check for timeout
    const unsigned long timeout_ms = (unsigned long)gScreenSaverTimeoutS * 1000UL;
    if (!gScreenSaverActive && (now - gLastUserActivityMs >= timeout_ms)) {
      screensaver_show();
      gScreenSaverActive = true;
    }
  }

  // ---- LVGL task handler (must be called regularly) --------------------
  lv_timer_handler();
  
  delay(2);
}