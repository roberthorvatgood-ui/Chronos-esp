
#include "../core/rtc_manager.h"   // <-- include our own header FIRST
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "../drivers/hal_i2c_manager.h"

/* ========= Local settings/state ========= */

static bool      s_auto_sync = true;
static struct tm s_manual_tm = {};                 // persisted manual baseline
static char      s_tz[64]    = "CET-1CEST,M3.5.0,M10.5.0/3";
static uint64_t  s_last_epoch= 0ULL;               // last known good epoch

static void apply_tz() { setenv("TZ", s_tz, 1); tzset(); }

static void nvs_load_all() {
  Preferences p;
  if (p.begin("rtc", true)) {
    s_auto_sync          = p.getBool ("auto_sync", true);
    int y                = p.getShort("year", 2025);
    s_manual_tm.tm_year  = (y > 1900 ? y - 1900 : 125);
    s_manual_tm.tm_mon   = p.getShort("month", 1) - 1;
    s_manual_tm.tm_mday  = p.getShort("day",   1);
    s_manual_tm.tm_hour  = p.getShort("hour", 12);
    s_manual_tm.tm_min   = p.getShort("min",   0);
    s_manual_tm.tm_sec   = p.getShort("sec",   0);
    String tz            = p.getString("tz", s_tz);
    strncpy(s_tz, tz.c_str(), sizeof(s_tz)-1); s_tz[sizeof(s_tz)-1]='\0';
    s_last_epoch         = p.getULong64("last_epoch", 0ULL);
    p.end();
  }
  apply_tz();
}

static void nvs_save_all() {
  Preferences p;
  if (p.begin("rtc", false)) {
    p.putBool ("auto_sync", s_auto_sync);
    p.putShort("year",  s_manual_tm.tm_year + 1900);
    p.putShort("month", s_manual_tm.tm_mon + 1);
    p.putShort("day",   s_manual_tm.tm_mday);
    p.putShort("hour",  s_manual_tm.tm_hour);
    p.putShort("min",   s_manual_tm.tm_min);
    p.putShort("sec",   s_manual_tm.tm_sec);
    p.putString("tz",   s_tz);
    p.putULong64("last_epoch", s_last_epoch);
    p.end();
  }
}

static void nvs_save_last_epoch(time_t now) {
  if (now < 946684800 /*2000-01-01*/) return;
  s_last_epoch = (uint64_t)now;
  Preferences p; 
  if (p.begin("rtc", false)) { 
    p.putULong64("last_epoch", s_last_epoch); 
    p.end(); 
  }
}

/* ========= Optional HW hooks ========= */

static rtc_hw_write_fn_t s_hw_writer = nullptr;
static rtc_hw_read_fn_t  s_hw_reader = nullptr;

void rtc_set_hw_time_writer(rtc_hw_write_fn_t fn) { s_hw_writer = fn; }
void rtc_set_hw_time_reader(rtc_hw_read_fn_t fn)  { s_hw_reader = fn; }

/* ========= SNTP integration (non-blocking) ========= */

#if defined(ESP_PLATFORM)
  #include <esp_sntp.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t s_rtc_task = nullptr;

static void rtc_worker_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Persist “last_epoch”
    time_t now = time(nullptr);
    nvs_save_last_epoch(now);

    // Hardware writer (PCF85063) is called in safe context
    if (s_hw_writer) {
      struct tm loc{}; localtime_r(&now, &loc);
      
      if (!hal::i2c_lock(100)) {
        Serial.println("[RTC] HW writer skipped: I2C lock timeout");
        continue;
      }
      
      bool ok = s_hw_writer(loc);
      hal::i2c_unlock();
      
      Serial.printf("[RTC] HW writer called -> %s\n", ok ? "OK" : "fail");
    }
  }
}

#if defined(ESP_PLATFORM)
static void on_time_sync_cb(struct timeval* tv) {
  (void)tv;
  if (s_rtc_task) xTaskNotifyGive(s_rtc_task);
}
#endif

/* ========= Public API ========= */

void rtc_load_settings(void) { nvs_load_all(); }
void rtc_save_settings(void) { nvs_save_all(); }

void rtc_set_timezone(const char* tz) {
  if (!tz) return;
  strncpy(s_tz, tz, sizeof(s_tz)-1); 
  s_tz[sizeof(s_tz)-1]='\0';
  apply_tz();
  nvs_save_all();
}

