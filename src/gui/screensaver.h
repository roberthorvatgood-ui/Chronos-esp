
// screensaver.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Show floating welcome-screen saver (dims if real PWM is available)
void screensaver_show(void);

// Hide saver and restore normal UI/backlight
void screensaver_hide(void);

// Hide asynchronously via lv_async_call (safe from non-LVGL contexts)
void screensaver_hide_async(void);

// Refresh locale-dependent text (“Touch to wake” label etc.)
void screensaver_refresh_language(void);

// [2026-01-25 12:30 CET] AP-web hold gate
// - hold = true  → saver is fully disabled; if active, it is hidden immediately.
// - hold = false → saver is allowed again (subject to overall inactivity logic).
void screensaver_set_apweb_hold(bool hold);

#ifdef __cplusplus
} // extern "C"
#endif
