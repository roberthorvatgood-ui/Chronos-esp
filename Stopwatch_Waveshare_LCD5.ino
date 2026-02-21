/*****
 * Chronos – Main Application (.ino)
 * Integrated SD Export + Web UI + CH422G Pushbutton Gates
 * [Updated: 2026-02-08] Added I²C executor init and gate input pause/resume coordination
 * [Updated: 2026-02-11] Removed loop-level gate throttle for faster response
 * [Updated: 2026-02-18] Reintroduced splash screen; black screen during boot instead of white
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
#include "src/export/app_log.h"

// ── Splash screen on boot ────────────────────────────────────────────────────
#define SHOW_SPLASH 1       // set to 0 to skip the splash and go straight to main menu

namespace chronos { bool apweb_fs_busy(); }

EventBus gBus;
AppController gApp;

uint32_t gScreenSaverTimeoutS = 60;     // 0 disables screensaver
unsigned long gLastUserActivityMs = 0;  // last activity timestamp
bool gScreenSaverActive = false;        // current screensaver state
volatile bool gWakeFromSaverPending = false;
volatile bool gWakeInProgress = false;  // avoid double wake
volatile bool g_screensaver_hide_requested = false; 

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

// [2026-02-08] Main setup with I²C executor and CH422G pushbutton gate support
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  delay(50);
  Serial.println("\n=== CHRONOS BOOT ===");

  // ═══════════════════════════════════════════════════════════════════════
  // CRITICAL: Initialize I²C executor FIRST (before any I²C operations)
  // This creates a dedicated Core 0 task for serialized I²C access
  // ═══════════════════════════════════════════════════════════════════════
  if (!hal_i2c_executor_init(32)) {  // Queue size 32
    Serial.println("[FATAL] I²C executor init failed!");
    while(1) { delay(1000); }  // Halt system - cannot proceed without executor
  }
  Serial.println("[I²C] Executor initialized (queue=32, core=0)");

  // Localization
  i18n_init();
  i18n_load_saved();
  Serial.printf("[i18n] Boot language: %s\n", i18n_get_lang_code());
  
  // CRITICAL: Give executor task time to fully start
  delay(100);

  load_screensaver_timeout();

  // ═══════════════════════════════════════════════════════════════════════
  // HAL: panel, LCD, touch, backlight
  // Now uses I²C executor for CH422G operations
  // ═══════════════════════════════════════════════════════════════════════
  if (!hal::init())  Serial.println("[HAL] init() failed");
  if (!hal::begin()) Serial.println("[HAL] begin() failed");
  if (!hal::lvgl_init()) Serial.println("[HAL] lvgl_init() failed");

  // ═══════════════════════════════════════════════════════════════════════
  // Immediately paint a black screen so the user sees black (not white)
  // during the remaining setup.  This replaces the uninitialized white
  // framebuffer that became visible after recent setup got longer.
  // The splash or main menu will replace this screen later.
  // ═══════════════════════════════════════════════════════════════════════
  {
    lv_obj_t* blk = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(blk, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_scr_load(blk);
    lv_timer_handler();   // flush the black frame to the display NOW
  }

  // Wait for CH422G expander to be ready (with timeout)
  hal::expander_wait_ready(800);

  // ═══════════════════════════════════════════════════════════════════════
  // Network bring-up honoring saved General Settings
  // ═══════════════════════════════════════════════════════════════════════
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

  // ═══════════════════════════════════════════════════════════════════════
  // Export system: mount SD & start Web UI (AP mode)
  // SD operations now protected by I²C executor when accessing CH422G CS pin
  // ═══════════════════════════════════════════════════════════════════════
  bool sd_ok = chronos_sd_begin();
  if (!sd_ok) {
    Serial.println("[Export] SD init failed — export UI will still start, but listing will be empty.");
  }
  
  // Initialize logging system right after SD mount
  if (sd_ok) {
    applog_init();
  }
  
  // Log boot information
  LOG_I("BOOT", "Firmware built %s %s", __DATE__, __TIME__);
  LOG_I("BOOT", "Wi-Fi pref: %s", wifi_on ? "ON" : "OFF");
  LOG_I("BOOT", "SD: %s", sd_ok ? "OK" : "FAIL");
  LOG_I("BOOT", "Language: %s", i18n_get_lang_code());
  LOG_I("BOOT", "Screensaver timeout: %lu s", (unsigned long)gScreenSaverTimeoutS);
  
  // Start AP + Web UI (will trigger screensaver pause via apweb hold)
  chronos::apweb_begin("Chronos-AP", "chronos123");
  LOG_I("BOOT", "AP Web started");

  // ═══════════════════════════════════════════════════════════════════════
  // Splash screen or main menu
  // (replaces the temporary black screen loaded above)
  // ═══════════════════════════════════════════════════════════════════════
#if SHOW_SPLASH
  gui_show_splash_embedded();
#else
  gui_show_main_menu();
#endif

  // ═══════════════════════════════════════════════════════════════════════
  // Configure CH422G pushbuttons as gate inputs
  // Map pushbuttons: IO0 -> Gate A, IO5 -> Gate B (active_low = true)
  // Now reads via I²C executor for thread-safe access
  // ═══════════════════════════════════════════════════════════════════════
  Serial.println("[Setup] Configuring CH422G pushbuttons for gate inputs...");
  input_configure_pushbuttons(0, 5, true);

  // Initialize input system (will set CH422G to input mode and configure debounce)
  input_init();
  
  LOG_I("BOOT", "Gate inputs configured");

  // Subscribe to event bus
  gBus.subscribe(&onBusEvent);

  // ═══════════════════════════════════════════════════════════════════════
  // Attach hardware RTC (PCF85063A on SDA=8, SCL=9)
  // Now uses I²C executor for all RTC read/write operations
  // ═══════════════════════════════════════════════════════════════════════
  init_pcf_hooks();
  delay(50);

  rtc_init();
  test_pcf_battery();

  // Auto-clear OS/VL flag if RTC time is valid
  struct tm t {};
  rtc_get_time(t);
  if ((t.tm_year + 1900) >= 2024) {
    LOG_I("RTC", "Time: %d-%02d-%02d %02d:%02d:%02d", 
          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, 
          t.tm_hour, t.tm_min, t.tm_sec);
    bool os = true;
    if (pcf_rtc_get_os_flag(os) && os) {
      pcf_rtc_clear_os_flag();
      Serial.println("[PCF] Cleared OS/VL flag automatically.");
    }
  }

  // ═══════════════════════════════════════════════════════════════════════
  // Initialize gate engine and experiments
  // ═══════════════════════════════════════════════════════════════════════
  gate_engine_init();
  experiments_init();

  gLastUserActivityMs = millis();
  
  // Flush all buffered boot log lines to SD in a single write
  applog_boot_complete();

  Serial.println("=== CHRONOS READY ===\n");
  LOG_I("BOOT", "CHRONOS READY (heap: %u)", ESP.getFreeHeap());
}

// [2026-02-11] Main loop — no loop-level gate throttle; input.cpp handles its own 5ms period
void loop() {
  // ═══════════════════════════════════════════════════════════════════════
  // Web server and network processing
  // ═══════════════════════════════════════════════════════════════════════
  chronos::apweb_loop();
  network_process_portal();

  // ═══════════════════════════════════════════════════════════════════════
  // Gate input polling — called every loop iteration.
  // input_poll_and_publish() has its own internal 5ms throttle so calling
  // it more often than that is a harmless no-op.  Removing the loop-level
  // 10ms gate reduces worst-case latency from 15ms to 5ms.
  // ═══════════════════════════════════════════════════════════════════════
  if (!input_is_paused()) {
    static Buttons gBtns;
    input_poll_and_publish(gBtns);
  }
  
  // ═══════════════════════════════════════════════════════════════════════
  // Event bus and experiment state
  // ═══════════════════════════════════════════════════════════════════════
  gBus.dispatch();
  gui_poll_real_gate_experiments();

  // ═══════════════════════════════════════════════════════════════════════
  // Screensaver wake handling
  // ═══════════════════════════════════════════════════════════════════════
  if (gScreenSaverActive && gWakeFromSaverPending && !gWakeInProgress) {
    gWakeInProgress = true;
    
    Serial.println("[Screensaver] Wake requested");
    
    // 1. Hide screensaver overlay
    screensaver_hide();
    
    // 2. Turn on backlight
    hal::backlight_on();
    delay(50);  // brief settle — was 600ms total
    
    // 3. Resume gate polling
    input_resume();
    
    gScreenSaverActive = false;
    gWakeFromSaverPending = false;
    gWakeInProgress = false;
    gLastUserActivityMs = millis();  // reset timeout
    
    Serial.println("[Screensaver] Wake complete");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // Screensaver timeout logic
  // Automatically pauses gate polling to reduce I²C bus load
  // ═══════════════════════════════════════════════════════════════════════
  const unsigned long now = millis();
  const bool measuring        = gApp.sw.running();
  const bool experimentsArmed = gui_is_armed();
  const bool portalRunning    = network_portal_running();
  const bool blockSaver       = measuring || experimentsArmed || portalRunning;

  if (blockSaver) {
    // Activity detected: reset timer and ensure screen is on
    gLastUserActivityMs = now;
    if (gScreenSaverActive) {
      screensaver_hide();
      hal::backlight_on();
      gScreenSaverActive = false;
      Serial.println("[Loop] Screensaver hidden (activity detected)");
    }
  } else if (gScreenSaverTimeoutS > 0) {
    // Check for timeout
    const unsigned long timeout_ms = (unsigned long)gScreenSaverTimeoutS * 1000UL;
    if (!gScreenSaverActive && (now - gLastUserActivityMs >= timeout_ms)) {
      Serial.println("[Loop] Screensaver timeout - pausing gates");
      LOG_I("SAVER", "Timeout — entering screensaver");
      
      // Pause gates FIRST
      input_pause();
      
      // Wait for any in-flight I²C operations to complete
      delay(100);
      
      // Now show screensaver
      screensaver_show();
      gScreenSaverActive = true;
    }
  }

  // ═══════════════════════════════════════════════════════════════════════
  // LVGL task handler (must be called regularly for UI updates)
  // ═══════════════════════════════════════════════════════════════════════
  lv_timer_handler();
  
  // Minimal delay — input.cpp throttles its own I²C reads at 5ms intervals,
  // so the loop can spin faster without extra bus pressure.
  delay(2);
}