const char* rtc_get_timezone(void) { return s_tz; }

bool rtc_get_time(struct tm& out) {
  time_t now; time(&now);
  localtime_r(&now, &out);
  return true;
}

bool rtc_is_time_set(void) {
  struct tm t{}; rtc_get_time(t);
  return (t.tm_year + 1900) >= 2000;
}

/* --- PATCH: manual set writes HW RTC immediately --- */
void rtc_set_manual(const struct tm& t) {
  s_manual_tm = t;

  time_t sec = mktime((struct tm*)&t);
  struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  nvs_save_last_epoch(sec);
  nvs_save_all();

  if (s_hw_writer) {
    if (!hal::i2c_lock(100)) {
      Serial.println("[RTC] Manual time -> HW RTC: SKIPPED (I2C lock timeout)");
    } else {
      bool ok = s_hw_writer(t);
      hal::i2c_unlock();
      Serial.printf("[RTC] Manual time -> HW RTC: %s\n", ok ? "OK" : "FAIL");
    }
  }
}

static void seed_from_last_epoch_if_needed() {
  time_t now; time(&now);
  if (now >= 946684800) return;
  if (s_last_epoch >= 946684800) {
    struct timeval tv = { .tv_sec = (time_t)s_last_epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[RTC] Seeded time from last_epoch (NVS).");
  } else {
    Serial.println("[RTC] No valid last_epoch; waiting for NTP/manual.");
  }
}

void rtc_sync_ntp(void) {
  apply_tz();
#if defined(ESP_PLATFORM)
  configTzTime(s_tz, "pool.ntp.org", "time.google.com", "time.windows.com");
#else
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.windows.com");
#endif
}

void rtc_set_auto_sync(bool on) {
  s_auto_sync = on;

  if (!on) {
    // fresh manual baseline
    time_t now; time(&now);
    struct tm loc{}; localtime_r(&now, &loc);
    s_manual_tm = loc;
  }
  nvs_save_all();
}

bool rtc_get_auto_sync(void) { return s_auto_sync; }

/* --- PATCH: DO NOT override valid RTC time with manual baseline --- */
void rtc_apply(void) {
  if (s_auto_sync) {
    rtc_sync_ntp();
    return;
  }

  // Manual mode: only apply manual time if system time is invalid
  if (!rtc_is_time_set()) {
    rtc_set_manual(s_manual_tm);
  } else {
    Serial.println("[RTC] Skipped manual baseline (system time valid)");
  }
}

/* --- PATCH: Prefer hardware RTC and return immediately on success --- */
void rtc_init(void) {

  nvs_load_all();
  apply_tz();

  // 1) Try hardware RTC first
  if (s_hw_reader) {
    struct tm hw{};
    bool hw_ok = false;
    
    if (!hal::i2c_lock(100)) {
      Serial.println("[RTC] HW reader skipped: I2C lock timeout");
    } else {
      hw_ok = s_hw_reader(hw) && (hw.tm_year + 1900) >= 2020;
      hal::i2c_unlock();
    }
    
    if (hw_ok) {

      time_t sec = mktime(&hw);
      struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
      settimeofday(&tv, nullptr);

      Serial.println("[RTC] Boot from HW reader hook.");

#if defined(ESP_PLATFORM)
      sntp_set_time_sync_notification_cb(on_time_sync_cb);
#endif

      if (!s_rtc_task) {
        xTaskCreatePinnedToCore(rtc_worker_task, "rtc_worker",
                                4096, nullptr, 4, &s_rtc_task, tskNO_AFFINITY);
      }

      rtc_apply();
      return;   // <-- CRITICAL LINE
    }
  }

  // 2) Otherwise seed from NVS if needed
  if (!rtc_is_time_set()) seed_from_last_epoch_if_needed();

#if defined(ESP_PLATFORM)
  sntp_set_time_sync_notification_cb(on_time_sync_cb);
#endif

  if (!s_rtc_task) {
    xTaskCreatePinnedToCore(rtc_worker_task, "rtc_worker",
                            4096, nullptr, 4, &s_rtc_task, tskNO_AFFINITY);
  }

  rtc_apply();
}
