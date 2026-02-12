/*******************************************************
 * gui.cpp
 * Chronos – LVGL GUI for Waveshare ESP32-S3 Touch LCD 5"
 * [2026-01-18 16:30 CET] UPDATED
 *
 * Purpose:
 *  - Renders all screens (Main Menu, Stopwatch, CV, Photogate, UA,
 *    Free Fall, Inclined Plane, Tachometer, Settings, Date&Time)
 *  - Manages header, timers, keyboard, and experiment history panes
 *
 * Key features:
 *  - Unified header net icon (Wi‑Fi) with tiny “AP” badge (Option D2/A)
 *  - Color-coded internet status (green=OK, gray=No Internet, dark=Wi‑Fi OFF)
 *  - Settings row toggle for “WiFi Export (AP)”
 *  - CSV export via exportfs_save_csv(), appears in web UI automatically
 *
 * Notes:
 *  - Uses LVGL 8 API and custom fonts
 *  - Relies on app_network portal FSM for STA status updates
 *******************************************************/

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <WiFi.h>
#include "../intl/i18n.h"
#include "gui.h"
#include "../net/app_network.h"
#include "../core/config.h" // SCREEN_WIDTH, SCREEN_HEIGHT
#include "../core/event_bus.h"
#include "../core/app_controller.h"
#include "../drivers/hal_panel.h"
#include "../core/gate_engine.h"
#include "../experiments/experiments.h"
#include <string.h>
#include <vector>
#include <string>
#include "src/gui/screensaver.h"
#include "../core/rtc_manager.h"
#include "src/export/chronos_sd.h"
#include "src/export/export_fs.h"
#include "src/export/web_export.h"
#include <extra/libs/qrcode/lv_qrcode.h>

// ── Globals ───────────────────────────────────────────────────────────────────
static lv_obj_t* g_header          = nullptr;
static lv_obj_t* g_header_title    = nullptr;
static lv_obj_t* g_hdr_settings    = nullptr;
static lv_obj_t* g_val_label       = nullptr;
static lv_obj_t* g_formula_label   = nullptr;
static lv_obj_t* g_history_panel   = nullptr;
static bool      g_wifi_enabled    = false;
static bool      g_sim_enabled     = true;
static bool      g_armed           = false;
static lv_obj_t* g_hdr_clock = nullptr;
static lv_timer_t* g_clock_timer = nullptr;

// Date-Time presets (used only once after manual save)
static bool g_dt_presets_valid = false;
static int  g_dt_preset_y=0, g_dt_preset_m=0, g_dt_preset_d=0;
static int  g_dt_preset_h=0, g_dt_preset_min=0, g_dt_preset_s=0;

// [2026-01-18 CET] NEW: Unified net icon + tiny "AP" badge + AP state
static lv_obj_t* g_hdr_net_icon = nullptr;   // single Wi‑Fi icon (color coded)
static lv_obj_t* g_hdr_ap_badge = nullptr;   // tiny "AP" text badge placed near icon

static bool g_ap_running = false; // true when AP export is active
static lv_obj_t* g_ap_modal = nullptr; // AP export instruction overlay
static lv_obj_t* cv_btn_arm = nullptr;

// External references
extern EventBus gBus;
extern AppController gApp;
extern uint64_t gui_get_stopwatch_us();
extern uint32_t gScreenSaverTimeoutS;
extern unsigned long gLastUserActivityMs;
extern bool gScreenSaverActive;
extern volatile bool gWakeFromSaverPending;  // NEW: deferred wake flag
extern void experiments_emit_csv(Print& out);


// Global references to simulation buttons for animation
static lv_obj_t* g_sim_btn_a = nullptr;
static lv_obj_t* g_sim_btn_b = nullptr;
// Flag: only update buttons when on experiment screen
static bool g_experiment_screen_active = false;
// Flag to trigger history update from LVGL timer (safe context)
static bool g_history_update_pending = false;
static const char* g_pending_history_mode = nullptr;
static lv_timer_t* g_history_update_timer = nullptr;

// Screen transition protection
volatile bool g_screen_transition_active = false;

// Clear simulation button pointers (call when leaving experiment screens)
static void clear_sim_button_refs() {
  g_sim_btn_a = nullptr;
  g_sim_btn_b = nullptr;
}

// ── Embedded splash asset (welcome_screen.c/.h) ──
// Try common relative paths; error out if not found.
#ifdef __has_include
 #if __has_include("../assets/welcome_screen.h")
  #include "../assets/welcome_screen.h"
 #elif __has_include("assets/welcome_screen.h")
  #include "assets/welcome_screen.h"
 #else
  #error "welcome_screen.h not found. Place it in src/assets/ and include here."
 #endif
#else
 #include "../assets/welcome_screen.h"
#endif

// ── Current screen tracking ───────────────────────────────────────────────────
enum class CurrentScreen : uint8_t {
  None = 0,
  MainMenu,
  Stopwatch,
  CV,
  Photogate,
  UA,
  FreeFall,
  Incline,
  Tachometer,
  Settings,
  WifiSettings,
  CVSettings,
  PGSettings,
  UASettings,
  FFSettings,
  INSettings,
  TASettings,
  SWSettings, // Stopwatch settings
  DTSettings
};
static CurrentScreen g_current_screen = CurrentScreen::None;


// Stopwatch gate modes:
// - None: Manual mode (no gate control)
// - GateA: Gate A toggles start/stop, Gate B records laps
// - GateAB: Gate A starts (ignores if already running), Gate B stops (ignores if not running)
enum class SwGateMode : uint8_t { None = 0, GateA = 1, GateAB = 2 };
static SwGateMode g_sw_mode = SwGateMode::None;

// Lap event types for stopwatch history:
// - Start: Stopwatch started
// - Stop: Stopwatch stopped (final time recorded)
// - Lap: Intermediate time recorded while running
enum class LapEvent : uint8_t { Start=0, Stop=1, Lap=2 };


// Forward declaration
void gui_refresh_active_screen();

// [2026-01-18 19:50 CET] UPDATED: Forward declaration for gui_set_net_icon_color.
static void gui_set_net_icon_color(bool wifi_enabled, bool internet_ok);

// [2026-01-18 19:50 CET] UPDATED: Forward declaration for set_arm_button_visual.
static void set_arm_button_visual(lv_obj_t* btn_arm, bool armed,
                                  const char* armed_text = "Armed",
                                  const char* disarmed_text = "Start / Arm");

// [2026-01-18 16:15 CET] UPDATED: forward declarations (ONE block only)
static void on_ta_focus(lv_event_t* e);
static void on_ta_defocus(lv_event_t* e);
static void on_scr_click_hide_kb(lv_event_t* e);
static void on_ta_value_changed(lv_event_t* e); // NEW
static void on_back(lv_event_t* e);
static void on_settings(lv_event_t* e);

// Stopwatch
static void sw_clear_history();
static void sw_update_cb(lv_timer_t* t);
static void sw_startstop_cb(lv_event_t* e);
static void sw_reset_cb(lv_event_t* e);
static void sw_lap_cb(lv_event_t* e);
static void sw_export_csv_cb(lv_event_t* e);
static void sw_settings_cb(lv_event_t* e);

// Stopwatch helper functions forward declarations
static inline void fmt_time_ms(char* out, size_t n, uint64_t us_total);
static void sw_record_event(LapEvent e);
static void update_lap_history();

// CV
static void cv_arm_toggle_cb(lv_event_t* e);
static void cv_reset_cb(lv_event_t* e);
static void cv_export_cb(lv_event_t* e);
static void cv_settings_cb(lv_event_t* e);
static void sim_cv_gate_a(lv_event_t* e);
static void sim_cv_gate_b(lv_event_t* e);

// Photogate
static void pg_arm_toggle_cb(lv_event_t* e);
static void pg_reset_cb(lv_event_t* e);
static void pg_export_cb(lv_event_t* e);
static void pg_settings_cb(lv_event_t* e);


// ── Fonts ─────────────────────────────────────────────────────────────────────
#include "src/assets/fonts/ui_font_84.h"
#include "src/assets/fonts/ui_font_48.h"
#include "src/assets/fonts/ui_font_32.h"
#include "src/assets/fonts/ui_font_28.h"
#include "src/assets/fonts/ui_font_16.h"

#define FONT_STOPWATCH  (&ui_font_84)
#define FONT_TITLE  (&ui_font_32)
#define FONT_LABEL  (&ui_font_28)
#define FONT_BIG    (&ui_font_48)
#define FONT_SMALL  (&ui_font_16)
#define FONT_FOOTER (&ui_font_16)



// Helper to stop AP export when switch goes OFF
static void ap_export_stop(void)
{
    chronos::apweb_end();
    g_ap_running = false;

    if (g_hdr_ap_badge) {
        lv_obj_add_flag(g_hdr_ap_badge, LV_OBJ_FLAG_HIDDEN);
    }

    screensaver_set_apweb_hold(false);

    if (g_ap_modal) {
        lv_obj_del(g_ap_modal);
        g_ap_modal = nullptr;
    }
}

// Safe screen transition wrapper
static void safe_transition(void (*show_screen_fn)()) {
  // 1. Set transition flag to block I²C polling
  g_screen_transition_active = true;
  
  // 2. Disarm experiment
  g_armed = false;
  experiment_set_state(ExperimentState::IDLE);
  
  // 3. Give I²C subsystems time to finish current operation
  delay(50);
  
  // 4. Show new screen
  show_screen_fn();
  
  // 5. Small delay for LVGL to settle
  delay(50);
  
  // 6. Clear transition flag
  g_screen_transition_active = false;
}

// [2026-01-26 21:22 CET] NEW: AP modal close handler (CLOSE AP button)

// [2026-01-26 21:28 CET] NEW: CLOSE AP handler (button in AP modal)
static void ap_export_close_btn_cb(lv_event_t* /*e*/)
{
    // Stop AP + hide badge + remove modal + release screensaver hold
    ap_export_stop();
    // Return to Settings; switch reflects OFF
    gui_show_settings();
}

// Animate simulation buttons based on real gate state
void gui_set_sim_button_state(int gate_index, bool active) {
  lv_obj_t* btn = nullptr;
  
  if (gate_index == 0) btn = g_sim_btn_a;
  else if (gate_index == 1) btn = g_sim_btn_b;
  
  if (!btn) return;
  
  if (active) {
    // Gate is BLOCKED (active) - show green
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_100, 0);
  } else {
    // Gate is UNBLOCKED - restore default gray (same as CLR_BTN)
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x30324A), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
  }
}

// [2026-01-26 21:28 CET] FIX: AP modal — ALL texts use tr(); body uses key "AP modal instructions"
static void on_sw_ap_changed(lv_event_t* e)
{
    lv_obj_t* sw     = (lv_obj_t*)lv_event_get_target(e);
    const bool ap_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (ap_on) {
        chronos::apweb_begin("Chronos-AP", "chronos123");
        g_ap_running = true;

        if (g_hdr_ap_badge) lv_obj_clear_flag(g_hdr_ap_badge, LV_OBJ_FLAG_HIDDEN);
        screensaver_set_apweb_hold(true);

        if (!g_ap_modal) {
            g_ap_modal = lv_obj_create(lv_scr_act());
            lv_obj_remove_style_all(g_ap_modal);
            lv_obj_set_size(g_ap_modal, SCREEN_WIDTH, SCREEN_HEIGHT);
            lv_obj_set_style_bg_color(g_ap_modal, lv_color_make(0, 60, 160), 0);
            lv_obj_set_style_bg_opa(g_ap_modal, LV_OPA_COVER, 0);
            lv_obj_add_flag(g_ap_modal, LV_OBJ_FLAG_CLICKABLE);

            // Card
            const int card_w = SCREEN_WIDTH  - 80;
            const int card_h = SCREEN_HEIGHT - 140;
            lv_obj_t* card = lv_obj_create(g_ap_modal);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, card_w, card_h);
            lv_obj_set_style_bg_color(card, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(card, 16, 0);
            lv_obj_set_style_pad_all(card, 24, 0);
            lv_obj_set_style_shadow_width(card, 12, 0);
            lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
            lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
            lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

            // Top content container: text (left) + QR (right)
            const int top_h = card_h - 80;
            lv_obj_t* top = lv_obj_create(card);
            lv_obj_remove_style_all(top);
            lv_obj_set_size(top, LV_PCT(100), top_h);
            lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_column(top, 24, 0);
            lv_obj_set_style_pad_row(top, 0, 0);
            lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

            // LEFT: instructions (entire body comes from a single key)
            lv_obj_t* text_col = lv_obj_create(top);
            lv_obj_remove_style_all(text_col);
            lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(text_col, LV_PCT(55), LV_PCT(100));

            lv_obj_t* lbl = lv_label_create(text_col);
            lv_label_set_text(lbl, tr("AP modal instructions"));  // <—— your key
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            lv_obj_set_style_text_font(lbl, &ui_font_16, 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, LV_PCT(100));

            // RIGHT: QR column
            lv_obj_t* qr_col = lv_obj_create(top);
            lv_obj_remove_style_all(qr_col);
            lv_obj_set_style_bg_opa(qr_col, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(qr_col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(qr_col, LV_PCT(45), LV_PCT(100));

            const char* ap_url = "http://192.168.4.1";
            const int QR_SIZE  = 180;
        #if LV_USE_QRCODE
            lv_obj_t* qr = lv_qrcode_create(qr_col, QR_SIZE, lv_color_black(), lv_color_white());
            lv_qrcode_update(qr, ap_url, strlen(ap_url));
            lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 8);
        #else
            lv_obj_t* url_lbl = lv_label_create(qr_col);
            lv_label_set_text(url_lbl, ap_url); // URL literal (not localized)
            lv_obj_set_style_text_color(url_lbl, lv_color_black(), 0);
            lv_obj_set_style_text_font(url_lbl, &ui_font_16, 0);
            lv_obj_align(url_lbl, LV_ALIGN_TOP_MID, 0, 8);
        #endif
            lv_obj_t* qr_help = lv_label_create(qr_col);
            lv_label_set_text(qr_help, tr("Scan with your phone"));
            lv_obj_set_style_text_color(qr_help, lv_color_black(), 0);
            lv_obj_set_style_text_font(qr_help, &ui_font_16, 0);
            lv_obj_align(qr_help, LV_ALIGN_BOTTOM_MID, 0, -4);

            // CLOSE AP button
            lv_obj_t* btn = lv_btn_create(card);
            lv_obj_set_size(btn, 220, 56);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -12);
            lv_obj_set_style_radius(btn, 10, 0);

            lv_obj_t* bl = lv_label_create(btn);
            lv_label_set_text(bl, tr("CLOSE AP"));
            lv_obj_center(bl);

            lv_obj_add_event_cb(btn, ap_export_close_btn_cb, LV_EVENT_CLICKED, NULL);
        }
    } else {
        // OFF → stop AP + modal
        ap_export_stop();
    }

    // Recolor unified Wi‑Fi icon (AP runs without Internet)
    gui_set_net_icon_color(/*wifi_enabled*/ g_ap_running || g_wifi_enabled,
                           /*internet_ok*/  false);
}

static void on_sw_sim_changed(lv_event_t* e);

// Date & Time
static void on_sw_autosync_changed(lv_event_t* e);
static void on_kb_ready_dt(lv_event_t* e);
static void on_dt_save_click(lv_event_t* e);

// Screensaver
static void on_kb_ready_ss(lv_event_t* e);


// Header helpers
static void ClockTickCbShim(lv_timer_t*); // optional shim if needed

// ── Screensaver helpers (definitions) ─────────────────────────────────────────

void gui_note_user_activity()
{
    gLastUserActivityMs = millis();
    if (gScreenSaverActive) {
        // Defer LVGL work (scr_load/del) to main loop to avoid reentrancy crashes
        gWakeFromSaverPending = true;
        // no direct LVGL calls here
    }
}

// [2026-01-18 16:15 CET] UPDATED: Start AP-export (AP+STA). Reveal tiny "AP" badge.
// [2026-01-18 CET] UPDATED: Start Export AP (AP+STA safe) and show tiny "AP" badge
static void gui_ap_start()
{
  // apweb_begin moved to on_sw_ap_changed (AP modal)  // overload also available with no args
  g_ap_running = true;

  if (g_hdr_ap_badge) {
    lv_obj_clear_flag(g_hdr_ap_badge, LV_OBJ_FLAG_HIDDEN);
  }
  Serial.println("[GUI] AP Export started (badge visible)");
}

// [2026-01-18 CET] UPDATED: Stop Export AP and hide tiny "AP" badge
static void gui_ap_stop()
{
  chronos::apweb_end();   // stops HTTP + AP, leaves STA intact (Option A ready)
  g_ap_running = false;

  if (g_hdr_ap_badge) {
    lv_obj_add_flag(g_hdr_ap_badge, LV_OBJ_FLAG_HIDDEN);
  }
  Serial.println("[GUI] AP Export stopped (badge hidden)");
}

void gui_show_datetime_settings();

// ── Layout & Colors ───────────────────────────────────────────────────────────
static const int HEADER_H  = 72;
static const int SIMROW_H  = 54;
static const int FOOTER_H  = 80;
static const int HISTORY_W = 220;
static inline lv_color_t CLR_BG()      { return lv_color_make(0, 60, 160); }
static inline lv_color_t CLR_HDR()     { return lv_color_make(0, 40, 120); }
static inline lv_color_t CLR_BTN()     { return lv_color_hex(0x30324A); }
static inline lv_color_t CLR_WIFI_OK() { return lv_color_hex(0x00E07A); }
static inline lv_color_t CLR_WIFI_DIM(){ return lv_color_hex(0x8080A0); }
static inline lv_color_t CLR_ALERT()   { return lv_color_hex(0xFF6060); }
static inline lv_color_t CLR_ARMED()   { return lv_color_hex(0x2EBF6D); }
static inline lv_color_t CLR_FOOTER()  { return lv_color_hex(0x23263A); }

// Settings layout
static const int MARGIN_LEFT   = 20;
static const int MARGIN_RIGHT  = 20;
static const int CAPTION_Y0    = 20;
static const int BLOCK_SPACING = 80;
static const int FIELD_W       = 260;
static const int FIELD_H       = 60;


// Helper: Hide keyboard when clicking outside textarea
static void on_scr_click_hide_kb(lv_event_t* e)
{
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
  if (kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }
}

// Helper: Hide keyboard when textarea loses focus
static void on_ta_defocus(lv_event_t* e)
{
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
  if (kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }
}


// [2026-01-18 15:45 CET] NEW: Set unified Wi-Fi icon color by Internet status
//  - WiFi OFF      -> dark gray  (#404060)
//  - WiFi ON, no Internet -> gray       (#8080A0)
//  - WiFi ON, Internet OK -> green      (#00E07A)
//  - AP badge visibility handled elsewhere (g_ap_running)

