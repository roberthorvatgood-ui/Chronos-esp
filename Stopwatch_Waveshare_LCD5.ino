/*****
 * Chronos – Main Application (.ino)
 * Integrated SD Export + Web UI
 * [Updated: 2026-02-01] — added waveshare IO test header include
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
#include "src/drivers/expander_output_blink.h"

// Add the waveshare IO test header so waveshare_io_test() and waveshare_io_test_debug()
// are visible to this translation unit.
#include "src/include/waveshare_io_port.h"

// ── Export system (SD + Web UI) ────────────────────────────────────────────────
#include "src/export/export_fs.h"
#include "src/export/chronos_sd.h"
#include "src/export/web_export.h"

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
  // Explicitly pass the initial backlight state (no default in header)
  if (!hal::begin(false)) Serial.println("[HAL] begin() failed");
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
/*
    // --- Quick SW test: drive DO0/DO1 HIGH (temporary) ---
  {
  // Update these if your DO pins map to different EXIO indices
  const uint8_t DO0_EXIO = 0;
  const uint8_t DO1_EXIO = 1;

  Serial.println("[TEST] Attempting to drive DO0/DO1 HIGH (software test)");
  if (!hal::expander_wait_ready(2000)) {
    Serial.println("[TEST] Expander not ready; aborting DO set");
  } else {
    // Configure as outputs and drive HIGH
    hal::expander_pinMode(DO0_EXIO, true);
    hal::expander_pinMode(DO1_EXIO, true);
    hal::expander_digitalWrite(DO0_EXIO, true);
    hal::expander_digitalWrite(DO1_EXIO, true);
    Serial.println("[TEST] DO0/DO1 set HIGH. Measure terminal voltage now.");
  }
  } */
}

// [2026-01-25 13:30 CET] UPDATED (Robert + Copilot):
// - AP web now holds the screensaver via screensaver_set_apweb_hold().
// - We no longer force-hide saver from apweb_fs_busy() here to avoid CH422G races.
void loop() {
  // Check Serial for an interactive command to run the waveshare IO test.
  // Open Serial Monitor (115200) and send the line: io_test
  if (Serial.available()) {
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.equalsIgnoreCase("io_test")) {
    Serial.println("Starting waveshare_io_test() — press Gate A (EXIO0) and Gate B (EXIO5) now");
    waveshare_io_test(); // blocking; returns when test completes
    Serial.println("waveshare_io_test() finished — resuming normal operation");
  } else if (cmd.equalsIgnoreCase("io_debug")) {
    Serial.println("Starting waveshare_io_test_debug() for 60s");
    waveshare_io_test_debug();
    Serial.println("waveshare_io_test_debug() finished");
  } else if (cmd.startsWith("toggle_do")) {
    // Format: "toggle_do [period_ms] [cycles]"
    // Example: "toggle_do 3000 10" or "toggle_do 3000 0" (0 = run forever in task)
    // Or use "toggle_do_forever 3000"
    uint32_t period = 3000;
    uint32_t cycles = 10;
    // split args
    int firstSpace = cmd.indexOf(' ');
    if (firstSpace >= 0) {
      String rest = cmd.substring(firstSpace + 1);
      rest.trim();
      // try parse up to two numbers
      int sp = rest.indexOf(' ');
      if (sp >= 0) {
        period = (uint32_t)rest.substring(0, sp).toInt();
        cycles = (uint32_t)rest.substring(sp + 1).toInt();
      } else {
        period = (uint32_t)rest.toInt();
      }
    }
    // If user asked "toggle_do_forever ..." treat cycles=0
    if (cmd.startsWith("toggle_do_forever")) cycles = 0;

    Serial.printf("Starting toggle_do: period=%u ms cycles=%u (non-blocking task)\n", (unsigned)period, (unsigned)cycles);
    if (!expander_toggle_terminal_do01_start(period, cycles, /*exio_do0*/0, /*exio_do1*/1)) {
      Serial.println("[ERROR] Failed to start expander toggle task");
    }
  } else {
    // handle other commands as before...
  }
}

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