/*****
 * app_log.cpp — Chronos Persistent Logging Implementation
 *****/

#include "app_log.h"
#include "../export/chronos_sd.h"
#include "rtc_manager.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <time.h>
#include <stdarg.h>

// ─────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────
static const char* LOG_DIR         = "/log";
static const char* LOG_FILE        = "/log/chronos.log";
static const char* LOG_FILE_OLD    = "/log/chronos.old.log";
static const char* NVS_NAMESPACE   = "applog";
static const char* NVS_KEY_LEVEL   = "level";

static constexpr uint16_t RING_SIZE      = 4096;
static constexpr uint32_t FLUSH_INTERVAL = 5000; // ms

// ─────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────
static char      s_ring[RING_SIZE];
static uint16_t  s_ring_head = 0;
static uint16_t  s_ring_tail = 0;
static LogLevel  s_min_level = LOG_INFO;
static uint16_t  s_max_file_kb = 512;
static uint32_t  s_max_file_bytes = 0; // Cached byte threshold
static uint32_t  s_last_flush_ms = 0;
static bool      s_initialized = false;
static uint32_t  s_boot_uptime_s = 0;
static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;

// ─────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────
static inline uint16_t ring_used() {
  if (s_ring_head >= s_ring_tail) return s_ring_head - s_ring_tail;
  return RING_SIZE - s_ring_tail + s_ring_head;
}

static inline uint16_t ring_free() {
  return RING_SIZE - ring_used() - 1; // leave 1 byte gap
}

static void ring_push(const char* data, uint16_t len) {
  for (uint16_t i = 0; i < len; ++i) {
    s_ring[s_ring_head] = data[i];
    s_ring_head = (s_ring_head + 1) % RING_SIZE;
    // If buffer full, overwrite tail (lose oldest data)
    if (s_ring_head == s_ring_tail) {
      s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
    }
  }
}

static char level_char(LogLevel lv) {
  switch (lv) {
    case LOG_DEBUG: return 'D';
    case LOG_INFO:  return 'I';
    case LOG_WARN:  return 'W';
    case LOG_ERROR: return 'E';
    case LOG_FATAL: return 'F';
    default:        return '?';
  }
}

static void format_timestamp(char* buf, size_t bufsize) {
  struct tm t;
  if (rtc_get_time(t) && rtc_is_time_set()) {
    snprintf(buf, bufsize, "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    // Fallback: uptime since boot
    // Note: millis() overflows after ~49.7 days; this is acceptable for temporary fallback
    uint32_t uptime_s = (millis() / 1000) + s_boot_uptime_s;
    snprintf(buf, bufsize, "UP+%lu", (unsigned long)uptime_s);
  }
}

static void do_rotation() {
  if (!chronos_sd_is_ready()) return;
  
  ChronosSdSelectGuard _sel;
  
  // Delete old backup if it exists
  if (SD.exists(LOG_FILE_OLD)) {
    SD.remove(LOG_FILE_OLD);
  }
  
  // Rename current to old
  if (SD.exists(LOG_FILE)) {
    SD.rename(LOG_FILE, LOG_FILE_OLD);
  }
}

static void check_rotation() {
  if (!chronos_sd_is_ready()) return;
  
  ChronosSdSelectGuard _sel;
  if (!SD.exists(LOG_FILE)) return;
  
  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) return;
  
  size_t sz = f.size();
  f.close();
  
  if (sz >= s_max_file_bytes) {
    do_rotation();
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────
void app_log_init(LogLevel minLevel, uint16_t maxFileKB) {
  if (s_initialized) return;
  
  s_min_level = minLevel;
  s_max_file_kb = maxFileKB;
  s_max_file_bytes = (uint32_t)maxFileKB * 1024; // Cache byte threshold
  s_boot_uptime_s = millis() / 1000;
  s_last_flush_ms = millis();
  
  // Load persisted level from NVS
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    uint8_t saved = prefs.getUChar(NVS_KEY_LEVEL, (uint8_t)s_min_level);
    if (saved <= LOG_FATAL) {
      s_min_level = (LogLevel)saved;
    }
    prefs.end();
  }
  
  // Create log directory if needed
  if (chronos_sd_is_ready()) {
    ChronosSdSelectGuard _sel;
    if (!SD.exists(LOG_DIR)) {
      SD.mkdir(LOG_DIR);
    }
  }
  
  s_initialized = true;
  
  // Write boot marker
  app_log(LOG_INFO, "APPLOG", "Logging initialized (level=%c, max=%u KB)",
          level_char(s_min_level), s_max_file_kb);
}

void app_log(LogLevel level, const char* tag, const char* fmt, ...) {
  if (!s_initialized) return;
  if (level < s_min_level) return;
  
  // Format message
  char msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  
  // Build log line: "YYYY-MM-DD HH:MM:SS [L] [tag] message\n"
  char timestamp[32];
  format_timestamp(timestamp, sizeof(timestamp));
  
  char line[384];
  int len = snprintf(line, sizeof(line), "%s [%c] [%s] %s\n",
                     timestamp, level_char(level), tag, msg);
  if (len <= 0) return;
  if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
  
  // Echo to Serial
  Serial.print(line);
  
  // Add to ring buffer (thread-safe spinlock)
  portENTER_CRITICAL(&s_spinlock);
  ring_push(line, len);
  portEXIT_CRITICAL(&s_spinlock);
  
  // Immediate flush for critical errors
  if (level >= LOG_ERROR) {
    app_log_flush();
  }
}

void app_log_flush() {
  if (!s_initialized || !chronos_sd_is_ready()) return;
  
  // Copy ring buffer to local buffer (minimize critical section)
  portENTER_CRITICAL(&s_spinlock);
  uint16_t used = ring_used();
  if (used == 0) {
    portEXIT_CRITICAL(&s_spinlock);
    return;
  }
  
  char local[RING_SIZE];
  uint16_t idx = 0;
  while (s_ring_tail != s_ring_head) {
    local[idx++] = s_ring[s_ring_tail];
    s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
  }
  portEXIT_CRITICAL(&s_spinlock);
  
  // Check for rotation
  check_rotation();
  
  // Write to SD
  ChronosSdSelectGuard _sel;
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (f) {
    f.write((const uint8_t*)local, idx);
    f.close();
  }
  
  s_last_flush_ms = millis();
}

void app_log_loop() {
  if (!s_initialized) return;
  
  uint32_t now = millis();
  if (now - s_last_flush_ms >= FLUSH_INTERVAL) {
    app_log_flush();
  }
}

void app_log_set_level(LogLevel level) {
  if (level > LOG_FATAL) return;
  
  s_min_level = level;
  
  // Persist to NVS
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_KEY_LEVEL, (uint8_t)level);
    prefs.end();
  }
  
  app_log(LOG_INFO, "APPLOG", "Log level changed to %c", level_char(level));
}

LogLevel app_log_get_level() {
  return s_min_level;
}

const char* app_log_get_path() {
  return LOG_FILE;
}

uint32_t app_log_total_size() {
  uint32_t total = 0;
  
  if (!chronos_sd_is_ready()) return 0;
  
  ChronosSdSelectGuard _sel;
  
  if (SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_READ);
    if (f) {
      total += f.size();
      f.close();
    }
  }
  
  if (SD.exists(LOG_FILE_OLD)) {
    File f = SD.open(LOG_FILE_OLD, FILE_READ);
    if (f) {
      total += f.size();
      f.close();
    }
  }
  
  return total;
}