// [2026-01-18 16:15 CET] NEW: Set unified Wi-Fi icon color by Internet status
// [2026-01-18 CET] NEW: Color the unified Wi‑Fi icon by connectivity
//  - Wi‑Fi OFF            -> dark gray (#404060)
//  - Wi‑Fi ON, no Internet-> gray      (#8080A0)
//  - Wi‑Fi ON, Internet OK-> green     (#00E07A)
static void gui_set_net_icon_color(bool wifi_enabled, bool internet_ok)
{
  if (!g_hdr_net_icon) return;
  lv_color_t c = lv_color_hex(0x404060);          // OFF
  if (wifi_enabled) c = internet_ok ? lv_color_hex(0x00E07A) : lv_color_hex(0x8080A0);
  lv_obj_set_style_text_color(g_hdr_net_icon, c, 0);
}


// ── Header clock tick — now with seconds ─────────────────────────────────────

static void clock_tick_cb(lv_timer_t*)
{
    if (!g_hdr_clock) return;

    struct tm now{};
    rtc_get_time(now);
    if ((now.tm_year + 1900) < 2000) {
        lv_label_set_text(g_hdr_clock, "--:--:--");
        return;
    }

    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    lv_label_set_text(g_hdr_clock, buf);
}


// [2026-01-18 19:30 CET] NEW: CV arm toggle handler (replaces stray global-scope fragment)
static void cv_arm_toggle_cb(lv_event_t* e)
{
    (void)e;
    g_armed = !g_armed;
    
    // NEW: Set experiment state
    if (g_armed) {
      experiment_set_state(ExperimentState::ARMED);
    } else {
      experiment_set_state(ExperimentState::IDLE);
    }

    set_arm_button_visual(cv_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
    experiments_clear_timestamps();   // clear timing markers; keep last result on screen
}

// ─────────────────────────────────────────────────────────────────────────────
// Stopwatch — Mode and State (moved here to be accessible from gui_poll_real_gate_experiments)
// ─────────────────────────────────────────────────────────────────────────────


// Stopwatch vars (declare ONCE)
static lv_obj_t*   sw_time_lbl = nullptr;
static lv_timer_t* sw_timer    = nullptr;
static char        sw_last_text[24] = "";

// ── Splash state ──────────────────────────────────────────────────────────────
static lv_timer_t* g_splash_timer = nullptr;
static lv_obj_t*   g_splash_scr   = nullptr;
static bool        g_splash_active= false;

// Make any container visually "flat": no border/outline/shadow
static inline void apply_no_borders(lv_obj_t* obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_opa        (obj, LV_OPA_TRANSP, 0);  // keep transparent bg for rows/containers
    lv_obj_set_style_border_width  (obj, 0, 0);
    lv_obj_set_style_border_opa    (obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width (obj, 0, 0);
    lv_obj_set_style_outline_opa   (obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width  (obj, 0, 0);
    lv_obj_set_style_shadow_opa    (obj, LV_OPA_TRANSP, 0);
}

// ─────────────────────────────────────────────────────────────
// Flex helpers for Settings page
// ─────────────────────────────────────────────────────────────


static lv_obj_t* make_row(lv_obj_t* parent, int h = 60)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width (row, LV_PCT(100));
    lv_obj_set_height(row, h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Flex row: label left, controls right; vertically centered
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_BETWEEN,  // main (x)
        LV_FLEX_ALIGN_CENTER,         // cross (y)
        LV_FLEX_ALIGN_CENTER);        // track

    // Respect your margins and spacing
    lv_obj_set_style_pad_left  (row, MARGIN_LEFT,  0);
    lv_obj_set_style_pad_right (row, MARGIN_RIGHT, 0);
    lv_obj_set_style_pad_row   (row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);

    // No hairlines/borders on rows
    apply_no_borders(row);
    return row;
}

static lv_obj_t* make_spacer(lv_obj_t* parent)
{
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_set_size(s, 0, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    // Also borderless
    apply_no_borders(s);
    return s;
}

// Cancel splash timer and clear state (callable from anywhere in gui.cpp)
static inline void splash_cancel_if_running()
{
  if (g_splash_timer) {
    lv_timer_del(g_splash_timer);
    g_splash_timer = nullptr;
  }
  g_splash_active = false;
  g_splash_scr = nullptr;
}

// Settings textareas (other experiments)
static lv_obj_t* g_cv_ta_dist = nullptr;
static lv_obj_t* g_pg_ta_flag = nullptr;
static lv_obj_t* g_ua_ta0     = nullptr; // UA length (only field after Step‑2)
static lv_obj_t* g_ua_ta1     = nullptr; // reused by FF/Incline settings
static lv_obj_t* g_ua_ta2     = nullptr; // reused by FF/Incline settings


// ─────────────────────────────────────────────────────────────
// Edit-session state (keep near your existing globals)
// ─────────────────────────────────────────────────────────────
static lv_obj_t* s_edit_ta = nullptr;
static bool      s_edit_made = false;
static char      s_old_value[64] = {0};

// NEW: first-key replacement one-shot
static bool      s_replace_on_first_key = false;

// Forward-declare/define once, above all uses:
static inline void end_edit_session() {
    s_edit_ta = nullptr;
    s_edit_made = false;
    s_old_value[0] = '\0';
    s_replace_on_first_key = false;
}

// The VALUE_CHANGED handler you already have:
static void on_ta_value_changed(lv_event_t* e)
{
    if (lv_event_get_target(e) == s_edit_ta) {
        s_edit_made = true;
    }
}

// Fires before LVGL inserts typed text; clears once so first key replaces old value
static void on_ta_first_insert(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_INSERT) return;
    lv_obj_t* ta = lv_event_get_target(e);
    if (ta == s_edit_ta && s_replace_on_first_key && !s_edit_made) {
        lv_textarea_set_text(ta, "");     // clear once
        s_replace_on_first_key = false;   // one-shot
        // LVGL will now insert the user's key into an empty field
    }
}

// ── Splash screen (welcome) ───────────────────────────────────────────────────
static void splash_to_main_cb(lv_timer_t* /*t*/)
{
  if (!g_splash_active) return;
  if (g_splash_scr && lv_scr_act() != g_splash_scr) {
    splash_cancel_if_running();
    return;
  }
  splash_cancel_if_running();
  gui_show_main_menu();
}

// Tap anywhere to skip early
static void splash_skip_cb(lv_event_t* /*e*/)
{
  splash_cancel_if_running();
  gui_show_main_menu();
}

/**
 * Show the embedded welcome image for ~6 seconds (no subtitle).
 * Safe against late timer: if you leave the splash meanwhile (tap or navigate),
 * the timer callback becomes a no‑op.
 */
void gui_show_splash_embedded(void)
{
  // Create splash screen
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
     
    // ⬇️ Make screen non-scrollable
  lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  // Centered embedded image
  lv_obj_t* img = lv_img_create(scr);
  lv_img_set_src(img, &welcome_screen);
  lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

  // Tap to skip the splash
  lv_obj_add_event_cb(scr, splash_skip_cb, LV_EVENT_CLICKED, nullptr);

  // Wake backlight so the splash is visible
  hal::backlight_on();
  
  lv_timer_create([](lv_timer_t*){
      hal::backlight_on();         // forced ON (safe)
  }, 1200, NULL);   // delay 1.2s after splash load


  // Mark active and remember screen pointer (used to ignore late timer)
  g_splash_active = true;
  g_splash_scr = scr;

  // Auto‑advance after 6 seconds (6000 ms)
  g_splash_timer = lv_timer_create(splash_to_main_cb, 6000, nullptr);

  // Show splash with fade
  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);
}

// Optional backward‑compat wrapper
void gui_show_splash_embedded(const char* /*unused_subtitle*/)
{
  gui_show_splash_embedded();
}

// ── Helpers: use previous value if no input was made ──────────────────────────
static const char* get_effective_ta_text_for_ready(lv_obj_t* kb, lv_obj_t* ta)
{
  if (kb &&
      lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_NUMBER &&
      ta == s_edit_ta && !s_edit_made)
  {
    return s_old_value;
  }
  return lv_textarea_get_text(ta);
}
static const char* get_effective_ta_text_for_save(lv_obj_t* ta)
{
  if (ta == s_edit_ta && !s_edit_made) {
    return s_old_value;
  }
  return lv_textarea_get_text(ta);
}


// Public accessor for armed state (used by .ino or other modules)
bool gui_is_armed()
{
  return g_armed;
}

// Public accessor for stopwatch gate mode (used by app_controller)
uint8_t gui_get_stopwatch_mode()
{
  return (uint8_t)g_sw_mode;
}

// Public accessor for current screen (used by app_controller)
bool gui_is_stopwatch_screen()
{
  return g_current_screen == CurrentScreen::Stopwatch;
}


// Forward declarations (used for uniform history clearing)
static void clear_history_for_current_screen();
static void update_history(const char* mode); 
// Forward declarations for history update system
static void history_update_timer_cb(lv_timer_t* timer);
static void request_history_update(const char* mode);



// Timer callback to safely update history from LVGL context
static void history_update_timer_cb(lv_timer_t* timer) {
  (void)timer;
  
  if (g_history_update_pending && g_pending_history_mode) {
    update_history(g_pending_history_mode);
    g_history_update_pending = false;
    g_pending_history_mode = nullptr;
    
    // One-shot timer, delete it
    if (g_history_update_timer) {
      lv_timer_del(g_history_update_timer);
      g_history_update_timer = nullptr;
    }
  }
}

// Helper to request history update from non-LVGL context
static void request_history_update(const char* mode) {
  if (!g_history_update_pending) {
    g_history_update_pending = true;
    g_pending_history_mode = mode;
    
    // Create one-shot timer to run on next LVGL cycle
    if (!g_history_update_timer) {
      g_history_update_timer = lv_timer_create(history_update_timer_cb, 10, nullptr);
      lv_timer_set_repeat_count(g_history_update_timer, 1);
    }
  }
}


// ══════════════════════════════════════════════════════════════════════════════
// [2026-02-07] Poll for experiment completion from real optical gates
// ══════════════════════════════════════════════════════════════════════════════
// Called from main loop when simulation is OFF and experiment is armed.
// Checks if gate events have completed a valid measurement and updates GUI.

// ══════════════════════════════════════════════════════════════════════════════
// Stopwatch real-gate deferred update helpers (must precede the poll function)
// ════════════════════════════════════════════════════════���═════════════════════
static uint64_t sw_last_tsA = 0, sw_last_tsB = 0;
static bool sw_lap_update_pending = false;
static lv_timer_t* sw_lap_update_timer = nullptr;

static void sw_lap_update_timer_cb(lv_timer_t* t) {
    update_lap_history();
    sw_lap_update_timer = nullptr;
    lv_timer_del(t);
}

// ══════════════════════════════════════════════════════════════════════════════
// [2026-02-07] Poll for experiment completion from real optical gates
// [2026-02-11] Per-experiment throttle; stopwatch has zero throttle for speed
// ══════════════════════════════════════════════════════════════════════════════
// Called from main loop when simulation is OFF and experiment is armed.
// Checks if gate events have completed a valid measurement and updates GUI.

void gui_poll_real_gate_experiments() {
  unsigned long now = millis();
  
  // Only poll when armed AND simulation is disabled
  if (!g_armed || g_sim_enabled) {
    return;
  }
  
  // Check which experiment screen is active and try to record.
  // Each case carries its own throttle so the stopwatch can react instantly
  // while physics experiments keep a relaxed 100ms cadence.
  switch (g_current_screen) {

    // ── Physics experiments using SIMPLE TIMESTAMPS (100ms poll, 500ms dedup) ──
    case CurrentScreen::CV: {
      static unsigned long last_cv_poll = 0;
      if (now - last_cv_poll < 100) break;
      last_cv_poll = now;
      static unsigned long last_cv_ok = 0;
      double speed, time_ms;
      std::string formula;
      if (experiments_record_cv(speed, time_ms, formula)) {
        if (now - last_cv_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "%.3f m/s", speed);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("CV");
          Serial.printf("[GUI] CV: %.3f m/s\n", speed);
          last_cv_ok = now;
        }
        gate_clear_trigger_timestamps();
      }
      break;
    }
    
    case CurrentScreen::Photogate: {
      static unsigned long last_pg_poll = 0;
      if (now - last_pg_poll < 100) break;
      last_pg_poll = now;
      static unsigned long last_pg_ok = 0;
      double speed, time_ms;
      std::string formula;
      if (experiments_record_photogate(speed, time_ms, formula)) {
        if (now - last_pg_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "%.3f m/s", speed);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("Photogate");
          Serial.printf("[GUI] Photogate: %.3f m/s\n", speed);
          last_pg_ok = now;
        }
        gate_clear_trigger_timestamps();
      }
      break;
    }
    
    case CurrentScreen::FreeFall: {
      static unsigned long last_ff_poll = 0;
      if (now - last_ff_poll < 100) break;
      last_ff_poll = now;
      static unsigned long last_ff_ok = 0;
      double v_mps, g_mps2, tau_ms;
      std::string formula;
      if (experiments_record_freefall(v_mps, g_mps2, tau_ms, formula)) {
        if (now - last_ff_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "g=%.3f m/s²", g_mps2);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("FreeFall");
          Serial.printf("[GUI] FreeFall: g=%.3f m/s²\n", g_mps2);
          last_ff_ok = now;
        }
        gate_clear_trigger_timestamps();  // ← only clears simple timestamps, NOT block ranges
      }
      break;
    }

    case CurrentScreen::Tachometer: {
      static unsigned long last_ta_poll = 0;
      if (now - last_ta_poll < 100) break;
      last_ta_poll = now;
      static unsigned long last_ta_ok = 0;
      double rpm, period_ms;
      std::string formula;
      if (experiments_record_tacho(rpm, period_ms, formula)) {
        if (now - last_ta_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "%.1f RPM", rpm);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("Tacho");
          Serial.printf("[GUI] Tacho: %.1f RPM\n", rpm);
          last_ta_ok = now;
        }
        gate_clear_trigger_timestamps();
      }
      break;
    }

    // ── Physics experiments using BLOCK RANGES (100ms poll, 500ms dedup) ──
    case CurrentScreen::UA: {
      static unsigned long last_ua_poll = 0;
      if (now - last_ua_poll < 100) break;
      last_ua_poll = now;
      static unsigned long last_ua_ok = 0;
      double a, vmid, tms, v1, v2;
      std::string formula;
      if (experiments_record_ua(a, vmid, tms, formula, &v1, &v2)) {
        if (now - last_ua_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "a=%.3f m/s²", a);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("UA");
          Serial.printf("[GUI] UA: a=%.3f m/s²\n", a);
          last_ua_ok = now;
        }
        gate_clear_trigger_timestamps();
        gate_clear_block_ranges();
      }
      break;
    }

    case CurrentScreen::Incline: {
      static unsigned long last_in_poll = 0;
      if (now - last_in_poll < 100) break;
      last_in_poll = now;
      static unsigned long last_in_ok = 0;
      double a_mps2, v1_mps, v2_mps, total_ms;
      std::string formula;
      if (experiments_record_incline(a_mps2, v1_mps, v2_mps, total_ms, formula)) {
        if (now - last_in_ok > 500) {
          char vbuf[48]; 
          snprintf(vbuf, sizeof(vbuf), "a=%.3f m/s²", a_mps2);
          if (g_val_label) {
            lv_label_set_text(g_val_label, vbuf);
          }
          if (g_formula_label) {
            lv_label_set_text(g_formula_label, formula.c_str());
          }
          request_history_update("Incline");
          Serial.printf("[GUI] Incline: a=%.3f m/s²\n", a_mps2);
          last_in_ok = now;
        }
        gate_clear_trigger_timestamps();
        gate_clear_block_ranges();
      }
      break;
    }

    // ── Stopwatch — NO throttle, NO dedup ───────────────────────────────
    // Timestamp comparison (sw_last_tsA/B) is the only dedup needed.
    // React as fast as loop() runs so short gate pulses are never missed.
    case CurrentScreen::Stopwatch: {
        if (g_sw_mode == SwGateMode::None) break;

        uint64_t tsA = gate_timestamp(GATE_A);
        uint64_t tsB = gate_timestamp(GATE_B);

        // Gate A logic
        if (tsA && tsA != sw_last_tsA) {
            sw_last_tsA = tsA;
            if (g_sw_mode == SwGateMode::GateA) {
                if (!gApp.sw.running()) {
                    gApp.sw.start();
                    sw_record_event(LapEvent::Start);
                } else {
                    gApp.sw.stop();
                    sw_record_event(LapEvent::Stop);
                }
            } else if (g_sw_mode == SwGateMode::GateAB) {
                if (!gApp.sw.running()) {
                    gApp.sw.start();
                    sw_record_event(LapEvent::Start);
                } else {
                    sw_record_event(LapEvent::Lap);
                }
            }
            sw_lap_update_pending = true;
        }

        // Gate B logic
        if (tsB && tsB != sw_last_tsB) {
            sw_last_tsB = tsB;
            if (g_sw_mode == SwGateMode::GateA) {
                if (gApp.sw.running()) sw_record_event(LapEvent::Lap);
            } else if (g_sw_mode == SwGateMode::GateAB) {
                if (gApp.sw.running()) {
                    gApp.sw.stop();
                    sw_record_event(LapEvent::Stop);
                }
            }
            sw_lap_update_pending = true;
        }

        // Deferred LVGL update via one-shot timer (same pattern as other experiments)
        if (sw_lap_update_pending) {
            sw_lap_update_pending = false;
            if (!sw_lap_update_timer) {
                sw_lap_update_timer = lv_timer_create(sw_lap_update_timer_cb, 10, nullptr);
                lv_timer_set_repeat_count(sw_lap_update_timer, 1);
            }
            // Do NOT clear timestamps — sw_last_tsA/B dedup handles this.
        }
        break;
    }
    
    default:
      break;
  }
  
  // REMOVED: the old blanket "if (success) experiments_clear_timestamps()" is gone.
  // Each case above now clears exactly what it consumed.
}

static void on_back(lv_event_t* e) { 
  (void)e;
  
  Serial.println("[on_back] Starting safe transition");
  g_screen_transition_active = true;
  g_armed = false;
  experiment_set_state(ExperimentState::IDLE);
  clear_history_for_current_screen();
  delay(50);
  
  gui_show_main_menu();
  
  delay(50);
  g_screen_transition_active = false;
  Serial.println("[on_back] Transition complete");
}

static void on_settings(lv_event_t* e) { 
  (void)e;
  
  Serial.println("[on_settings] Starting safe transition");
  g_screen_transition_active = true;
  g_armed = false;
  experiment_set_state(ExperimentState::IDLE);
  clear_history_for_current_screen();
  delay(50);
  
  gui_show_settings();
  
  delay(50);
  g_screen_transition_active = false;
  Serial.println("[on_settings] Transition complete");
}

