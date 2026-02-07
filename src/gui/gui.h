
#pragma once
#include "../net/app_network.h"

void gui_refresh_active_screen();

// Net indicators in header
void gui_on_net_state(NetworkState st, const char* ssid, const char* ip);

// Main navigation
void gui_show_main_menu();
void gui_show_settings();
void gui_show_wifi_settings();

// --- Splash (ADD THESE) ---
void gui_show_splash_embedded(void);
void gui_show_splash_embedded(const char* subtitle);  // overload accepts/ignores subtitle

// Experiment screens
void gui_show_stopwatch();
void gui_show_cv();
void gui_show_photogate();
void gui_show_ua();
void gui_show_freefall();
void gui_show_incline();
void gui_show_tacho();

// Per-experiment settings
void gui_show_stopwatch_settings();
void gui_show_cv_settings();
void gui_show_pg_settings();
void gui_show_ua_settings();
void gui_show_freefall_settings();
void gui_show_incline_settings();
void gui_show_tacho_settings();

// Status/info labels
void gui_set_info (const char* text);
void gui_set_extra(const char* text);

// NEW: global activity + armed status for screensaver logic
void gui_note_user_activity();   // mark activity + wake screen if needed
bool gui_is_armed();             // true when any experiment screen is armed

// Poll for experiment completion from real optical gates (when simulation OFF)
void gui_poll_real_gate_experiments();

// Animate simulation buttons based on real gate state
void gui_set_sim_button_state(int gate_index, bool active);

// Update simulation button colors (called from main loop)
void gui_update_sim_button_colors();

// Simulation button animation control
void gui_start_sim_button_animation();
void gui_stop_sim_button_animation();