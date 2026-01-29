
#pragma once
#include <time.h>
#include <stdint.h>

/** Initialize: load settings, seed time from NVS (last_epoch) if needed, arm SNTP. */
void rtc_init(void);

/** Settings load/save (Preferences/NVS) */
void rtc_load_settings(void);
void rtc_save_settings(void);

/** Get current local time (true if a struct tm returned; ESP32 always returns something once set) */
bool rtc_get_time(struct tm& out);

/** Manually set the clock (also persisted to NVS as manual baseline & last_epoch) */
void rtc_set_manual(const struct tm& t);

/** Automatic NTP sync toggle (persisted) */
void rtc_set_auto_sync(bool on);
bool rtc_get_auto_sync(void);

/** Trigger NTP sync now (uses saved TZ and servers) */
void rtc_sync_ntp(void);

/** Apply current policy: if auto -> NTP; else -> manual */
void rtc_apply(void);

/** Optional: set timezone (POSIX TZ string, e.g., "CET-1CEST,M3.5.0,M10.5.0/3") (persisted) */
void rtc_set_timezone(const char* tz);
const char* rtc_get_timezone(void);

/** Utility: check if time has been set (year >= 2000) */
bool rtc_is_time_set(void);

/* --------------------------------------------------------------------------
   OPTIONAL HARDWARE RTC HOOKS (NO I²C HERE)
   If you later want to push time to a PCF85063 (or read from it) using the
   correct I²C stack elsewhere in your project, register two callbacks.

   - hw_write(t): called after SNTP sync (in a worker task), to write t to HW.
   - hw_read(t):  called during init; if returns true with a valid t (>= 2000),
                  rtc_init() seeds the system clock from it, before NTP.
  -------------------------------------------------------------------------- */

typedef bool (*rtc_hw_write_fn_t)(const struct tm& t);
typedef bool (*rtc_hw_read_fn_t) (struct tm& out);

void rtc_set_hw_time_writer(rtc_hw_write_fn_t fn);  // optional
void rtc_set_hw_time_reader(rtc_hw_read_fn_t fn);   // optional