// ── Helpers (history for other experiments) ───────────────────────────────────
static const char* sup2_to_plain(const std::string& s)
{
  static char buf[256];
  size_t n = s.size();
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  memcpy(buf, s.c_str(), n);
  buf[n] = '\0';
  for (size_t i = 0; buf[i]; ++i) {
    unsigned char c = (unsigned char)buf[i];
    if (c == 0xC2 && (unsigned char)buf[i+1] == 0xB2) { // "²"
      buf[i] = '2';
      size_t j = i+2;
      while (buf[j]) { buf[j-1] = buf[j]; ++j; }
      buf[j-1] = '\0';
    }
  }
  return buf;
}


// [Updated: 2026-01-17 15:45:00 CET] Reason: apply custom FONT_SMALL to each history line (typo fix)
static void update_history(const char* mode)
{
    if (!g_history_panel) return;

    lv_obj_clean(g_history_panel);

    // Title
    lv_obj_t* title = lv_label_create(g_history_panel);
    lv_label_set_text(title, tr("Last 10:"));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, FONT_SMALL, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);

    // Pull last 10 strings from experiments.cpp
    auto last10 = experiments_get_last10(mode);
    int y = 36;

    for (auto it = last10.rbegin(); it != last10.rend(); ++it)
    {
        lv_obj_t* lbl = lv_label_create(g_history_panel);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, HISTORY_W - 16);
        lv_label_set_text(lbl, sup2_to_plain(*it));          // keep your ²→2 plain-text helper
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);      // <-- FIX: set font on 'lbl' (not 'title')
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
        lv_obj_update_layout(lbl);
        y += (int)lv_obj_get_height(lbl) + 8;
    }
}

// ── Persistence (general prefs) ───────────────────────────────────────────────
static void load_general_prefs()
{
  Preferences prefs;
  if (prefs.begin("general", true)) {
    g_wifi_enabled = prefs.getBool("wifi_on", false);
    g_sim_enabled  = prefs.getBool("sim_on",  true);
    prefs.end();
  }
}
static void save_wifi_enabled(bool on)
{
  Preferences prefs;
  if (prefs.begin("general", false)) { prefs.putBool("wifi_on", on); prefs.end(); }
  g_wifi_enabled = on;
}
static void save_sim_enabled(bool on)
{
  Preferences prefs;
  if (prefs.begin("general", false)) { prefs.putBool("sim_on",  on); prefs.end(); }
  g_sim_enabled = on;
}

// ── Header lifecycle ─────────────────────────────────────────────────────────
// [2026-01-18 16:15 CET] UPDATED: header_stop_and_clear()
// - clears splash/timers plus unified header pointers (net icon + badge)
// [2026-01-18 CET] UPDATED: header_stop_and_clear() resets unified header pointers
static void header_stop_and_clear()
{
  // Ensure splash timer never survives into real screens
  splash_cancel_if_running();
  if (sw_timer) { lv_timer_del(sw_timer); sw_timer = nullptr; }
  sw_time_lbl = nullptr;
  memset(sw_last_text, 0, sizeof(sw_last_text));

  // Unified header pointers (old g_hdr_wifi_icon / g_hdr_inet_led are no longer used)
  g_hdr_net_icon = nullptr;
  g_hdr_ap_badge = nullptr;
  g_hdr_settings = nullptr;
  g_header_title = nullptr;
  g_header       = nullptr;
  g_val_label    = nullptr;
  g_formula_label= nullptr;
  g_history_panel= nullptr;
  g_hdr_clock    = nullptr;

  // keep g_clock_timer alive globally; optional to delete if you prefer
}


// [Updated: 2026-01-17 16:05:00 CET] Reason: Ensure Stopwatch fully resets when leaving its screen
static void clear_history_for_current_screen()
{
    switch (g_current_screen)
    {
        case CurrentScreen::Stopwatch:
            // Reset the stopwatch engine so elapsed time doesn't persist across screens
            gApp.sw.reset();

            // Clear UI history buffer and numbering (existing helper)
            sw_clear_history();

            // Disarm to match other experiments' "leave screen" behavior
            g_armed = false;
            break;

        case CurrentScreen::CV:
            experiments_clear_history("CV");
            break;

        case CurrentScreen::Photogate:
            experiments_clear_history("Photogate");
            break;

        case CurrentScreen::UA:
            experiments_clear_history("UA");
            break;

        case CurrentScreen::FreeFall:
            experiments_clear_history("FreeFall");
            break;

        case CurrentScreen::Incline:
            experiments_clear_history("Incline");
            break;

        case CurrentScreen::Tachometer:
            experiments_clear_history("Tachometer");
            break;

        default:
            break; // screens with no history
    }
}

