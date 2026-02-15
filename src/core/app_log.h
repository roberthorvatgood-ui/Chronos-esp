/*****
 * app_log.h — Chronos Persistent Logging Subsystem
 * Logs operational events and faults to SD card with Web UI retrieval
 *****/
#pragma once

#include <Arduino.h>
#include <stdint.h>

/** Log severity levels (higher value = more severe) */
enum LogLevel : uint8_t {
  LOG_DEBUG = 0,
  LOG_INFO  = 1,
  LOG_WARN  = 2,
  LOG_ERROR = 3,
  LOG_FATAL = 4
};

/**
 * Initialize logging subsystem.
 * Call AFTER chronos_sd_begin() and rtc_init().
 * @param minLevel    Minimum log level to record (default: LOG_INFO)
 * @param maxFileKB   Max log file size in KB before rotation (default: 512)
 */
void app_log_init(LogLevel minLevel = LOG_INFO, uint16_t maxFileKB = 512);

/**
 * Write a timestamped log entry.
 * Format: "YYYY-MM-DD HH:MM:SS [L] [tag] message\n"
 * Also echoes to Serial.
 * Thread-safe (uses spinlock).
 */
void app_log(LogLevel level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/**
 * Flush buffered log entries to SD immediately.
 * Automatically called by app_log_loop() every 5 seconds.
 */
void app_log_flush();

/**
 * Periodic maintenance task — call from loop().
 * Flushes buffer if auto-flush interval elapsed.
 * Lightweight, returns immediately if nothing to write.
 */
void app_log_loop();

/**
 * Change minimum log level at runtime.
 * New level is persisted to NVS.
 */
void app_log_set_level(LogLevel level);

/**
 * Get current minimum log level.
 */
LogLevel app_log_get_level();

/**
 * Get current log file path.
 * @return Path to current log file (e.g., "/log/chronos.log")
 */
const char* app_log_get_path();

/**
 * Get total log size on disk (current + rotated).
 * @return Total size in bytes
 */
uint32_t app_log_total_size();

// ───────────────────────────────────────────────────────────────────────────
// Convenience Macros
// ───────────────────────────────────────────────────────────────────────────
#define CLOG_D(tag, fmt, ...) app_log(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define CLOG_I(tag, fmt, ...) app_log(LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define CLOG_W(tag, fmt, ...) app_log(LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define CLOG_E(tag, fmt, ...) app_log(LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define CLOG_F(tag, fmt, ...) app_log(LOG_FATAL, tag, fmt, ##__VA_ARGS__)