// Helper: make a borderless, transparent group container (flex row)
static lv_obj_t* make_borderless_row(lv_obj_t* parent)
{
    lv_obj_t* g = lv_obj_create(parent);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa   (g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_border_opa  (g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(g, 0, 0);
    lv_obj_set_style_outline_opa  (g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa   (g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // CRITICAL: make the group content-sized in BOTH directions
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return g;
}

// ══════════════════════════════════════════════════════════════════════════════
// [2026-02-11] Export-and-reset helper with on-screen toast confirmation
// ═════════════════════════════════════════════════════════════════���════════════
// Shows a brief green/red toast over the current screen, then auto-deletes.
// On success, calls the supplied reset lambda to clear the experiment state.

// ══════════════════════════════════════════════════════════════════════════════
// [2026-02-11] Export toast with i18n and project fonts
// ══════════════════════════════════════════════════════════════════════════════

static void show_export_toast(bool ok, const char* path)
{
    lv_obj_t* toast = lv_obj_create(lv_scr_act());
    lv_obj_set_size(toast, SCREEN_WIDTH - 40, 52);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, HEADER_H + 4);
    lv_obj_set_style_radius(toast, 10, 0);
    lv_obj_set_style_bg_color(toast, ok ? lv_color_hex(0x2E7D32) : lv_color_hex(0xC62828), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_scroll_dir(toast, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(toast, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* lbl = lv_label_create(toast);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

    if (ok) {
        const char* fname = path;
        const char* slash = strrchr(path, '/');
        if (slash) fname = slash + 1;
        // tr("Saved") → "Spremi" (hr), "Gespeichert" (de), etc.
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", tr("Saved"), fname);
        lv_label_set_text(lbl, buf);
    } else {
        // tr("Export failed") → "Izvoz neuspješan" (hr), "Export fehlgeschlagen" (de), etc.
        lv_label_set_text(lbl, tr("Export failed"));
    }
    lv_obj_center(lbl);

    // Auto-delete after 2.5 seconds
    lv_timer_create(
        [](lv_timer_t* t) {
            lv_obj_del((lv_obj_t*)t->user_data);
            lv_timer_del(t);
        },
        2500,
        toast
    );
}

// ── Build header using LVGL Grid: [LEFT | CENTER(FR) | RIGHT] ────────────────

// [2026-01-18 15:45 CET] UPDATED: build_header()
// - Single unified net icon (g_hdr_net_icon) at LEFT, color-coded by internet status
// - Tiny "AP" badge (g_hdr_ap_badge) appears when AP running
// - Removed separate Wi-Fi icon + Internet LED to save space

// [2026-01-18 CET] UPDATED: build_header() with one Wi‑Fi icon + tiny "AP" badge
static lv_obj_t* build_header(lv_obj_t* scr, const char* title_text)
{
  lv_obj_t* header = lv_obj_create(scr);
  g_header = header;

  lv_obj_set_size(header, SCREEN_WIDTH, HEADER_H);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(header, CLR_HDR(), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_scroll_dir(header, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_outline_width(header, 0, 0);
  lv_obj_set_style_outline_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(header, 6, 0);
  lv_obj_set_style_pad_right(header, 6, 0);
  lv_obj_set_style_pad_top(header, 0, 0);
  lv_obj_set_style_pad_bottom(header, 0, 0);

  static const lv_coord_t col_dsc[] = {
    LV_GRID_CONTENT,  // LEFT cluster
    LV_GRID_FR(1),    // CENTER title
    LV_GRID_CONTENT,  // RIGHT cluster
    LV_GRID_TEMPLATE_LAST
  };
  static const lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(header, col_dsc, row_dsc);

  // LEFT group: Back + unified net icon + tiny AP badge
  lv_obj_t* left = lv_obj_create(header);
  lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_set_style_outline_width(left, 0, 0);
  lv_obj_set_style_shadow_opa(left, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(left, 0, 0);
  lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(left, 12, 0);
  lv_obj_set_grid_cell(left, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  // Back button
  lv_obj_t* btn_back = lv_btn_create(left);
  lv_obj_set_size(btn_back, 60, 60);
  lv_obj_set_style_radius(btn_back, 10, 0);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x1F2238), 0);
  lv_obj_set_style_bg_opa(btn_back, LV_OPA_90, 0);
  lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, nullptr);
  {
    lv_obj_t* lbl = lv_label_create(btn_back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
  }

  // Unified Wi‑Fi icon (hidden, but kept for compatibility)
  if (g_hdr_net_icon) { lv_obj_del(g_hdr_net_icon); g_hdr_net_icon = nullptr; }
  if (g_hdr_ap_badge) { lv_obj_del(g_hdr_ap_badge); g_hdr_ap_badge = nullptr; }

  g_hdr_net_icon = lv_label_create(left);
  lv_label_set_text(g_hdr_net_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(g_hdr_net_icon, &lv_font_montserrat_28, 0);
  // HIDE WiFi icon - we only show AP badge
  lv_obj_add_flag(g_hdr_net_icon, LV_OBJ_FLAG_HIDDEN);

  // Tiny "AP" text badge (standalone, no WiFi icon)
  g_hdr_ap_badge = lv_label_create(left);
  lv_label_set_text(g_hdr_ap_badge, "AP");
  lv_obj_set_style_text_font(g_hdr_ap_badge, FONT_SMALL, 0);
  lv_obj_set_style_text_color(g_hdr_ap_badge, lv_color_hex(0x00E07A), 0);
  // AP badge appears in the flex layout of 'left' container (after back button)
  // Hidden by default, shown when AP is running
  if (!g_ap_running) lv_obj_add_flag(g_hdr_ap_badge, LV_OBJ_FLAG_HIDDEN);

  // CENTER: Title
  g_header_title = lv_label_create(header);
  lv_label_set_text(g_header_title, title_text);
  lv_obj_set_style_text_font(g_header_title, FONT_TITLE, 0);
  lv_obj_set_style_text_color(g_header_title, lv_color_white(), 0);
  lv_obj_set_width(g_header_title, LV_PCT(100));
  lv_obj_set_style_text_align(g_header_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_grid_cell(g_header_title, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  // RIGHT: Clock + Settings (unchanged)
  lv_obj_t* right = lv_obj_create(header);
  lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(right, 0, 0);
  lv_obj_set_style_outline_width(right, 0, 0);
  lv_obj_set_style_shadow_opa(right, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(right, 0, 0);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(right, 12, 0);
  lv_obj_set_grid_cell(right, LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  if (g_hdr_settings) { lv_obj_del(g_hdr_settings); g_hdr_settings = nullptr; }
  if (g_hdr_clock)    { lv_obj_del(g_hdr_clock);    g_hdr_clock    = nullptr; }

  g_hdr_clock = lv_label_create(right);
  lv_label_set_text(g_hdr_clock, "--:--:--");
  lv_obj_set_style_text_color(g_hdr_clock, lv_color_white(), 0);
  lv_obj_set_style_text_font(g_hdr_clock, &lv_font_montserrat_28, 0);

  g_hdr_settings = lv_btn_create(right);
  lv_obj_set_size(g_hdr_settings, 60, 60);
  lv_obj_set_style_radius(g_hdr_settings, 10, 0);
  lv_obj_set_style_bg_color(g_hdr_settings, lv_color_hex(0x1F2238), 0);
  lv_obj_set_style_bg_opa(g_hdr_settings, LV_OPA_90, 0);
  lv_obj_add_event_cb(g_hdr_settings, on_settings, LV_EVENT_CLICKED, nullptr);
  {
    lv_obj_t* lbl = lv_label_create(g_hdr_settings);
    lv_label_set_text(lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
  }

  // Reserve fixed width on the RIGHT so the title stays perfectly centered
  const lv_font_t* CLOCK_FONT = &lv_font_montserrat_28;
  const int GEAR_BTN_W = 60;
  const int RIGHT_GAP_PX = 12;

  lv_obj_add_flag(g_hdr_clock, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_font(g_hdr_clock, CLOCK_FONT, 0);
  lv_label_set_text(g_hdr_clock, "88:88:88");
  lv_obj_update_layout(g_hdr_clock);
  int clock_max_w = (int)lv_obj_get_content_width(g_hdr_clock);
  clock_max_w += 10;
  lv_obj_set_width(g_hdr_clock, clock_max_w);
  lv_obj_set_style_text_align(g_hdr_clock, LV_TEXT_ALIGN_LEFT, 0);
  const int right_fixed_w = clock_max_w + RIGHT_GAP_PX + GEAR_BTN_W;
  lv_obj_set_style_min_width(right, right_fixed_w, 0);
  lv_obj_set_style_max_width(right, right_fixed_w, 0);
  lv_obj_clear_flag(g_hdr_clock, LV_OBJ_FLAG_HIDDEN);
  if (!g_clock_timer) g_clock_timer = lv_timer_create(clock_tick_cb, 1000, nullptr);
  clock_tick_cb(nullptr);

  // Initial color for unified icon
  extern bool g_wifi_enabled;
  gui_set_net_icon_color(/*wifi_enabled*/ (g_ap_running || g_wifi_enabled), /*internet_ok*/ false);

  return header;
}



// ── Generic Buttons ──────────────────────────────────────────────────────────
static int       MENU_BTN_W = 340;
static int       MENU_BTN_H = 72;
static lv_obj_t* make_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb)
{
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 160, 50);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

static lv_obj_t* make_footer_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, int w)
{
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, 56);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, FONT_FOOTER, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

static lv_obj_t* make_sim_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb)
{
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 140, 46);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}


// [2026-01-18 20:56 CET] FIX: definition has NO default arguments
static void set_arm_button_visual(lv_obj_t* btn_arm, bool armed,
    const char* armed_text,
    const char* disarmed_text)
{
    if (!btn_arm) return;
    lv_obj_t* lbl = lv_obj_get_child(btn_arm, 0);
    if (armed) {
        lv_obj_set_style_bg_color(btn_arm, CLR_ARMED(), 0);
        if (lbl) lv_label_set_text(lbl, armed_text);
    } else {
        lv_obj_set_style_bg_color(btn_arm, CLR_BTN(), 0);
        if (lbl) lv_label_set_text(lbl, disarmed_text);
    }
}

static lv_obj_t* make_caption(lv_obj_t* parent, const char* caption)
{
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, caption);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);
  return lbl;
}

static lv_obj_t* make_help(lv_obj_t* parent, const char* text, lv_obj_t* anchor, int dx = 0, int dy = 8)
{
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
  lv_obj_align_to(lbl, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, dx, dy);
  return lbl;
}

// ── Common measurement content builder ───────────────────────────────────────
static void build_measurement_labels(lv_obj_t* content, const char* initial_val)
{
  lv_obj_update_layout(content);

  g_val_label = lv_label_create(content);
  lv_label_set_text(g_val_label, initial_val);
  lv_obj_set_style_text_font(g_val_label, FONT_BIG, 0);
  lv_obj_set_style_text_color(g_val_label, lv_color_white(), 0);
  lv_label_set_long_mode(g_val_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_val_label, lv_obj_get_width(content) - 40);
  lv_obj_align(g_val_label, LV_ALIGN_TOP_LEFT, 20, 10);
  lv_obj_clear_flag(g_val_label, LV_OBJ_FLAG_HIDDEN);

  g_formula_label = lv_label_create(content);
  lv_label_set_text(g_formula_label, "");
  lv_obj_set_style_text_font(g_formula_label, FONT_SMALL, 0);
  lv_obj_set_style_text_color(g_formula_label, lv_color_white(), 0);
  lv_label_set_long_mode(g_formula_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_formula_label, lv_obj_get_width(content) - 40);
  lv_obj_align_to(g_formula_label, g_val_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
  lv_obj_clear_flag(g_formula_label, LV_OBJ_FLAG_HIDDEN);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stopwatch — Helper functions
// ─────────────────────────────────────────────────────────────────────────────

static const char* sw_mode_name(SwGateMode m)
{
  switch (m) {
    case SwGateMode::None:  return tr("No gate");
    case SwGateMode::GateA: return tr("Gate A");
    case SwGateMode::GateAB:return tr("Gate A+B");
    default:                return tr("No gate");
  }
}

static void sw_mode_load()
{
  Preferences prefs;
  if (prefs.begin("stopwatch", true)) {
    uint8_t m = prefs.getUChar("gate_mode", (uint8_t)SwGateMode::None);
    prefs.end();
    g_sw_mode = (m <= (uint8_t)SwGateMode::GateAB) ? (SwGateMode)m : SwGateMode::None;
  } else {
    g_sw_mode = SwGateMode::None;
  }
}
static void sw_mode_save(SwGateMode m)
{
  g_sw_mode = m;
  Preferences prefs;
  if (prefs.begin("stopwatch", false)) { prefs.putUChar("gate_mode", (uint8_t)m); prefs.end(); }
}

// Stopwatch history with event type + raw time
struct LapEntry {
  uint32_t    num;  // absolute entry number
  std::string text; // "MM:SS.mmm"
  LapEvent    evt;  // Start / Stop / Lap
  uint64_t    us;   // elapsed us at record time (for CSV)
};
static std::vector<LapEntry> lap_history;
static uint32_t sw_lap_counter = 0;

static inline void fmt_time_ms(char* out, size_t n, uint64_t us_total)
{
  uint64_t ms = us_total / 1000ULL;
  uint32_t mm = ms / 60000ULL;
  uint32_t ss = (ms / 1000ULL) % 60ULL;
  uint32_t mmm= ms % 1000ULL;
  snprintf(out, n, "%02u:%02u.%03u", mm, ss, mmm);
}

static const char* evt_name(LapEvent e)
{
  switch (e) { case LapEvent::Start: return tr("Start"); case LapEvent::Stop: return tr("Stop"); default: return tr("Lap"); }
}

static void sw_record_event(LapEvent e)
{
  uint64_t us = gui_get_stopwatch_us();
  char buf[32];
  fmt_time_ms(buf, sizeof(buf), us);
  if (lap_history.size() >= 10) lap_history.erase(lap_history.begin());
  lap_history.push_back(LapEntry{ ++sw_lap_counter, std::string(buf), e, us });
}

static void update_lap_history()
{
  if (!g_history_panel) return;
  lv_obj_clean(g_history_panel);

  lv_obj_t* title = lv_label_create(g_history_panel);
  lv_label_set_text(title, tr("Last 10 Laps:"));
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, FONT_SMALL, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);

  int y = 36;
  for (auto it = lap_history.rbegin(); it != lap_history.rend(); ++it) {
    lv_obj_t* lbl = lv_label_create(g_history_panel);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, HISTORY_W - 16);
    lv_label_set_text_fmt(lbl, "%u. [%s] %s", it->num, evt_name(it->evt), it->text.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_update_layout(lbl);
    y += (int)lv_obj_get_height(lbl) + 8;
  }
}

// Public wrappers for stopwatch event recording (used by app_controller for real gate events)
void gui_sw_record_start()
{
  sw_record_event(LapEvent::Start);
  update_lap_history();
}

void gui_sw_record_stop()
{
  sw_record_event(LapEvent::Stop);
  update_lap_history();
}

void gui_sw_record_lap()
{
  sw_record_event(LapEvent::Lap);
  update_lap_history();
}

static void sw_clear_history()
{
  lap_history.clear();
  sw_lap_counter = 0;
  update_lap_history();
}

// Stopwatch UI elements
static lv_obj_t* sw_btn_startstop = nullptr;
static lv_obj_t* sw_btn_settings  = nullptr;
static lv_obj_t* sw_btn_lap_or_export = nullptr;

// Stopwatch update timer
// [Updated: 2026-01-17 17:06:00 CET] Reason: smoother counter — 10 ms while running, 250 ms while idle
static void sw_update_cb(lv_timer_t*)
{
    const bool running = gApp.sw.running();  // engine state (unchanged)  [1](https://melektro1-my.sharepoint.com/personal/robert_horvat_montelektro_hr/Documents/Microsoft%20Copilot%20Chat%20Files/experiments.cpp)
    static bool s_last_running = false;

    // Tighten/relax the refresh period exactly when state flips
    if (running != s_last_running && sw_timer) {
        lv_timer_set_period(sw_timer, running ? 10 : 250);   // 100 Hz when running
        s_last_running = running;
    }

    // Format "MM:SS.mmm" only when content actually changes (avoids redundant redraws)
    const uint64_t us = gui_get_stopwatch_us();              // engine-provided elapsed  [1](https://melektro1-my.sharepoint.com/personal/robert_horvat_montelektro_hr/Documents/Microsoft%20Copilot%20Chat%20Files/experiments.cpp)
    char buf[24];
    fmt_time_ms(buf, sizeof(buf), us);                       // helper you already have  [1](https://melektro1-my.sharepoint.com/personal/robert_horvat_montelektro_hr/Documents/Microsoft%20Copilot%20Chat%20Files/experiments.cpp)

    if (strcmp(buf, sw_last_text) != 0) {
        if (sw_time_lbl) lv_label_set_text(sw_time_lbl, buf);
        strncpy(sw_last_text, buf, sizeof(sw_last_text) - 1);
        sw_last_text[sizeof(sw_last_text) - 1] = '\0';
    }
}


// Stopwatch gate simulation handlers
static void sim_sw_gate_a(lv_event_t* /*e*/)
{
  if (!g_sim_enabled) return;
  if (g_sw_mode == SwGateMode::None) {
    sw_record_event(LapEvent::Lap);
    update_lap_history();
    return;
  }
  if (!g_armed) return;
  gate_simulate_gate_a();
  if (g_sw_mode == SwGateMode::GateA) {
    if (!gApp.sw.running()) {
      gApp.sw.start();
      sw_record_event(LapEvent::Start);
    } else {
      gApp.sw.stop();
      sw_record_event(LapEvent::Stop);
    }
  } else if (g_sw_mode == SwGateMode::GateAB) {
    if (!gApp.sw.running()) {
      gApp.sw.start();
      sw_record_event(LapEvent::Start);
    } else {
      sw_record_event(LapEvent::Lap);
    }
  }
  update_lap_history();
}


// [Updated: 2026-01-17 17:20:00 CET] Reason: Gate-AB stop paints final time immediately on B release
static void sim_sw_gate_b(lv_event_t* /*e*/)
{
    if (!g_sim_enabled) return;

    if (g_sw_mode == SwGateMode::None) {
        sw_record_event(LapEvent::Lap);
        update_lap_history();
        return;
    }
    if (!g_armed) return;

    gate_simulate_gate_b();

    if (g_sw_mode == SwGateMode::GateA) {
        sw_record_event(LapEvent::Lap);
    } else if (g_sw_mode == SwGateMode::GateAB) {
        if (gApp.sw.running()) {
            gApp.sw.stop();
            sw_record_event(LapEvent::Stop);
            update_lap_history();

            // ── NEW: latch & paint final time instantly
            const uint64_t us_final = gui_get_stopwatch_us();
            char buf[24]; fmt_time_ms(buf, sizeof(buf), us_final);
            if (sw_time_lbl) lv_label_set_text(sw_time_lbl, buf);
            strncpy(sw_last_text, buf, sizeof(sw_last_text) - 1);
            sw_last_text[sizeof(sw_last_text) - 1] = '\0';

            // Relax refresh (optional, keeps UX consistent)
            if (sw_timer) lv_timer_set_period(sw_timer, 250);
        } else {
            sw_record_event(LapEvent::Lap);
        }
    }
    update_lap_history();
}


// Footer callbacks

// [Updated: 2026-01-17 17:06:00 CET] Reason: switch timer period immediately on start/stop for smoother feel
// [Updated: 2026-01-17 17:20:00 CET] Reason: latch & paint final time immediately on Stop (eliminate post-stop “creep”)
static void sw_startstop_cb(lv_event_t*)
{
    if (g_sw_mode == SwGateMode::None)
    {
        const bool was_running = gApp.sw.running();
        gApp.sw.toggle();
        const bool now_running = gApp.sw.running();

        // Button visuals + history (unchanged)
        lv_obj_t* lbl = lv_obj_get_child(sw_btn_startstop, 0);
        if (now_running) {
            lv_obj_set_style_bg_color(sw_btn_startstop, CLR_ARMED(), 0);
            if (lbl) lv_label_set_text(lbl, tr("Running"));
            sw_record_event(LapEvent::Start);
        } else {
            lv_obj_set_style_bg_color(sw_btn_startstop, CLR_BTN(), 0);
            if (lbl) lv_label_set_text(lbl, tr("Start / Stop"));
            if (was_running) sw_record_event(LapEvent::Stop);
        }
        update_lap_history();

        // ── NEW: if we just STOPPED, latch and paint the final time immediately
        if (!now_running) {
            const uint64_t us_final = gui_get_stopwatch_us();   // engine’s latched final time
            char buf[24];
            fmt_time_ms(buf, sizeof(buf), us_final);            // "MM:SS.mmm"
            if (sw_time_lbl) lv_label_set_text(sw_time_lbl, buf);
            strncpy(sw_last_text, buf, sizeof(sw_last_text) - 1);
            sw_last_text[sizeof(sw_last_text) - 1] = '\0';

            // Also relax UI refresh right away (smooth UX, saves cycles)
            if (sw_timer) lv_timer_set_period(sw_timer, 250);
        } else {
            // If we just started, speed up the UI timer immediately
            if (sw_timer) lv_timer_set_period(sw_timer, 10);
        }
        return;
    }

    // ── Gate modes (unchanged behavior + timer period tweak) ────────────────
    g_armed = !g_armed;
    
    // NEW: Set experiment state for gate polling
    if (g_armed) {
        experiment_set_state(ExperimentState::ARMED);
    } else {
        experiment_set_state(ExperimentState::IDLE);
    }
    
    set_arm_button_visual(sw_btn_startstop, g_armed, tr("Armed"), tr("Start / Arm"));
    if (!g_armed && gApp.sw.running()) {
        gApp.sw.stop();
        sw_record_event(LapEvent::Stop);
        update_lap_history();

        // Optional: latch & paint here too for consistency with manual mode
        const uint64_t us_final = gui_get_stopwatch_us();
        char buf[24]; fmt_time_ms(buf, sizeof(buf), us_final);
        if (sw_time_lbl) lv_label_set_text(sw_time_lbl, buf);
        strncpy(sw_last_text, buf, sizeof(sw_last_text) - 1);
        sw_last_text[sizeof(sw_last_text) - 1] = '\0';
    }
    if (sw_timer) lv_timer_set_period(sw_timer, (g_armed && gApp.sw.running()) ? 10 : 250);
}


static void sw_reset_cb(lv_event_t*)
{
  gApp.sw.reset();
  lap_history.clear();
  sw_lap_counter = 0;
  update_lap_history();
  if (sw_time_lbl) lv_label_set_text(sw_time_lbl, "00:00.000");
  g_armed = false;
  
  // Set experiment state to IDLE when resetting
  experiment_set_state(ExperimentState::IDLE);
  
  if (g_sw_mode == SwGateMode::None) {
    lv_obj_t* lbl = lv_obj_get_child(sw_btn_startstop, 0);
    lv_obj_set_style_bg_color(sw_btn_startstop, CLR_BTN(), 0);
    if (lbl) lv_label_set_text(lbl, tr("Start / Stop"));
  } else {
    set_arm_button_visual(sw_btn_startstop, false, tr("Armed"), tr("Start / Arm"));
  }
}

static void sw_lap_cb(lv_event_t*)
{
  sw_record_event(LapEvent::Lap);
  update_lap_history();
}


// [2026-01-18 13:55 CET] NEW: Stopwatch CSV emitter for exportfs_save_csv()
// Generates a complete stopwatch CSV including event name and elapsed time.
static void stopwatch_emit_csv(Print& out)
{
    out.println("num,event,elapsed_ms,elapsed_text");

    for (const auto& r : lap_history)
    {
        double ms = r.us / 1000.0;
        out.printf("%u,%s,%.3f,%s\n",
                   r.num,
                   evt_name(r.evt),
                   ms,
                   r.text.c_str());
    }
}

static void sw_export_csv_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][Stopwatch] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("Stopwatch", stopwatch_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][Stopwatch] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset stopwatch after successful export
        gApp.sw.reset();
        sw_clear_history();
        if (sw_time_lbl) lv_label_set_text(sw_time_lbl, "00:00.000");
        g_armed = false;
        if (g_sw_mode == SwGateMode::None) {
            lv_obj_t* lbl = lv_obj_get_child(sw_btn_startstop, 0);
            lv_obj_set_style_bg_color(sw_btn_startstop, CLR_BTN(), 0);
            if (lbl) lv_label_set_text(lbl, tr("Start / Stop"));
        } else {
            set_arm_button_visual(sw_btn_startstop, false, tr("Armed"), tr("Start / Arm"));
        }
    } else {
        Serial.println("[Export][Stopwatch] save failed");
        show_export_toast(false, "");
    }
}


static void sw_settings_cb(lv_event_t*)
{
  // Uniform policy: clear stopwatch lap history when leaving the screen
  clear_history_for_current_screen();
  gui_show_stopwatch_settings();
}

// Render Stopwatch

// [Updated: 2026-01-17 16:36:00 CET] Reason: render Stopwatch counter with ui_font_84
// [Updated: 2026-01-17 16:58:00 CET] Reason: vertically center stopwatch counter within content area; keep dynamic refresh & ui_font_62
// [Updated: 2026-01-17 17:06:00 CET] Reason: center counter and create timer; sw_update_cb will run at 10 ms when running
// [Updated: 2026-01-17 17:28:00 CET] Reason: lift stopwatch counter slightly above vertical center for better balance
void gui_show_stopwatch()
{
    g_current_screen = CurrentScreen::Stopwatch;

    header_stop_and_clear();
    load_general_prefs();
    sw_mode_load();
    g_armed = false;

    char title[64];
    snprintf(title, sizeof(title), "%s (%s)", tr("Stopwatch"), sw_mode_name(g_sw_mode));

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_header(scr, title);

    int topOffset = HEADER_H;

    // Simulation row only for gate modes
    const bool showA = (g_sw_mode == SwGateMode::GateA || g_sw_mode == SwGateMode::GateAB);
    const bool showB = (g_sw_mode == SwGateMode::GateAB);
    if (showA || showB) {
        lv_obj_t* sim_row = lv_obj_create(scr);
        lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
        lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
        lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
        // Non-scrollable
        lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

        if (showA) {
            lv_obj_t* bA = make_sim_btn(sim_row, tr("Gate A"), sim_sw_gate_a);
            lv_obj_align(bA, LV_ALIGN_LEFT_MID, 20, 0);
        }
        if (showB) {
            lv_obj_t* bB = make_sim_btn(sim_row, tr("Gate B"), sim_sw_gate_b);
            lv_obj_align(bB, LV_ALIGN_LEFT_MID, showA ? 160 : 20, 0);
        }
        topOffset += SIMROW_H;
    }

    const int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;

    // Results (left) pane
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    // Non-scrollable
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    // Lifted centered stopwatch counter
    static const int COUNTER_Y_OFFSET = -24;       // tweak: -12 (subtle) .. -40 (higher)
    sw_time_lbl = lv_label_create(content);
    lv_label_set_text(sw_time_lbl, "00:00.000");
    lv_obj_set_style_text_font(sw_time_lbl, FONT_STOPWATCH, 0);
    lv_obj_set_style_text_color(sw_time_lbl, lv_color_white(), 0);
    lv_obj_align(sw_time_lbl, LV_ALIGN_CENTER, 0, COUNTER_Y_OFFSET);

    // Footer (buttons)
    lv_obj_t* footer = lv_obj_create(scr);
    lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);
    // Non-scrollable
    lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

    const bool is_gate_mode = (g_sw_mode != SwGateMode::None);
    const char* start_text  = is_gate_mode ? tr("Start / Arm") : tr("Start / Stop");
    sw_btn_startstop      = make_footer_btn(footer, start_text,     sw_startstop_cb, 120);
    lv_obj_t* bReset      = make_footer_btn(footer, tr("Reset"),     sw_reset_cb,     120);
    sw_btn_lap_or_export  = is_gate_mode
                            ? make_footer_btn(footer, tr("Export CSV"), sw_export_csv_cb, 120)
                            : make_footer_btn(footer, tr("Lap"),        sw_lap_cb,        120);
    sw_btn_settings       = make_footer_btn(footer, tr("Settings"),  sw_settings_cb,  120);

    lv_obj_align(sw_btn_startstop,     LV_ALIGN_LEFT_MID,  12, 0);
    lv_obj_align(bReset,               LV_ALIGN_LEFT_MID, 144, 0);
    lv_obj_align(sw_btn_lap_or_export, LV_ALIGN_LEFT_MID, 276, 0);
    lv_obj_align(sw_btn_settings,      LV_ALIGN_LEFT_MID, 408, 0);

    if (is_gate_mode)
        set_arm_button_visual(sw_btn_startstop, false, tr("Armed"), tr("Start / Arm"));

    // History (right) pane
    g_history_panel = lv_obj_create(scr);
    lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
    lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
    update_lap_history();

    // Create the update timer; sw_update_cb will switch to 10 ms when running
    sw_timer = lv_timer_create(sw_update_cb, 250, nullptr);

    // Show the screen
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    
}

// Stopwatch Settings — dropdown view
static void on_sw_mode_dd_changed(lv_event_t* e)
{
  lv_obj_t* dd = lv_event_get_target(e);
  int idx = lv_dropdown_get_selected(dd);
  SwGateMode m = SwGateMode::None;
  if (idx == 1) m = SwGateMode::GateA;
  else if (idx == 2) m = SwGateMode::GateAB;
  sw_mode_save(m);
  gApp.sw.reset();
  lap_history.clear();
  sw_lap_counter = 0;
  g_armed = false;
  
  // Set experiment state to IDLE when mode changes
  experiment_set_state(ExperimentState::IDLE);
  
  gui_show_stopwatch();
}

void gui_show_stopwatch_settings()
{
  g_current_screen = CurrentScreen::SWSettings;
  header_stop_and_clear();
  sw_mode_load();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Stopwatch Settings"));

  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* cap = make_caption(content, tr("Gate mode:"));
  lv_obj_align(cap, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0);

  lv_obj_t* dd_mode = lv_dropdown_create(content);
  // Localize options
  std::string dd_opts = std::string(tr("No gate")) + "\n" + tr("Gate A") + "\n" + tr("Gate A+B");
  lv_dropdown_set_options(dd_mode, dd_opts.c_str());
  int sel = 0; switch (g_sw_mode) { case SwGateMode::None: sel=0; break; case SwGateMode::GateA: sel=1; break; case SwGateMode::GateAB: sel=2; break; }
  lv_dropdown_set_selected(dd_mode, sel);
  lv_obj_set_size(dd_mode, FIELD_W, FIELD_H);
  lv_obj_align(dd_mode, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, CAPTION_Y0);
  lv_obj_add_event_cb(dd_mode, on_sw_mode_dd_changed, LV_EVENT_VALUE_CHANGED, nullptr);

  make_help(content, tr("Stopwatch gate help"), cap);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// CV Screen
// ─────────────────────────────────────────────────────────────────────────────


static void cv_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(cv_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  lv_label_set_text(g_val_label, "0.000 m/s");
  lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
  experiments_clear_history("CV");
  update_history("CV");
}

static void cv_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][CV] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("CV", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][CV] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset CV after successful export
        g_armed = false;
        set_arm_button_visual(cv_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        if (g_val_label) lv_label_set_text(g_val_label, "0.000 m/s");
        if (g_formula_label) lv_label_set_text(g_formula_label, "");
        experiments_clear_history("CV");
        update_history("CV");
    } else {
        Serial.println("[Export][CV] save failed");
        show_export_toast(false, "");
    }
}


static void cv_settings_cb(lv_event_t*) { clear_history_for_current_screen(); gui_show_cv_settings(); }
static void sim_cv_gate_a(lv_event_t* /*e*/) { if (g_sim_enabled) gate_simulate_gate_a(); }
static void sim_cv_gate_b(lv_event_t* /*e*/)
{
  if (!g_sim_enabled || !g_armed) return;
  gate_simulate_gate_b();
  double v, tms; std::string formula;
  if (experiments_record_cv(v, tms, formula)) {
    char buf[64]; snprintf(buf, sizeof(buf), "%.3f m/s", v);
    lv_label_set_text(g_val_label, buf); lv_obj_invalidate(g_val_label);
    lv_label_set_text(g_formula_label, formula.c_str()); lv_obj_invalidate(g_formula_label);
    update_history("CV");
  }
}

void gui_show_cv()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::CV;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Linear Motion (CV)"));

  int topOffset = HEADER_H;
  // ALWAYS show simulation buttons (animate on real gate events)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  g_sim_btn_a = make_sim_btn(sim_row, tr("Gate A"), sim_cv_gate_a);
  g_sim_btn_b = make_sim_btn(sim_row, tr("Gate B"), sim_cv_gate_b);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_align(g_sim_btn_b, LV_ALIGN_LEFT_MID, 160, 0);
  topOffset += SIMROW_H;
  

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
  build_measurement_labels(content, "0.000 m/s");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);

    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm    = make_footer_btn(footer, tr("Start / Arm"), cv_arm_toggle_cb, 120);
  lv_obj_t* bReset  = make_footer_btn(footer, tr("Reset"),        cv_reset_cb,      120);
  lv_obj_t* bCSV    = make_footer_btn(footer, tr("Export CSV"),   cv_export_cb,     120);
  lv_obj_t* bSettings=make_footer_btn(footer, tr("Settings"),     cv_settings_cb,   120);

  lv_obj_align(bArm,     LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,   LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,     LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings,LV_ALIGN_LEFT_MID, 408, 0);

  cv_btn_arm = bArm; set_arm_button_visual(cv_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("CV");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// Photogate Screen
// ─────────────────────────────────────────────────────────────────────────────
static lv_obj_t* pg_btn_arm = nullptr;
static void pg_arm_toggle_cb(lv_event_t*)
{
  g_armed = !g_armed;

  // NEW: Set experiment state
  if (g_armed) {
    experiment_set_state(ExperimentState::ARMED);
  } else {
    experiment_set_state(ExperimentState::IDLE);
  }

  set_arm_button_visual(pg_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  experiments_clear_timestamps();
}
static void pg_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(pg_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  lv_label_set_text(g_val_label, "0.000 m/s");
  lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
  experiments_clear_history("Photogate");
  update_history("Photogate");
}


static void pg_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][PG] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("Photogate", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][PG] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset Photogate after successful export
        g_armed = false;
        set_arm_button_visual(pg_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        if (g_val_label) lv_label_set_text(g_val_label, "0.000 m/s");
        if (g_formula_label) lv_label_set_text(g_formula_label, "");
        experiments_clear_history("Photogate");
        update_history("Photogate");
    } else {
        Serial.println("[Export][PG] save failed");
        show_export_toast(false, "");
    }
}


// [2026-01-18 21:24 CET] ADD: Photogate simulation + settings callback (linker fix)
// [2026-01-26 21:22 CET] FIX: Photogate — record on Unblock; paint big (speed) + small (formula)
// [2026-01-26 21:28 CET] FIX: Photogate — compute & paint on Unblock

// [2026-01-26 21:34 CET] FIX: Photogate — use legacy A/B timestamps for simulation
// Reason: experiments_record_photogate() reads gate_timestamp(GATE_A/B),
// so we must simulate Block→A and Unblock→B via gate_simulate_block()/unblock().

static void sim_pg_block(lv_event_t* /*e*/)
{
    if (!g_sim_enabled || !g_armed) return;

    // Legacy mapping for Photogate: Block -> Gate A timestamp
    gate_simulate_block(); // sets gate_timestamp(GATE_A)
}

static void sim_pg_unblock(lv_event_t* /*e*/)
{
    if (!g_sim_enabled || !g_armed) return;

    // Legacy mapping for Photogate: Unblock -> Gate B timestamp
    gate_simulate_unblock(); // sets gate_timestamp(GATE_B)

    // Now compute & paint result using the legacy A/B timestamps
    double v_mps = 0.0, t_ms = 0.0;
    std::string formula;
    if (experiments_record_photogate(v_mps, t_ms, formula)) {  // uses gate_timestamp(A/B)
        char big[64];  snprintf(big, sizeof(big), "%.3f m/s", v_mps);
        if (g_val_label)     { lv_label_set_text(g_val_label, big); lv_obj_invalidate(g_val_label); }
        if (g_formula_label) { lv_label_set_text(g_formula_label, formula.c_str()); lv_obj_invalidate(g_formula_label); }
        update_history("Photogate");
    }
}





static void pg_settings_cb(lv_event_t* /*e*/)
{
    clear_history_for_current_screen();
    gui_show_pg_settings();
}

void gui_show_photogate()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::Photogate;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Photogate Speed"));

  int topOffset = HEADER_H;
  // ALWAYS show simulation buttons (animate on real gate events)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  // Note: Photogate uses "Block"/"Unblock" but maps to Gate A/B internally
  g_sim_btn_a = make_sim_btn(sim_row, tr("Block"),   sim_pg_block);
  g_sim_btn_b = make_sim_btn(sim_row, tr("Unblock"), sim_pg_unblock);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_align(g_sim_btn_b, LV_ALIGN_LEFT_MID, 160, 0);
  topOffset += SIMROW_H;
  

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
  build_measurement_labels(content, "0.000 m/s");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);

    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm     = make_footer_btn(footer, tr("Start / Arm"), pg_arm_toggle_cb, 120);
  lv_obj_t* bReset   = make_footer_btn(footer, tr("Reset"),        pg_reset_cb,      120);
  lv_obj_t* bCSV     = make_footer_btn(footer, tr("Export CSV"),   pg_export_cb,     120);
  lv_obj_t* bSettings= make_footer_btn(footer, tr("Settings"),     pg_settings_cb,   120);
  lv_obj_align(bArm,      LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,    LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,      LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings, LV_ALIGN_LEFT_MID, 408, 0);

  pg_btn_arm = bArm; set_arm_button_visual(pg_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("Photogate");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// UA Screen — two‑gate press/hold
// ─────────────────────────────────────────────────────────────────────────────
static lv_obj_t* ua_btn_arm = nullptr;
static inline void ua_clear_state()
{
  if (g_val_label)     lv_label_set_text(g_val_label, "0.000 m/s²");
  if (g_formula_label) lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
}

// [Updated: 2026-01-17 15:56:00 CET] Reason: UA should not reset UI on Arm toggle; match other experiments
static void ua_arm_toggle_cb(lv_event_t* e)
{
    (void)e;
    // Toggle armed state and update button visuals
    g_armed = !g_armed;

    // NEW: Set experiment state
    if (g_armed) {
      experiment_set_state(ExperimentState::ARMED);
    } else {
      experiment_set_state(ExperimentState::IDLE);
    }

    set_arm_button_visual(ua_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

    // Clear timing state only (like CV/PG/FF/Incline/Tacho), but do NOT clear UI labels here
    // This keeps the last measured values visible until the user presses Reset.
    experiments_clear_timestamps();
}

static void ua_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(ua_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  experiments_clear_timestamps();
  ua_clear_state();
  experiments_clear_history("UA");
  update_history("UA");
}

static void ua_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][UA] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("UA", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][UA] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset UA after successful export
        g_armed = false;
        set_arm_button_visual(ua_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        experiments_clear_timestamps();
        ua_clear_state();
        experiments_clear_history("UA");
        update_history("UA");
    } else {
        Serial.println("[Export][UA] save failed");
        show_export_toast(false, "");
    }
}


static void ua_settings_cb(lv_event_t*) { clear_history_for_current_screen(); gui_show_ua_settings(); }

// Press/hold simulation events: A and B (press = block; release = unblock)
static void ua_gate_a_event(lv_event_t* e)
{
  if (!g_sim_enabled || !g_armed) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED)      { gate_simulate_block_a(); }
  else if (code == LV_EVENT_RELEASED) { gate_simulate_unblock_a(); }
}
static void ua_gate_b_event(lv_event_t* e)
{
  if (!g_sim_enabled || !g_armed) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    gate_simulate_block_b();
  } else if (code == LV_EVENT_RELEASED) {
    gate_simulate_unblock_b();
    double a, vmid, tms, v1, v2; std::string formula;
    if (experiments_record_ua(a, vmid, tms, formula, &v1, &v2)) {
      char vbuf[48]; snprintf(vbuf, sizeof(vbuf), "a=%.3f m/s²", a);
      if (g_val_label)     { lv_label_set_text(g_val_label, vbuf); lv_obj_invalidate(g_val_label); }
      if (g_formula_label) { lv_label_set_text(g_formula_label, formula.c_str()); lv_obj_invalidate(g_formula_label); }
      update_history("UA");
    }
  }
}

void gui_show_ua()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::UA;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Uniform Acceleration"));

  int topOffset = HEADER_H;
  
  // ALWAYS show simulation buttons (even when simulation is OFF)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  g_sim_btn_a = make_sim_btn(sim_row, tr("Gate A (hold)"), nullptr);
  g_sim_btn_b = make_sim_btn(sim_row, tr("Gate B (hold)"), nullptr);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_align(g_sim_btn_b, LV_ALIGN_LEFT_MID, 160, 0);
  
  // Add event handlers (will check g_sim_enabled inside)
  lv_obj_add_event_cb(g_sim_btn_a, ua_gate_a_event, LV_EVENT_PRESSED,  nullptr);
  lv_obj_add_event_cb(g_sim_btn_a, ua_gate_a_event, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(g_sim_btn_b, ua_gate_b_event, LV_EVENT_PRESSED,  nullptr);
  lv_obj_add_event_cb(g_sim_btn_b, ua_gate_b_event, LV_EVENT_RELEASED, nullptr);
  
  topOffset += SIMROW_H;

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
  build_measurement_labels(content, "0.000 m/s²");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);

    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm     = make_footer_btn(footer, tr("Start / Arm"), ua_arm_toggle_cb, 120);
  lv_obj_t* bReset   = make_footer_btn(footer, tr("Reset"),        ua_reset_cb,      120);
  lv_obj_t* bCSV     = make_footer_btn(footer, tr("Export CSV"),   ua_export_cb,     120);
  lv_obj_t* bSettings= make_footer_btn(footer, tr("Settings"),     ua_settings_cb,   120);
  lv_obj_align(bArm,      LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,    LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,      LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings, LV_ALIGN_LEFT_MID, 408, 0);

  ua_btn_arm = bArm; set_arm_button_visual(ua_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("UA");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// Free Fall Screen
// ─────────────────────────────────────────────────────────────────────────────
static lv_obj_t* ff_btn_arm = nullptr;
static void ff_arm_toggle_cb(lv_event_t*)
{
  g_armed = !g_armed;

  // NEW: Set experiment state
  if (g_armed) {
    experiment_set_state(ExperimentState::ARMED);
  } else {
    experiment_set_state(ExperimentState::IDLE);
  }

  set_arm_button_visual(ff_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  experiments_clear_timestamps();
}
static void ff_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(ff_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  lv_label_set_text(g_val_label, "v=0.000 m/s g=0.000 m/s²");
  lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
  experiments_clear_history("FreeFall");
  update_history("FreeFall");
}


static void ff_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][FF] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("FreeFall", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][FF] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset FreeFall after successful export
        g_armed = false;
        set_arm_button_visual(ff_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        if (g_val_label) lv_label_set_text(g_val_label, "0.000 m/s²");
        if (g_formula_label) lv_label_set_text(g_formula_label, "");
        experiments_clear_history("FreeFall");
        update_history("FreeFall");
    } else {
        Serial.println("[Export][FF] save failed");
        show_export_toast(false, "");
    }
}


// [2026-01-18 21:24 CET] ADD: Free Fall simulation + settings callback (linker fix)
static void sim_ff_gate_a(lv_event_t* /*e*/)
{
    if (!g_sim_enabled || !g_armed) return;
    gate_simulate_gate_a();
}


// [2026-01-26 21:10 CET] FIX: Free Fall simulation now computes & paints results
// [2026-01-26 21:22 CET] FIX: Free Fall — big label shows v and g; small label shows tau + formula
// [2026-01-26 21:28 CET] FIX: Free Fall — big label shows v & g; small shows tau + formula
static void sim_ff_gate_b(lv_event_t* /*e*/)
{
    if (!g_sim_enabled || !g_armed) return;

    gate_simulate_gate_b();  // complete the simulated measurement

    double v_mps = 0.0, g_mps2 = 0.0, tau_ms = 0.0;
    std::string formula;
    if (experiments_record_freefall(v_mps, g_mps2, tau_ms, formula)) {
        // BIG: v and g on the same line (ASCII units to avoid encoding issues)
        char big[96];
        snprintf(big, sizeof(big), "v=%.3f m/s g=%.3f m/s2", v_mps, g_mps2);
        if (g_val_label) { lv_label_set_text(g_val_label, big); lv_obj_invalidate(g_val_label); }

        // SMALL: tau + formula/explanation
        char small[256];
        snprintf(small, sizeof(small), "tau=%.3f ms\n%s", tau_ms, formula.c_str());
        if (g_formula_label) { lv_label_set_text(g_formula_label, small); lv_obj_invalidate(g_formula_label); }

        update_history("FreeFall");
    }
}




static void ff_settings_cb(lv_event_t* /*e*/)
{
    clear_history_for_current_screen();
    gui_show_freefall_settings();
}

void gui_show_freefall()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::FreeFall;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Free Fall"));

  int topOffset = HEADER_H;
  // ALWAYS show simulation buttons (animate on real gate events)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  g_sim_btn_a = make_sim_btn(sim_row, tr("Gate A"), sim_ff_gate_a);
  g_sim_btn_b = make_sim_btn(sim_row, tr("Gate B"), sim_ff_gate_b);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_align(g_sim_btn_b, LV_ALIGN_LEFT_MID, 160, 0);
  topOffset += SIMROW_H;
  

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

  build_measurement_labels(content, "v=0.000 m/s g=0.000 m/s²");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);

    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm     = make_footer_btn(footer, tr("Start / Arm"), ff_arm_toggle_cb, 120);
  lv_obj_t* bReset   = make_footer_btn(footer, tr("Reset"),        ff_reset_cb,      120);
  lv_obj_t* bCSV     = make_footer_btn(footer, tr("Export CSV"),   ff_export_cb,     120);
  lv_obj_t* bSettings= make_footer_btn(footer, tr("Settings"),     ff_settings_cb,   120);
  lv_obj_align(bArm,      LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,    LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,      LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings, LV_ALIGN_LEFT_MID, 408, 0);
  ff_btn_arm = bArm; set_arm_button_visual(ff_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("FreeFall");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// Inclined Plane Screen (UPDATED: press/hold simulation)
// ─────────────────────────────────────────────────────────────────────────────
static lv_obj_t* in_btn_arm = nullptr;
static void in_arm_toggle_cb(lv_event_t*)
{
  g_armed = !g_armed;

  // NEW: Set experiment state
  if (g_armed) {
    experiment_set_state(ExperimentState::ARMED);
  } else {
    experiment_set_state(ExperimentState::IDLE);
  }

  set_arm_button_visual(in_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  experiments_clear_timestamps();
}
static void in_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(in_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  lv_label_set_text(g_val_label, "a=0.000 m/s²");
  lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
  experiments_clear_history("Incline");
  update_history("Incline");
}

static void in_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][Incline] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("Incline", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][Incline] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset Incline after successful export
        g_armed = false;
        set_arm_button_visual(in_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        if (g_val_label) lv_label_set_text(g_val_label, "0.000 m/s²");
        if (g_formula_label) lv_label_set_text(g_formula_label, "");
        experiments_clear_history("Incline");
        update_history("Incline");
    } else {
        Serial.println("[Export][Incline] save failed");
        show_export_toast(false, "");
    }
}

static void in_settings_cb(lv_event_t*) { clear_history_for_current_screen(); gui_show_incline_settings(); }

// NEW: press/hold simulation like UA — provides τA/τB and fronts
static void in_gate_a_event(lv_event_t* e)
{
  if (!g_sim_enabled || !g_armed) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED)      { gate_simulate_block_a(); }
  else if (code == LV_EVENT_RELEASED) { gate_simulate_unblock_a(); }
}

static void in_gate_b_event(lv_event_t* e)
{
  if (!g_sim_enabled || !g_armed) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    gate_simulate_block_b();
  } else if (code == LV_EVENT_RELEASED) {
    gate_simulate_unblock_b();
    double a, v1, v2, tms; std::string formula;
    if (experiments_record_incline(a, v1, v2, tms, formula)) {
      char buf[64]; snprintf(buf, sizeof(buf), "a=%.3f m/s²", a);
      lv_label_set_text(g_val_label, buf); lv_obj_invalidate(g_val_label);
      lv_label_set_text(g_formula_label, formula.c_str()); lv_obj_invalidate(g_formula_label);
      update_history("Incline");
    }
  }
}

void gui_show_incline()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::Incline;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Inclined Plane"));

  int topOffset = HEADER_H;
  // ALWAYS show simulation buttons (animate on real gate events)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  g_sim_btn_a = make_sim_btn(sim_row, tr("Gate A (hold)"), nullptr);
  g_sim_btn_b = make_sim_btn(sim_row, tr("Gate B (hold)"), nullptr);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_align(g_sim_btn_b, LV_ALIGN_LEFT_MID, 160, 0);
  lv_obj_add_event_cb(g_sim_btn_a, in_gate_a_event, LV_EVENT_PRESSED,  nullptr);
  lv_obj_add_event_cb(g_sim_btn_a, in_gate_a_event, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(g_sim_btn_b, in_gate_b_event, LV_EVENT_PRESSED,  nullptr);
  lv_obj_add_event_cb(g_sim_btn_b, in_gate_b_event, LV_EVENT_RELEASED, nullptr);
  topOffset += SIMROW_H;
  

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  
  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

  build_measurement_labels(content, "a=0.000 m/s²");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);

    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm     = make_footer_btn(footer, tr("Start / Arm"), in_arm_toggle_cb, 120);
  lv_obj_t* bReset   = make_footer_btn(footer, tr("Reset"),        in_reset_cb,      120);
  lv_obj_t* bCSV     = make_footer_btn(footer, tr("Export CSV"),   in_export_cb,     120);
  lv_obj_t* bSettings= make_footer_btn(footer, tr("Settings"),     in_settings_cb,   120);
  lv_obj_align(bArm,      LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,    LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,      LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings, LV_ALIGN_LEFT_MID, 408, 0);

  in_btn_arm = bArm; set_arm_button_visual(in_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("Incline");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// Tachometer Screen
// ─────────────────────────────────────────────────────────────────────────────
static lv_obj_t* ta_btn_arm = nullptr;
static void ta_arm_toggle_cb(lv_event_t*)
{
  g_armed = !g_armed;

  // NEW: Set experiment state
  if (g_armed) {
    experiment_set_state(ExperimentState::ARMED);
  } else {
    experiment_set_state(ExperimentState::IDLE);
  }

  set_arm_button_visual(ta_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  experiments_clear_timestamps();
}
static void ta_reset_cb(lv_event_t*)
{
  g_armed = false;
  set_arm_button_visual(ta_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
  lv_label_set_text(g_val_label, "RPM=0.0");
  lv_label_set_text(g_formula_label, "");
  lv_obj_invalidate(g_val_label);
  lv_obj_invalidate(g_formula_label);
  experiments_clear_history("Tachometer");
  update_history("Tachometer");
}

static void ta_export_cb(lv_event_t*)
{
    if (!chronos_sd_begin()) {
        Serial.println("[Export][Tacho] SD init failed");
        show_export_toast(false, "");
        return;
    }

    String path = chronos::exportfs_save_csv("Tachometer", experiments_emit_csv);

    if (path.length()) {
        Serial.printf("[Export][Tacho] saved: %s\n", path.c_str());
        show_export_toast(true, path.c_str());

        // Reset Tachometer after successful export
        g_armed = false;
        set_arm_button_visual(ta_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));
        if (g_val_label) lv_label_set_text(g_val_label, "0.0 RPM");
        if (g_formula_label) lv_label_set_text(g_formula_label, "");
        experiments_clear_history("Tachometer");
        update_history("Tachometer");
    } else {
        Serial.println("[Export][Tacho] save failed");
        show_export_toast(false, "");
    }
}


// [2026-01-18 21:24 CET] ADD: Tachometer pulse simulation + settings callback (linker fix)
// [2026-01-26 21:10 CET] FIX: Tachometer simulation now records RPM & repaints UI
static void sim_ta_pulse(lv_event_t* /*e*/)
{
    if (!g_sim_enabled || !g_armed) return;

    // Single-channel pulse on A
    gate_simulate_gate_a();

    double rpm = 0.0, period_ms = 0.0;
    std::string formula;
    if (experiments_record_tacho(rpm, period_ms, formula)) {
        char buf[64]; snprintf(buf, sizeof(buf), "RPM=%.1f", rpm);
        if (g_val_label)      { lv_label_set_text(g_val_label, buf);               lv_obj_invalidate(g_val_label); }
        if (g_formula_label)  { lv_label_set_text(g_formula_label, formula.c_str()); lv_obj_invalidate(g_formula_label); }
        update_history("Tachometer");
    }
}


static void ta_settings_cb(lv_event_t* /*e*/)
{
    clear_history_for_current_screen();
    gui_show_tacho_settings();
}


void gui_show_tacho()
{
  g_experiment_screen_active = true;  // Enable button animation
  g_current_screen = CurrentScreen::Tachometer;
  header_stop_and_clear();
  load_general_prefs();
  g_armed = false;

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Tachometer"));

  int topOffset = HEADER_H;
  // ALWAYS show simulation button (animate on real gate events)
  lv_obj_t* sim_row = lv_obj_create(scr);
  lv_obj_set_size(sim_row, SCREEN_WIDTH, SIMROW_H);
  lv_obj_align(sim_row, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(sim_row, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(sim_row, LV_SCROLLBAR_MODE_OFF);

  // Tachometer only uses Gate A (single pulse input)
  g_sim_btn_a = make_sim_btn(sim_row, tr("Pulse A"), sim_ta_pulse);
  lv_obj_align(g_sim_btn_a, LV_ALIGN_LEFT_MID, 20, 0);
  g_sim_btn_b = nullptr;  // Tachometer doesn't use Gate B
  topOffset += SIMROW_H;
  

  int contentH = SCREEN_HEIGHT - topOffset - FOOTER_H;
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH - HISTORY_W, contentH);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, topOffset);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  // ⬇️ Make results content non-scrollable
  lv_obj_set_scroll_dir(content, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

  build_measurement_labels(content, "RPM=0.0");

  lv_obj_t* footer = lv_obj_create(scr);
  lv_obj_set_size(footer, SCREEN_WIDTH - HISTORY_W, FOOTER_H);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, CLR_FOOTER(), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_80, 0);
  
    // ⬇️ Make footer non-scrollable
  lv_obj_set_scroll_dir(footer, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* bArm     = make_footer_btn(footer, tr("Start / Arm"), ta_arm_toggle_cb, 120);
  lv_obj_t* bReset   = make_footer_btn(footer, tr("Reset"),        ta_reset_cb,      120);
  lv_obj_t* bCSV     = make_footer_btn(footer, tr("Export CSV"),   ta_export_cb,     120);
  lv_obj_t* bSettings= make_footer_btn(footer, tr("Settings"),     ta_settings_cb,   120);
  lv_obj_align(bArm,      LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_align(bReset,    LV_ALIGN_LEFT_MID, 144, 0);
  lv_obj_align(bCSV,      LV_ALIGN_LEFT_MID, 276, 0);
  lv_obj_align(bSettings, LV_ALIGN_LEFT_MID, 408, 0);

  ta_btn_arm = bArm; set_arm_button_visual(ta_btn_arm, g_armed, tr("Armed"), tr("Start / Arm"));

  g_history_panel = lv_obj_create(scr);
  lv_obj_set_size(g_history_panel, HISTORY_W, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(g_history_panel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(g_history_panel, lv_color_hex(0x202030), 0);
  lv_obj_set_style_bg_opa(g_history_panel, LV_OPA_80, 0);
  update_history("Tachometer");

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
    // Start button animation timer
  
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Menu
// ─────────────────────────────────────────────────────────────────────────────
enum class MenuTarget : uint8_t { Stopwatch=0, CV, Photogate, UA, FreeFall, Incline, Tachometer };
static void menu_nav_cb(lv_event_t* e)
{
  // 1. Start transition (blocks gate polling)
  g_screen_transition_active = true;
  
  // 2. Disarm experiment immediately
  g_armed = false;
  experiment_set_state(ExperimentState::IDLE);
  
  // 3. Clear history
  clear_history_for_current_screen();
  
  // 4. Give I²C time to settle
  delay(50);
  
  // 5. Navigate
  MenuTarget tgt = (MenuTarget)(uintptr_t)lv_event_get_user_data(e);
  switch (tgt) {
    case MenuTarget::Stopwatch: gApp.startIndex = (uint8_t)AppMode::Stopwatch; break;
    case MenuTarget::CV:        gApp.startIndex = (uint8_t)AppMode::CV;        break;
    case MenuTarget::Photogate: gApp.startIndex = (uint8_t)AppMode::Photogate; break;
    case MenuTarget::UA:        gApp.startIndex = (uint8_t)AppMode::UA;        break;
    case MenuTarget::FreeFall:  gApp.startIndex = (uint8_t)AppMode::FreeFall;  break;
    case MenuTarget::Incline:   gApp.startIndex = (uint8_t)AppMode::Incline;   break;
    case MenuTarget::Tachometer:gApp.startIndex = (uint8_t)AppMode::Tacho;     break;
  }
  
  Serial.printf("[menu_nav_cb] startIndex=%u (transition active)\n", (unsigned)gApp.startIndex);
  gApp.enter_mode();
  
  // 6. Let screen settle
  delay(50);
  
  // 7. End transition (re-enable gate polling if armed)
  g_screen_transition_active = false;
  Serial.println("[menu_nav_cb] Transition complete");
}

static lv_obj_t* make_menu_btn(lv_obj_t* parent, const char* text, MenuTarget tgt)
{
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, MENU_BTN_W, MENU_BTN_H);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, menu_nav_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)(uint8_t)tgt);
  return btn;
}


// [2026-01-18 14:20 CET] NEW: Launch Export AP (AP+STA) for Web Export UI
static void on_wifi_export_cb(lv_event_t* e)
{
    (void)e;

    // Start dual-mode AP+STA without disrupting STA
    // apweb_begin moved to on_sw_ap_changed (AP modal)
    Serial.println("[GUI] WiFi Export AP started (AP+STA mode)");

    // Optional: small toast message on screen
    lv_obj_t* toast = lv_label_create(lv_scr_act());
    lv_label_set_text(toast, "WiFi Export Ready: Connect to Chronos-AP");
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    lv_obj_set_style_text_font(toast, FONT_LABEL, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Auto-hide toast after 3 seconds
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            lv_obj_del((lv_obj_t*)t->user_data);
            lv_timer_del(t);
        },
        3000,
        toast
    );
}


// [2026-01-18 14:20 CET] UPDATED: Main Menu now includes "WiFi Export" button
void gui_show_main_menu()
{

    // Stop any running experiments
    g_armed = false;
    experiment_set_state(ExperimentState::IDLE);

      // Stop animation timer
    g_experiment_screen_active = false;  // Disable button animation
    g_current_screen = CurrentScreen::MainMenu;
    header_stop_and_clear();

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_header(scr, tr("Main Menu"));

    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    // Not scrollable
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    // Flex grid for menu buttons
    lv_obj_t* grid = lv_obj_create(content);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 20, 0);
    lv_obj_set_style_pad_all(grid, 8, 0);

    // Standard experiment icons/buttons
    (void)make_menu_btn(grid, tr("Stopwatch"),    MenuTarget::Stopwatch);
    (void)make_menu_btn(grid, tr("Linear Motion (CV)"), MenuTarget::CV);
    (void)make_menu_btn(grid, tr("Photogate Speed"), MenuTarget::Photogate);
    (void)make_menu_btn(grid, tr("Uniform Acceleration"), MenuTarget::UA);
    (void)make_menu_btn(grid, tr("Free Fall"), MenuTarget::FreeFall);
    (void)make_menu_btn(grid, tr("Inclined Plane"), MenuTarget::Incline);
    (void)make_menu_btn(grid, tr("Tachometer"), MenuTarget::Tachometer);

    // Show screen
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}


// ─────────────────────────────────────────────────────────────────────────────
// Wifi Settings (placeholder)
// ─────────────────────────────────────────────────────────────────────────────



// [2026-01-18 14:58 CET] NEW: WiFi Export Controls Screen
void gui_show_wifi_export()
{
     
    g_current_screen = CurrentScreen::WifiSettings; // reuse enum
    header_stop_and_clear();
    lv_obj_t* scr = lv_obj_create(NULL);

    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    build_header(scr, "WiFi Export");

    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    // Title
    lv_obj_t* lbl1 = lv_label_create(content);
    lv_label_set_text(lbl1, "Download experiment results via WiFi:");
    lv_obj_set_style_text_color(lbl1, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl1, FONT_LABEL, 0);
    lv_obj_align(lbl1, LV_ALIGN_TOP_LEFT, 20, 20);

    // Instructions
    lv_obj_t* instr = lv_label_create(content);
    lv_label_set_text(instr,
        "1. Connect to WiFi:  Chronos-AP\n"
        "2. Password:         chronos123\n"
        "3. Open browser:     http://192.168.4.1\n\n"
        "Here you can download CSV and ZIP files.");
    lv_obj_set_style_text_color(instr, lv_color_white(), 0);
    lv_obj_set_style_text_font(instr, FONT_SMALL, 0);
    lv_obj_align(instr, LV_ALIGN_TOP_LEFT, 20, 70);

    // Start AP button
    lv_obj_t* bStart = lv_btn_create(content);
    lv_obj_set_size(bStart, 260, 60);
    lv_obj_set_style_radius(bStart, 10, 0);
    lv_obj_set_style_bg_color(bStart, lv_color_hex(0x2EBF6D), 0);
    lv_obj_align(bStart, LV_ALIGN_TOP_LEFT, 20, 220);
    lv_obj_t* sLbl = lv_label_create(bStart);
    lv_label_set_text(sLbl, "Start WiFi Export");
    lv_obj_center(sLbl);
    lv_obj_add_event_cb(bStart, [](lv_event_t*){
        gui_ap_start();
    }, LV_EVENT_CLICKED, NULL);

    // Stop AP button
    lv_obj_t* bStop = lv_btn_create(content);
    lv_obj_set_size(bStop, 260, 60);
    lv_obj_set_style_radius(bStop, 10, 0);
    lv_obj_set_style_bg_color(bStop, lv_color_hex(0xFF6060), 0);
    lv_obj_align(bStop, LV_ALIGN_TOP_LEFT, 300, 220);
    lv_obj_t* pLbl = lv_label_create(bStop);
    lv_label_set_text(pLbl, "Stop WiFi Export");
    lv_obj_center(pLbl);
    lv_obj_add_event_cb(bStop, [](lv_event_t*){
        gui_ap_stop();
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings (General + Wifi + Screensaver)
// ─────────────────────────────────────────────────────────────────────────────
static void on_wifi_settings(lv_event_t* e) { (void)e; gui_show_wifi_settings(); }

// Wi‑Fi master alignment kept (Step‑1)

// [2026-01-18 15:45 CET] UPDATED: on_sw_wifi_changed()
// - Persists master Wi-Fi flag and powers radio accordingly (as before)
// - Updates unified net icon color (no separate Wi-Fi/LED widgets anymore)
// [2026-01-18 16:15 CET] UPDATED: on_sw_wifi_changed()
// - Persists master Wi-Fi flag; powers radio accordingly; recolors unified icon.
// [2026-01-18 22:16 CET] FIX: Keep radio up in AP+STA when AP is running
// [2026-01-18 22:20 CET] UPDATED: keep AP alive when Wi‑Fi selector is OFF


// [2026-01-18 15:58 CET] NEW: WebExport (AP) toggle handler in Settings.
// Turns the Export AP ON/OFF and updates the unified net icon + AP badge.
// [2026-01-18 16:15 CET] UPDATED: WebExport (AP) toggle in Settings.
// Turns the Export AP ON/OFF and updates unified net icon + tiny badge.



// Single, canonical handler for the Language dropdown.
static void on_dd_lang_changed(lv_event_t* e)
{
  lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
  uint16_t idx = lv_dropdown_get_selected(dd);
  Language lang = LANG_EN;
  switch (idx) {
    case 0: lang = LANG_EN; break; // English
    case 1: lang = LANG_HR; break; // Hrvatski
    case 2: lang = LANG_DE; break; // Deutsch
    case 3: lang = LANG_FR; break; // Français
    case 4: lang = LANG_ES; break; // Español
    default: lang = LANG_EN; break;
  }
  // Persist to NVS via i18n (writes "lang" in Preferences)
  i18n_set_language(lang);
  // If you use runtime YAML translations and want immediate updates:
  // i18n_reload();
  // Rebuild the current screen so all labels repaint now
  screensaver_refresh_language();
  gui_refresh_active_screen();
  // Optional toast for feedback
  lv_obj_t* toast = lv_label_create(lv_obj_get_parent(dd));
  lv_label_set_text(toast, tr("Language changed"));
  lv_obj_set_style_text_color(toast, lv_color_white(), 0);
  lv_obj_set_style_text_font(toast, FONT_LABEL, 0);
  lv_obj_align(toast, LV_ALIGN_TOP_LEFT, 320, 90);
}

static void on_sw_sim_changed(lv_event_t* e)
{
  lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  save_sim_enabled(on);
}

// ── NEW: Screensaver setting handlers ────────────────────────────────────────
static void on_kb_ready_ss(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  uint32_t s = (uint32_t)atoi(get_effective_ta_text_for_ready(kb, ta)); // seconds; 0 disables
  Preferences prefs;
  if (prefs.begin("display", false)) { prefs.putUInt("ss_to_s", s); prefs.end(); }
  gScreenSaverTimeoutS = s;
  gLastUserActivityMs  = millis();
  if (gScreenSaverActive) { hal::backlight_on(); gScreenSaverActive = false; }
  end_edit_session();
  gui_show_settings();
}

static void on_ss_save_click(lv_event_t* e)
{
  (void)e;
  lv_obj_t* content = lv_obj_get_parent(lv_event_get_target(e));
  lv_obj_t* ta = nullptr;
  uint32_t childCnt = lv_obj_get_child_cnt(content);
  for (uint32_t i = 0; i < childCnt; ++i) {
    lv_obj_t* c = lv_obj_get_child(content, i);
    if (lv_obj_check_type(c, &lv_textarea_class)) { ta = c; break; }
  }
  if (!ta) return;
  uint32_t s = (uint32_t)atoi(get_effective_ta_text_for_save(ta)); // 0 disables
  Preferences prefs;
  if (prefs.begin("display", false)) { prefs.putUInt("ss_to_s", s); prefs.end(); }
  gScreenSaverTimeoutS = s;
  gLastUserActivityMs  = millis();
  if (gScreenSaverActive) { hal::backlight_on(); gScreenSaverActive = false; }
  end_edit_session();
  gui_show_settings();
}


// [2026-01-18 22:38 CET] NEW: Network Mode dropdown handler
// Modes: 0=None, 1=Wi‑Fi (STA), 2=AP only, 3=Wi‑Fi+AP





// [2026-01-18 15:58 CET] UPDATED: Settings screen
// - Keeps original rows (Wifi, Wifi Settings, Language, Simulation, Screensaver, Date&Time)
// - ADDS a "WiFi Export (AP)" row with a switch that reflects g_ap_running and toggles AP.
// - Unified net icon coloring + AP badge are driven by on_sw_wifi_changed / on_sw_ap_changed.
void gui_show_settings()
{
      // Stop animation timer
    g_experiment_screen_active = false;  // Disable button animation
    g_current_screen = CurrentScreen::Settings;
    header_stop_and_clear();
    load_general_prefs();

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    build_header(scr, tr("Settings"));

    // Content root: full screen minus header, flex column
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_START,  // main
                          LV_FLEX_ALIGN_START,  // cross
                          LV_FLEX_ALIGN_START); // track
    lv_obj_set_style_pad_all (content, 0, 0);
    lv_obj_set_style_pad_row (content, 16, 0);
    lv_obj_set_style_pad_column(content, 0, 0);
    apply_no_borders(content);

    // Shared numeric keyboard (hidden by default)
    lv_obj_t* kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    apply_no_borders(kb);


    // [2026-01-18 22:38 CET] 


    // ─────────────────────────────────────────────────────────
    // Row: Wifi  (switch)  +  Wifi Settings (button)
    // ─────────────────────────────────────────────────────────
    

    // ─────────────────────────────────────────────────────────
    // Row: WiFi Export (AP) — NEW switch that reflects g_ap_running
    // ─────────────────────────────────────────────────────────
    {
        lv_obj_t* row_ap = make_row(content, 60);

        lv_obj_t* lbl = lv_label_create(row_ap);
        lv_label_set_text(lbl, tr("WiFi Export (AP)"));
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

        (void)make_spacer(row_ap);

        lv_obj_t* sw_ap = lv_switch_create(row_ap);
        if (g_ap_running) lv_obj_add_state(sw_ap, LV_STATE_CHECKED);

        // Start/Stop Export AP and update unified icon color + badge
        lv_obj_add_event_cb(sw_ap, on_sw_ap_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // ─────────────────────────────────────────────────────────
    // Row: Language (label left, dropdown right)
    // ─────────────────────────────────────────────────────────
    lv_obj_t* row_lang = make_row(content, 60);
    {
        lv_obj_t* lbl = lv_label_create(row_lang);
        lv_label_set_text(lbl, tr("Language"));
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

        (void)make_spacer(row_lang);

        lv_obj_t* dd = lv_dropdown_create(row_lang);
        std::string dd_opts = "English\nHrvatski\nDeutsch\nFrançais\nEspañol";
        lv_dropdown_set_options(dd, dd_opts.c_str());
        lv_obj_set_size(dd, 220, 60);
        lv_obj_set_style_text_font(dd,   FONT_SMALL, 0);
        if (lv_obj_t* dd_list = lv_dropdown_get_list(dd)) {
            lv_obj_set_style_text_font(dd_list, FONT_SMALL, 0);
        }

        const char* code = i18n_get_lang_code();
        int sel = 0;
        if (!strcmp(code, "en")) sel = 0;
        else if (!strcmp(code, "hr")) sel = 1;
        else if (!strcmp(code, "de")) sel = 2;
        else if (!strcmp(code, "fr")) sel = 3;
        else if (!strcmp(code, "es")) sel = 4;
        lv_dropdown_set_selected(dd, sel);

        lv_obj_add_event_cb(dd, on_dd_lang_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // ─────────────────────────────────────────────────────────
    // Row: Simulation (label left, switch right)
    // ─────────────────────────────────────────────────────────
    lv_obj_t* row_sim = make_row(content, 60);
    {
        lv_obj_t* lbl = lv_label_create(row_sim);
        lv_label_set_text(lbl, tr("Simulation"));
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

        (void)make_spacer(row_sim);

        lv_obj_t* sw = lv_switch_create(row_sim);
        if (g_sim_enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_sw_sim_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // ─────────────────────────────────────────────────────────
    // Row: Screensaver time (label left, numeric field right)
    // Help text is a separate row below
    // ─────────────────────────────────────────────────────────
    lv_obj_t* row_ss = make_row(content, 60);
    lv_obj_t* ta_ss = nullptr;
    {
        lv_obj_t* lbl = lv_label_create(row_ss);
        lv_label_set_text(lbl, tr("Screensaver (s):"));
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

        (void)make_spacer(row_ss);

        char sSS[16];
        snprintf(sSS, sizeof(sSS), "%lu", (unsigned long)gScreenSaverTimeoutS);
        ta_ss = lv_textarea_create(row_ss);
        lv_textarea_set_one_line(ta_ss, true);
        lv_textarea_set_text(ta_ss, sSS);
        lv_textarea_set_placeholder_text(ta_ss, "e.g. 60; 0 disables");
        lv_obj_set_size(ta_ss, 160, 60);

        // numeric keyboard handlers
        lv_obj_add_event_cb(ta_ss, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
        lv_obj_add_event_cb(ta_ss, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
        lv_obj_add_event_cb(scr,   on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);
        lv_obj_add_event_cb(kb,    on_kb_ready_ss, LV_EVENT_READY, ta_ss);
    }
    {
        lv_obj_t* help = lv_label_create(content);
        lv_label_set_text(help, tr("Screensaver time (seconds). Set 0 to disable."));
        lv_obj_set_style_text_color(help, lv_color_white(), 0);
        lv_obj_set_style_text_font(help,  FONT_SMALL, 0);
        lv_obj_set_width(help, SCREEN_WIDTH - (MARGIN_LEFT + MARGIN_RIGHT + 180));
        lv_obj_align(help, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, 0);
    }

    // ─────────────────────────────────────────────────────────
    // Row: Date & Time (wide button)
    // ─────────────────────────────────────────────────────────
    {
        lv_obj_t* row_dt = make_row(content, 60);

        // left filler to keep row height consistent
        lv_obj_t* filler = lv_label_create(row_dt);
        lv_label_set_text(filler, "");

        (void)make_spacer(row_dt);

        static void (*open_dt_cb)(lv_event_t*) = [](lv_event_t* e){
            (void)e;
            clear_history_for_current_screen();
            gui_show_datetime_settings();
        };

        lv_obj_t* btn_dt = make_btn(row_dt, tr("Date & Time"), open_dt_cb);
        lv_obj_set_width(btn_dt, 320);
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}


// ─────────────────────────────────────────────────────────────
// Common keyboard handlers  (UPDATED: no clear; replace on first key)
// ─────────────────────────────────────────────────────────────
static void on_ta_focus(lv_event_t* e)
{
    lv_obj_t* ta = lv_event_get_target(e);
    lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);

    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(kb, ta);

    if (lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_NUMBER) {
        // 1) Remember previous text (for "no edit → keep old")
        const char* prev = lv_textarea_get_text(ta);
        s_old_value[0] = '\0';
        if (prev) { strncpy(s_old_value, prev, sizeof(s_old_value) - 1);
                    s_old_value[sizeof(s_old_value) - 1] = '\0'; }

        // 2) Start edit session
        s_edit_ta   = ta;
        s_edit_made = false;

        // 3) Arm the one‑shot: first keystroke will replace existing text
        s_replace_on_first_key = true;

        // 4) Track subsequent changes (after user starts typing)
        lv_obj_add_event_cb(ta, on_ta_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

        // 5) Also hook the "first insert" event (see handler below)
        lv_obj_add_event_cb(ta, /* callback */ [](lv_event_t* e2){
            if (lv_event_get_code(e2) != LV_EVENT_INSERT) return;
            lv_obj_t* ta2 = lv_event_get_target(e2);
            if (ta2 == s_edit_ta && s_replace_on_first_key && !s_edit_made) {
                // Clear once so the incoming character/text replaces the old value
                lv_textarea_set_text(ta2, "");
                s_replace_on_first_key = false;
                // After this event returns, LVGL inserts the user's key normally
            }
        }, LV_EVENT_INSERT, NULL);
    }
}

void gui_show_cv_settings()
{
   
  g_experiment_screen_active = false;  // Disable button animation
  g_current_screen = CurrentScreen::CVSettings;  // Fixed: no underscore
  header_stop_and_clear();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("CV Settings"));

  // Content area
  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(content, 20, 0);
  lv_obj_set_style_pad_row(content, 16, 0);
  apply_no_borders(content);

  // Distance Between Gates setting
  lv_obj_t* row_dist = make_row(content, 60);
  {
    lv_obj_t* lbl = lv_label_create(row_dist);
    lv_label_set_text(lbl, tr("Distance Between Gates (mm)"));
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

    make_spacer(row_dist);

    uint16_t dist_mm = experiments_get_distance_mm();  // Fixed function name
    lv_obj_t* ta = lv_textarea_create(row_dist);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 5);
    lv_obj_set_width(ta, 120);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", dist_mm);
    lv_textarea_set_text(ta, buf);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    
    // Store textarea reference for saving
    lv_obj_set_user_data(row_dist, ta);
  }

  // Save button
  lv_obj_t* btn_save = lv_btn_create(content);
  lv_obj_set_size(btn_save, 200, 60);
  lv_obj_set_style_radius(btn_save, 10, 0);
  lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x2EBF6D), 0);
  lv_obj_align(btn_save, LV_ALIGN_CENTER, 0, 0);
  
  lv_obj_t* lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, tr("Save"));
  lv_obj_set_style_text_font(lbl_save, FONT_LABEL, 0);
  lv_obj_set_style_text_color(lbl_save, lv_color_white(), 0);
  lv_obj_center(lbl_save);
  
  // Save callback - store the textarea pointer
  lv_obj_set_user_data(btn_save, row_dist);
  lv_obj_add_event_cb(btn_save, [](lv_event_t* e) {
    lv_obj_t* row = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* ta = (lv_obj_t*)lv_obj_get_user_data(row);
    
    const char* txt = lv_textarea_get_text(ta);
    uint16_t val = (uint16_t)atoi(txt);
    if (val > 0 && val <= 10000) {  // Sanity check: 1mm to 10m
      experiments_set_distance_mm(val);  // Fixed function name
      Serial.printf("[CV Settings] Distance set to %u mm\n", val);
      gui_show_cv();  // Return to CV screen
    } else {
      Serial.println("[CV Settings] Invalid distance value");
    }
  }, LV_EVENT_CLICKED, nullptr);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}


// ─────────────────────────────────────────────────────────────────────────────
// CV Settings
// ─────────────────────────────────────────────────────────────────────────────
static void on_kb_ready_cv(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  const char* txt = get_effective_ta_text_for_ready(kb, ta);
  uint16_t mm = (uint16_t)atoi(txt); if (mm < 10) mm = 10;
  experiments_set_distance_mm(mm);
  end_edit_session();
  gui_show_cv();
}

static void on_cv_save_click(lv_event_t* e)
{
  (void)e;
  if (!g_cv_ta_dist) return;
  const char* txt = get_effective_ta_text_for_save(g_cv_ta_dist);
  uint16_t mm = (uint16_t)atoi(txt); if (mm < 10) mm = 10;
  experiments_set_distance_mm(mm);
  end_edit_session();
  gui_show_cv();
}



// ─────────────────────────────────────────────────────────────────────────────
// Photogate Settings
// ─────────────────────────────────────────────────────────────────────────────
static void on_kb_ready_pg(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  const char* txt = get_effective_ta_text_for_ready(kb, ta);
  uint16_t mm = (uint16_t)atoi(txt); if (mm < 1) mm = 1;
  experiments_set_flag_mm(mm);
  end_edit_session();
  gui_show_photogate();
}

static void on_pg_save_click(lv_event_t* e)
{
  (void)e;
  if (!g_pg_ta_flag) return;
  const char* txt = get_effective_ta_text_for_save(g_pg_ta_flag);
  uint16_t mm = (uint16_t)atoi(txt); if (mm < 1) mm = 1;
  experiments_set_flag_mm(mm);
  end_edit_session();
  gui_show_photogate();
}

void gui_show_pg_settings()
{
    // Stop animation timer
  g_current_screen = CurrentScreen::PGSettings;
  header_stop_and_clear();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Photogate Settings"));

  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* cap = make_caption(content, tr("Flag length (mm):"));
  lv_obj_align(cap, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0);

  char init_flag[12]; snprintf(init_flag, sizeof(init_flag), "%u", (unsigned)experiments_get_flag_mm());
  lv_obj_t* ta = lv_textarea_create(content);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_text(ta, init_flag);
  lv_textarea_set_placeholder_text(ta, "e.g. 50");
  lv_obj_set_size(ta, FIELD_W, FIELD_H);
  lv_obj_align(ta, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, CAPTION_Y0);
  make_help(content, tr("Length of the object's edge that blocks the light beam."), cap);

  lv_obj_add_event_cb(ta, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
  lv_obj_add_event_cb(ta, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
  lv_obj_add_event_cb(scr, on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(kb, on_kb_ready_pg, LV_EVENT_READY, ta);

  g_pg_ta_flag = ta;

  lv_obj_t* save = make_btn(content, tr("Save"), on_pg_save_click);
  lv_obj_align(save, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0 + BLOCK_SPACING);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// UA Settings (Step‑2: L only)
// ─────────────────────────────────────────────────────────────────────────────
static void on_kb_ready_ua_len(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kbLen = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* taLen = (lv_obj_t*)lv_event_get_user_data(e);
  const char* txt = get_effective_ta_text_for_ready(kbLen, taLen);
  uint16_t len = (uint16_t)atoi(txt);
  if (len < 1) len = 1;
  experiments_set_ua_len_mm(len);
  end_edit_session();
  gui_show_ua();
}

static void on_ua_len_save_click(lv_event_t* e)
{
  (void)e;
  if (!g_ua_ta0) return; // g_ua_ta0 -> length field
  const char* txt = get_effective_ta_text_for_save(g_ua_ta0);
  uint16_t len = (uint16_t)atoi(txt);
  if (len < 1) len = 1;
  experiments_set_ua_len_mm(len);
  end_edit_session();
  gui_show_ua();
}

void gui_show_ua_settings()
{
    // Stop animation timer
  g_current_screen = CurrentScreen::UASettings;
  header_stop_and_clear();
  uint16_t objLen = experiments_get_ua_len_mm();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("UA Settings"));

  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  // Only one field: Object length (mm)
  lv_obj_t* capLen = make_caption(content, tr("Object length (mm):"));
  lv_obj_align(capLen, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0);
  char sL[12]; snprintf(sL, sizeof(sL), "%u", (unsigned)objLen);
  lv_obj_t* taLen = lv_textarea_create(content);
  lv_textarea_set_one_line(taLen, true);
  lv_textarea_set_text(taLen, sL);
  lv_textarea_set_placeholder_text(taLen, "e.g. 50");
  lv_obj_set_size(taLen, FIELD_W, FIELD_H);
  lv_obj_align(taLen, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, CAPTION_Y0);
  make_help(content, tr("Blocking edge length."), capLen);

  lv_obj_add_event_cb(taLen, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
  lv_obj_add_event_cb(taLen, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
  lv_obj_add_event_cb(scr,   on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(kb,    on_kb_ready_ua_len,   LV_EVENT_READY,   taLen);

  g_ua_ta0 = taLen; // length
  g_ua_ta1 = nullptr;
  g_ua_ta2 = nullptr;

  lv_obj_t* save = make_btn(content, tr("Save"), on_ua_len_save_click);
  lv_obj_align(save, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0 + BLOCK_SPACING);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Free Fall Settings
// ─────────────────────────────────────────────────────────────────────────────
static void on_kb_ready_ff(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t** arr = (lv_obj_t**)lv_event_get_user_data(e);
  const char* txtL = get_effective_ta_text_for_ready(kb, arr[0]);
  const char* txtH = get_effective_ta_text_for_ready(kb, arr[1]);
  uint16_t len = (uint16_t)atoi(txtL);
  uint16_t h   = (uint16_t)atoi(txtH);
  if (len < 1) len = 1; if (h < 1) h = 1;
  experiments_set_freefall_params(len, h);
  end_edit_session();
  gui_show_freefall();
}

static void on_ff_save_click(lv_event_t* e)
{
  (void)e;
  if (!g_ua_ta0 || !g_ua_ta1) return;
  const char* txtL = get_effective_ta_text_for_save(g_ua_ta0);
  const char* txtH = get_effective_ta_text_for_save(g_ua_ta1);
  uint16_t len = (uint16_t)atoi(txtL);
  uint16_t h   = (uint16_t)atoi(txtH);
  if (len < 1) len = 1; if (h < 1) h = 1;
  experiments_set_freefall_params(len, h);
  end_edit_session();
  gui_show_freefall();
}

void gui_show_freefall_settings()
{
    // Stop animation timer
  g_current_screen = CurrentScreen::FFSettings;
  header_stop_and_clear();
  uint16_t len, h; experiments_get_freefall_params(len, h);

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Free Fall Settings"));

  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  int y0 = CAPTION_Y0;
  int y1 = CAPTION_Y0 + BLOCK_SPACING;

  lv_obj_t* capLen = make_caption(content, tr("Object length (mm):"));
  lv_obj_align(capLen, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, y0);
  char sL[12]; snprintf(sL, sizeof(sL), "%u", (unsigned)len);
  lv_obj_t* taLen = lv_textarea_create(content);
  lv_textarea_set_one_line(taLen, true);
  lv_textarea_set_text(taLen, sL);
  lv_textarea_set_placeholder_text(taLen, "e.g. 50");
  lv_obj_set_size(taLen, FIELD_W, FIELD_H);
  lv_obj_align(taLen, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, y0);
  make_help(content, tr("Blocking edge length."), capLen);

  lv_obj_t* capH = make_caption(content, tr("Drop height (mm):"));
  lv_obj_align(capH, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, y1);
  char sH[12]; snprintf(sH, sizeof(sH), "%u", (unsigned)h);
  lv_obj_t* taH = lv_textarea_create(content);
  lv_textarea_set_one_line(taH, true);
  lv_textarea_set_text(taH, sH);
  lv_textarea_set_placeholder_text(taH, "e.g. 500");
  lv_obj_set_size(taH, FIELD_W, FIELD_H);
  lv_obj_align(taH, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, y1);
  make_help(content, tr("Height from release to gate A."), capH);

  lv_obj_add_event_cb(taLen, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
  lv_obj_add_event_cb(taLen, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
  lv_obj_add_event_cb(taH,   on_ta_focus,   LV_EVENT_FOCUSED,   kb);
  lv_obj_add_event_cb(taH,   on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
  lv_obj_add_event_cb(scr,   on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);
  static lv_obj_t* ta_arr2[2]; ta_arr2[0] = taLen; ta_arr2[1] = taH;
  lv_obj_add_event_cb(kb, on_kb_ready_ff, LV_EVENT_READY, ta_arr2);

  g_ua_ta0 = taLen; g_ua_ta1 = taH;

  lv_obj_t* save = make_btn(content, tr("Save"), on_ff_save_click);
  lv_obj_align(save, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0 + 2*BLOCK_SPACING);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Inclined Plane Settings
// ─────────────────────────────────────────────────────────────────────────────

// [2026-01-18 21:02 CET] FIX: angle is integer (degrees); parse with atoi and pass uint16_t
// [2026-01-18 21:12 CET] FIX: angle is entered as integer (deg), but API wants float
// [2026-01-18 21:28 CET] UPDATED: on_kb_ready_in — UI integer angle → API float
static void on_kb_ready_in(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;

    lv_obj_t* kb   = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t** arr = (lv_obj_t**)lv_event_get_user_data(e);

    const char* txtL = get_effective_ta_text_for_ready(kb, arr[0]);
    const char* txtD = get_effective_ta_text_for_ready(kb, arr[1]);
    const char* txtA = get_effective_ta_text_for_ready(kb, arr[2]);

    uint16_t len  = (uint16_t)atoi(txtL); if (len  < 1) len  = 1;
    uint16_t dist = (uint16_t)atoi(txtD); if (dist < 1) dist = 1;
    uint16_t angU = (uint16_t)atoi(txtA);                     // UI integer degrees

    // API wants float for the angle — cast here
    experiments_set_incline_params(len, dist, (float)angU);

    end_edit_session();
    gui_show_incline();
}





// [2026-01-18 21:02 CET] FIX: angle is integer (degrees); parse with atoi and pass uint16_t
// [2026-01-18 21:12 CET] FIX: cast integer angle to float when saving
// [2026-01-18 21:28 CET] UPDATED: on_in_save_click — UI integer angle → API float
static void on_in_save_click(lv_event_t* e)
{
    (void)e;
    // Ensure textareas are available
    if (!g_ua_ta0 || !g_ua_ta1 || !g_ua_ta2) return;

    const char* txtL = get_effective_ta_text_for_save(g_ua_ta0);
    const char* txtD = get_effective_ta_text_for_save(g_ua_ta1);
    const char* txtA = get_effective_ta_text_for_save(g_ua_ta2);

    uint16_t len  = (uint16_t)atoi(txtL); if (len  < 1) len  = 1;
    uint16_t dist = (uint16_t)atoi(txtD); if (dist < 1) dist = 1;
    uint16_t angU = (uint16_t)atoi(txtA);                     // UI integer degrees

    // API wants float for the angle — cast here
    experiments_set_incline_params(len, dist, (float)angU);

    end_edit_session();
    gui_show_incline();
}





// [2026-01-18 21:02 CET] FIX: use integer angle (degrees) in UI and formatting

// [2026-01-18 21:12 CET] FIX: get float angle from API, show as integer in the UI

// [2026-01-18 21:28 CET] UPDATED: gui_show_incline_settings — show integer angle, keep API float
void gui_show_incline_settings()
{
      // Stop animation timer
    g_current_screen = CurrentScreen::INSettings;
    header_stop_and_clear();

    // API signature: experiments_get_incline_params(uint16_t&, uint16_t&, float&)
    uint16_t len = 0, dist = 0;
    float    angF = 0.0f;
    experiments_get_incline_params(len, dist, angF);   // angle comes as float from API

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_header(scr, tr("Inclined Plane Settings"));

    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    // Shared numeric keyboard (hidden by default)
    lv_obj_t* kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    int y0 = CAPTION_Y0;
    int y1 = CAPTION_Y0 + BLOCK_SPACING;
    int y2 = CAPTION_Y0 + 2 * BLOCK_SPACING;

    // --- L (mm) -------------------------------------------------------------
    lv_obj_t* capLen = make_caption(content, tr("Object length (mm):"));
    lv_obj_align(capLen, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, y0);

    char sL[12]; snprintf(sL, sizeof(sL), "%u", (unsigned)len);
    lv_obj_t* taLen = lv_textarea_create(content);
    lv_textarea_set_one_line(taLen, true);
    lv_textarea_set_text(taLen, sL);
    lv_textarea_set_placeholder_text(taLen, "e.g. 50");
    lv_obj_set_size(taLen, FIELD_W, FIELD_H);
    lv_obj_align(taLen, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, y0);
    make_help(content, tr("Blocking edge length."), capLen);

    // --- D (mm) -------------------------------------------------------------
    lv_obj_t* capDist = make_caption(content, tr("Gate distance (mm):"));
    lv_obj_align(capDist, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, y1);

    char sD[12]; snprintf(sD, sizeof(sD), "%u", (unsigned)dist);
    lv_obj_t* taDist = lv_textarea_create(content);
    lv_textarea_set_one_line(taDist, true);
    lv_textarea_set_text(taDist, sD);
    lv_textarea_set_placeholder_text(taDist, "e.g. 500");
    lv_obj_set_size(taDist, FIELD_W, FIELD_H);
    lv_obj_align(taDist, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, y1);
    make_help(content, tr("Distance between the beams."), capDist);

    // --- Angle (deg) — show as integer in UI, keep float in API ------------
    lv_obj_t* capA = make_caption(content, tr("Incline angle (deg):"));
    lv_obj_align(capA, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, y2);

    // Round float to nearest integer for display; avoid <math.h> by simple +0.5f
    unsigned angU = (unsigned)(angF + 0.5f);
    char sA[12]; snprintf(sA, sizeof(sA), "%u", angU);

    lv_obj_t* taA = lv_textarea_create(content);
    lv_textarea_set_one_line(taA, true);
    lv_textarea_set_text(taA, sA);
    lv_textarea_set_placeholder_text(taA, "e.g. 10");
    lv_obj_set_size(taA, FIELD_W, FIELD_H);
    lv_obj_align(taA, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, y2);
    make_help(content, tr("Ramp angle (for theory/compare)."), capA);

    // Numeric TA handlers
    lv_obj_add_event_cb(taLen,  on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taLen,  on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taDist, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taDist, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taA,    on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taA,    on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(scr,    on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);

    // READY on keyboard: integer UI → float API (handled in on_kb_ready_in)
    static lv_obj_t* ta_arr3[3];
    ta_arr3[0] = taLen; ta_arr3[1] = taDist; ta_arr3[2] = taA;
    lv_obj_add_event_cb(kb, on_kb_ready_in, LV_EVENT_READY, ta_arr3);

    // Store for explicit "Save" path
    g_ua_ta0 = taLen; g_ua_ta1 = taDist; g_ua_ta2 = taA;

    // Save button (explicit save path uses on_in_save_click)
    lv_obj_t* save = make_btn(content, tr("Save"), on_in_save_click);
    lv_obj_align(save, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0 + 3 * BLOCK_SPACING);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}



// ─────────────────────────────────────────────────────────────────────────────
// Tachometer Settings
// ─────────────────────────────────────────────────────────────────────────────
static void on_kb_ready_ta(lv_event_t* e)
{
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  const char* txt = get_effective_ta_text_for_ready(kb, ta);
  uint16_t slots = (uint16_t)atoi(txt);
  if (slots < 1) slots = 1;
  experiments_set_tacho_params(slots);
  end_edit_session();
  gui_show_tacho();
}

static void on_ta_save_click(lv_event_t* e)
{
  (void)e;
  if (!g_ua_ta0) return; // reuse g_ua_ta0 to store the slots field
  const char* txt = get_effective_ta_text_for_save(g_ua_ta0);
  uint16_t slots = (uint16_t)atoi(txt);
  if (slots < 1) slots = 1;
  experiments_set_tacho_params(slots);
  end_edit_session();
  gui_show_tacho();
}

void gui_show_tacho_settings()
{
    // Stop animation timer
  g_current_screen = CurrentScreen::TASettings;
  header_stop_and_clear();
  uint16_t slots = experiments_get_tacho_slots();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  build_header(scr, tr("Tachometer Settings"));

  lv_obj_t* content = lv_obj_create(scr);
  lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* capS = make_caption(content, tr("Slots (per revolution):"));
  lv_obj_align(capS, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0);

  char sS[12]; snprintf(sS, sizeof(sS), "%u", (unsigned)slots);
  lv_obj_t* taS = lv_textarea_create(content);
  lv_textarea_set_one_line(taS, true);
  lv_textarea_set_text(taS, sS);
  lv_textarea_set_placeholder_text(taS, "e.g. 60");
  lv_obj_set_size(taS, FIELD_W, FIELD_H);
  lv_obj_align(taS, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, CAPTION_Y0);
  make_help(content, tr("Number of slots cut in the disk."), capS);

  lv_obj_add_event_cb(taS, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
  lv_obj_add_event_cb(taS, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
  lv_obj_add_event_cb(scr, on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(kb,  on_kb_ready_ta, LV_EVENT_READY, taS);

  g_ua_ta0 = taS; // store pointer so on_ta_save_click can read the value
  lv_obj_t* save = make_btn(content, tr("Save"), on_ta_save_click);
  lv_obj_align(save, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0 + BLOCK_SPACING);

  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Date & Time Settings (RTC + NTP)
// ─────────────────────────────────────────────────────────────────────────────

static void on_sw_autosync_changed(lv_event_t* e);
static void on_kb_ready_dt(lv_event_t* e);
static void on_dt_save_click(lv_event_t* e);

void gui_show_datetime_settings()
{
      // Stop animation timer
    g_current_screen = CurrentScreen::DTSettings;
    header_stop_and_clear();

    // Read current time and autosync state
    struct tm now{};
    rtc_get_time(now);
    const bool autosync = rtc_get_auto_sync();

    // Root screen
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(scr, CLR_BG(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    build_header(scr, tr("Date & Time"));

    // Content area (non-scrollable)
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    // Shared numeric keyboard (hidden by default)
    lv_obj_t* kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // ── Row 1: Auto sync — label LEFT, switch RIGHT ─────────────────────────
    lv_obj_t* lblAuto = lv_label_create(content);
    lv_label_set_text(lblAuto, tr("Automatic sync via Internet"));
    lv_obj_set_style_text_color(lblAuto, lv_color_white(), 0);
    lv_obj_set_style_text_font(lblAuto, FONT_LABEL, 0);
    lv_obj_align(lblAuto, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, CAPTION_Y0);

    lv_obj_t* swAuto = lv_switch_create(content);
    if (autosync) lv_obj_add_state(swAuto, LV_STATE_CHECKED);
    lv_obj_align(swAuto, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, CAPTION_Y0 - 2);
    lv_obj_add_event_cb(swAuto, on_sw_autosync_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    // ── Compute display values ONCE, honoring presets ───────────────────────
    int yyyy = now.tm_year + 1900;
    int mm   = now.tm_mon + 1;
    int dd   = now.tm_mday;
    int hh   = now.tm_hour;
    int mi   = now.tm_min;
    int ss   = now.tm_sec;

    // If the previous READY saved presets, repaint with exactly what the user entered
    if (g_dt_presets_valid) {
        yyyy = g_dt_preset_y;  mm = g_dt_preset_m;  dd = g_dt_preset_d;
        hh   = g_dt_preset_h;  mi = g_dt_preset_min; ss = g_dt_preset_s;
        g_dt_presets_valid = false; // consume once
    }

    // Format strings once (no re-declaring later)
    char sYYYY[8];  snprintf(sYYYY, sizeof sYYYY, "%04d", yyyy);
    char sMM  [4];  snprintf(sMM,   sizeof sMM,   "%02d", mm);
    char sDD  [4];  snprintf(sDD,   sizeof sDD,   "%02d", dd);
    char sHH  [4];  snprintf(sHH,   sizeof sHH,   "%02d", hh);
    char sMin [4];  snprintf(sMin,  sizeof sMin,  "%02d", mi);
    char sSS  [4];  snprintf(sSS,   sizeof sSS,   "%02d", ss);

    // ── Row 2: Date (YYYY-MM-DD) — label LEFT, inputs RIGHT ────────────────
    const int yDate = CAPTION_Y0 + BLOCK_SPACING;

    lv_obj_t* lblDate = lv_label_create(content);
    lv_label_set_text(lblDate, tr("Date (YYYY-MM-DD)"));
    lv_obj_set_style_text_color(lblDate, lv_color_white(), 0);
    lv_obj_set_style_text_font(lblDate, FONT_LABEL, 0);
    lv_obj_align(lblDate, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, yDate);

    // Right-side column (YYYY | MM | DD)
    lv_obj_t* taYYYY = lv_textarea_create(content);
    lv_textarea_set_one_line(taYYYY, true);
    lv_textarea_set_text(taYYYY, sYYYY);
    lv_textarea_set_placeholder_text(taYYYY, "YYYY");
    lv_obj_set_size(taYYYY, 160, 60);
    lv_obj_align(taYYYY, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT - 220, yDate);

    lv_obj_t* taMM = lv_textarea_create(content);
    lv_textarea_set_one_line(taMM, true);
    lv_textarea_set_text(taMM, sMM);
    lv_textarea_set_placeholder_text(taMM, "MM");
    lv_obj_set_size(taMM, 100, 60);
    lv_obj_align(taMM, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT - 110, yDate);

    lv_obj_t* taDD = lv_textarea_create(content);
    lv_textarea_set_one_line(taDD, true);
    lv_textarea_set_text(taDD, sDD);
    lv_textarea_set_placeholder_text(taDD, "DD");
    lv_obj_set_size(taDD, 100, 60);
    lv_obj_align(taDD, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, yDate);

    // Attach numeric TA focus/defocus handlers
    lv_obj_add_event_cb(taYYYY, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taYYYY, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taMM,   on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taMM,   on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taDD,   on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taDD,   on_ta_defocus, LV_EVENT_DEFOCUSED, kb);

    // Allow background tap to dismiss keyboard
    lv_obj_add_event_cb(scr, on_scr_click_hide_kb, LV_EVENT_CLICKED, kb);

    // ── Row 3: Time (HH:MM:SS) — label LEFT, inputs RIGHT ───────────────────
    const int yTime = CAPTION_Y0 + 2 * BLOCK_SPACING;

    lv_obj_t* lblTime = lv_label_create(content);
    lv_label_set_text(lblTime, tr("Time (HH:MM:SS)"));
    lv_obj_set_style_text_color(lblTime, lv_color_white(), 0);
    lv_obj_set_style_text_font(lblTime, FONT_LABEL, 0);
    lv_obj_align(lblTime, LV_ALIGN_TOP_LEFT, MARGIN_LEFT, yTime);

    lv_obj_t* taHH = lv_textarea_create(content);
    lv_textarea_set_one_line(taHH, true);
    lv_textarea_set_text(taHH, sHH);
    lv_textarea_set_placeholder_text(taHH, "HH");
    lv_obj_set_size(taHH, 100, 60);
    lv_obj_align(taHH, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT - 220, yTime);

    lv_obj_t* taMin = lv_textarea_create(content);
    lv_textarea_set_one_line(taMin, true);
    lv_textarea_set_text(taMin, sMin);
    lv_textarea_set_placeholder_text(taMin, "MM");
    lv_obj_set_size(taMin, 100, 60);
    lv_obj_align(taMin, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT - 110, yTime);

    lv_obj_t* taSS = lv_textarea_create(content);
    lv_textarea_set_one_line(taSS, true);
    lv_textarea_set_text(taSS, sSS);
    lv_textarea_set_placeholder_text(taSS, "SS");
    lv_obj_set_size(taSS, 100, 60);
    lv_obj_align(taSS, LV_ALIGN_TOP_RIGHT, -MARGIN_RIGHT, yTime);

    lv_obj_add_event_cb(taHH,  on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taHH,  on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taMin, on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taMin, on_ta_defocus, LV_EVENT_DEFOCUSED, kb);
    lv_obj_add_event_cb(taSS,  on_ta_focus,   LV_EVENT_FOCUSED,   kb);
    lv_obj_add_event_cb(taSS,  on_ta_defocus, LV_EVENT_DEFOCUSED, kb);

    // READY (✔) on the numeric keyboard: parse via effective helpers in on_kb_ready_dt
    static lv_obj_t* ta_arr6[6];
    ta_arr6[0] = taYYYY; ta_arr6[1] = taMM;  ta_arr6[2] = taDD;
    ta_arr6[3] = taHH;   ta_arr6[4] = taMin; ta_arr6[5] = taSS;
    lv_obj_add_event_cb(kb, on_kb_ready_dt, LV_EVENT_READY, ta_arr6);

    // No "Save" button here by design (auto-save on READY)
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, true);
}



static void on_sw_autosync_changed(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    rtc_set_auto_sync(on);
    if (on) {
        // If Wi‑Fi is already ON in your app, you can NTP immediately.
        // (We know g_wifi_enabled exists.)
        if (g_wifi_enabled) rtc_sync_ntp();
    }
}

static void collect_dt_fields(lv_obj_t** arr6, int& yyyy, int& mm, int& dd, int& hh, int& mi, int& ss)
{
    auto txt = [](lv_obj_t* ta)->const char* { return lv_textarea_get_text(ta); };
    yyyy = atoi(txt(arr6[0]));
    mm   = atoi(txt(arr6[1]));
    dd   = atoi(txt(arr6[2]));
    hh   = atoi(txt(arr6[3]));
    mi   = atoi(txt(arr6[4]));
    ss   = atoi(txt(arr6[5]));
    // Basic clamping
    if (yyyy < 2000) yyyy = 2000;
    if (mm   < 1) mm = 1; if (mm > 12) mm = 12;
    if (dd   < 1) dd = 1; if (dd > 31) dd = 31;
    if (hh   < 0) hh = 0; if (hh > 23) hh = 23;
    if (mi   < 0) mi = 0; if (mi > 59) mi = 59;
    if (ss   < 0) ss = 0; if (ss > 59) ss = 59;
}

static void apply_manual_time_from_fields(lv_obj_t** arr6)
{
    int yyyy, mm, dd, hh, mi, ss;
    collect_dt_fields(arr6, yyyy, mm, dd, hh, mi, ss);
    struct tm t{};
    t.tm_year = yyyy - 1900;
    t.tm_mon  = mm - 1;
    t.tm_mday = dd;
    t.tm_hour = hh;
    t.tm_min  = mi;
    t.tm_sec  = ss;
    rtc_set_manual(t);
}

static void on_kb_ready_dt(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;

    lv_obj_t* kb  = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t** a6 = (lv_obj_t**)lv_event_get_user_data(e);

    // Use your "effective" helper so no-typing keeps old values
    int yyyy = atoi(get_effective_ta_text_for_ready(kb, a6[0]));
    int mm   = atoi(get_effective_ta_text_for_ready(kb, a6[1]));
    int dd   = atoi(get_effective_ta_text_for_ready(kb, a6[2]));
    int hh   = atoi(get_effective_ta_text_for_ready(kb, a6[3]));
    int mi   = atoi(get_effective_ta_text_for_ready(kb, a6[4]));
    int ss   = atoi(get_effective_ta_text_for_ready(kb, a6[5]));

    // clamp same as today
    if (yyyy < 2000) yyyy = 2000;
    if (mm < 1) mm = 1; if (mm > 12) mm = 12;
    if (dd < 1) dd = 1; if (dd > 31) dd = 31;
    if (hh < 0) hh = 0; if (hh > 23) hh = 23;
    if (mi < 0) mi = 0; if (mi > 59) mi = 59;
    if (ss < 0) ss = 0; if (ss > 59) ss = 59;

    struct tm t{};
    t.tm_year = yyyy - 1900; t.tm_mon = mm - 1; t.tm_mday = dd;
    t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss;

    if (!rtc_get_auto_sync()) { rtc_set_manual(t); }   // your existing policy

    // Seed presets so the next screen paints exactly what we typed
    g_dt_preset_y = yyyy; g_dt_preset_m = mm; g_dt_preset_d = dd;
    g_dt_preset_h = hh;   g_dt_preset_min = mi; g_dt_preset_s = ss;
    g_dt_presets_valid = true;

    gui_show_datetime_settings(); // rebuild once with presets
}


static void on_dt_save_click(lv_event_t* e)
{
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t** arr6 = (lv_obj_t**)lv_obj_get_user_data(btn);
    if (!arr6) return;
    if (!rtc_get_auto_sync()) {
        apply_manual_time_from_fields(arr6);
    } else {
        // Auto mode => kick an NTP sync if Wi‑Fi is ON
        if (g_wifi_enabled) rtc_sync_ntp();
    }
    gui_show_settings(); // Return to Settings (or keep this screen; up to you)
}


// Refresh (rebuild) whichever screen is currently shown.
void gui_refresh_active_screen()
{
  switch (g_current_screen)
  {
    case CurrentScreen::MainMenu:   gui_show_main_menu();   break;
    case CurrentScreen::Stopwatch:  gui_show_stopwatch();   break;
    case CurrentScreen::CV:         gui_show_cv();          break;
    case CurrentScreen::Photogate:  gui_show_photogate();   break;
    case CurrentScreen::UA:         gui_show_ua();          break;
    case CurrentScreen::FreeFall:   gui_show_freefall();    break;
    case CurrentScreen::Incline:    gui_show_incline();     break;
    case CurrentScreen::Tachometer: gui_show_tacho();       break;
    case CurrentScreen::Settings:   gui_show_settings();    break;
    case CurrentScreen::WifiSettings: gui_show_wifi_export(); break;
    case CurrentScreen::CVSettings:   gui_show_cv_settings();   break;
    case CurrentScreen::PGSettings:   gui_show_pg_settings();   break;
    case CurrentScreen::UASettings:   gui_show_ua_settings();   break;
    case CurrentScreen::FFSettings:   gui_show_freefall_settings(); break;
    case CurrentScreen::INSettings:   gui_show_incline_settings();  break;
    case CurrentScreen::TASettings:   gui_show_tacho_settings();    break;
    case CurrentScreen::DTSettings:   gui_show_datetime_settings(); break;
    case CurrentScreen::None:
    default: gui_show_main_menu(); break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Net state indicators in header
// ─────────────────────────────────────────────────────────────────────────────



// [2026-01-18 15:45 CET] UPDATED: gui_on_net_state()
// - Colors the unified Wi-Fi icon by Internet reachability
// - AP badge is independent (controlled by gui_ap_start/stop)
// - Preserves your debounced NTP sync on CONNECTED
// [2026-01-18 16:15 CET] UPDATED: gui_on_net_state()
// - Colors unified Wi-Fi icon by Internet reachability


    // (Optional) If you show a header clock, you can refresh it immediately
    // so the user sees current time soon after connect:
    // extern lv_obj_t* g_hdr_clock;
    // if (g_hdr_clock) {
    //     struct tm now{};
    //     rtc_get_time(now);
    //     char buf[8];
    //     snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
    //     lv_label_set_text(g_hdr_clock, buf);
    // }



void gui_on_net_state(NetworkState /*st*/, const char* /*ssid*/, const char* /*ip*/)
{
    gui_set_net_icon_color((g_ap_running || g_wifi_enabled), false);
}
