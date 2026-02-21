/*****
 * app_log.cpp
 * Chronos – Persistent SD-card-backed logging system
 * Thread-safe structured logging with runtime level filtering, rotation, NVS persistence
 * [Updated: 2026-02-18] Deferred boot writes to eliminate SD overhead during setup()
 *****/

#include "app_log.h"
#include "chronos_sd.h"
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>

/* Configuration */
static const char* LOG_PATH = "/log/chronos.log";
static const char* LOG_PATH_OLD = "/log/chronos.log.1";
static const size_t MAX_LOG_SIZE = 512 * 1024; // 512 KB
static const char* NVS_NAMESPACE = "applog";
static const char* NVS_LEVEL_KEY = "level";

/* State */
static SemaphoreHandle_t s_log_mutex = nullptr;
static int s_current_level = LOG_LEVEL_INFO;  // Default level
static bool s_initialized = false;
static bool s_boot_complete = false;           // NEW: deferred write flag

/* Boot buffer: accumulate log lines during setup(), flush once at end */
static String s_boot_buffer;
static const size_t BOOT_BUFFER_MAX = 4096;

/* Level to character mapping */
static char level_char(int level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return 'D';
        case LOG_LEVEL_INFO:  return 'I';
        case LOG_LEVEL_WARN:  return 'W';
        case LOG_LEVEL_ERROR: return 'E';
        case LOG_LEVEL_FATAL: return 'F';
        default: return '?';
    }
}

/* Format timestamp as YYYY-MM-DD HH:MM:SS */
static String format_timestamp() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        // Fallback if RTC not ready
        unsigned long sec = millis() / 1000;
        char buf[32];
        snprintf(buf, sizeof(buf), "2000-01-01 %02lu:%02lu:%02lu",
                 (sec / 3600) % 24, (sec / 60) % 60, sec % 60);
        return String(buf);
    }
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);
    return String(buf);
}

/* Check and rotate log file if needed */
static void check_rotation() {
    ChronosSdSelectGuard _sd;
    
    if (!SD.exists(LOG_PATH)) {
        return; // No rotation needed
    }
    
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) return;
    
    size_t size = f.size();
    f.close();
    
    if (size >= MAX_LOG_SIZE) {
        // Rotate: remove old backup, rename current to .1
        if (SD.exists(LOG_PATH_OLD)) {
            SD.remove(LOG_PATH_OLD);
        }
        SD.rename(LOG_PATH, LOG_PATH_OLD);
        Serial.println("[APPLOG] Log rotated (max size reached)");
    }
}

/* Load saved level from NVS */
static void load_level_from_nvs() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, true)) {  // Read-only
        s_current_level = prefs.getInt(NVS_LEVEL_KEY, LOG_LEVEL_INFO);
        prefs.end();
    }
}

/* Save level to NVS */
static void save_level_to_nvs(int level) {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {  // Read-write
        prefs.putInt(NVS_LEVEL_KEY, level);
        prefs.end();
    }
}

/* Flush boot buffer to SD in a single write */
static void flush_boot_buffer() {
    if (s_boot_buffer.length() == 0) return;
    
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        check_rotation();
        
        ChronosSdSelectGuard _sd;
        File f = SD.open(LOG_PATH, FILE_APPEND);
        if (f) {
            f.print(s_boot_buffer);  // single write for all boot lines
            f.close();
        }
        
        xSemaphoreGive(s_log_mutex);
    }
    
    s_boot_buffer = "";  // free memory
}

/* Initialize logging subsystem */
void applog_init() {
    if (s_initialized) {
        return;  // Already initialized
    }
    
    // Create mutex
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    
    // Load saved level from NVS
    load_level_from_nvs();
    
    // Create log directory if it doesn't exist
    {
        ChronosSdSelectGuard _sd;
        if (!SD.exists("/log")) {
            SD.mkdir("/log");
        }
    }
    
    // Reserve boot buffer
    s_boot_buffer.reserve(BOOT_BUFFER_MAX);
    
    s_initialized = true;
    
    // Write initialization message
    applog_write(LOG_LEVEL_INFO, "APPLOG", 
                 "Logging initialized (level=%c, max=%u KB)",
                 level_char(s_current_level), (unsigned)(MAX_LOG_SIZE / 1024));
}

/* Signal that boot is complete — flush buffered logs to SD */
void applog_boot_complete() {
    s_boot_complete = true;
    flush_boot_buffer();
    Serial.println("[APPLOG] Boot buffer flushed to SD");
}

/* Core logging function */
void applog_write(int level, const char* tag, const char* fmt, ...) {
    if (!s_initialized) {
        return;  // Not initialized yet
    }
    
    // Filter by level
    if (level < s_current_level) {
        return;
    }
    
    // Format message
    char msg_buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);
    
    // Format full log line: YYYY-MM-DD HH:MM:SS [L] [TAG] message
    String timestamp = format_timestamp();
    char level_c = level_char(level);
    String line = timestamp + " [" + level_c + "] [" + tag + "] " + msg_buf + "\n";
    
    // Print to Serial (always, regardless of level)
    Serial.print(line);
    
    // ── During boot: buffer in RAM instead of hitting SD per line ──
    if (!s_boot_complete) {
        if (s_boot_buffer.length() + line.length() < BOOT_BUFFER_MAX) {
            s_boot_buffer += line;
        }
        return;  // skip SD write until boot is done
    }
    
    // Write to SD card (with mutex protection)
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        check_rotation();
        
        ChronosSdSelectGuard _sd;
        File f = SD.open(LOG_PATH, FILE_APPEND);
        if (f) {
            f.print(line);
            f.close();
        }
        
        xSemaphoreGive(s_log_mutex);
    }
}

/* Read last N lines of log file */
String applog_read_tail(int lines) {
    if (!s_initialized || lines <= 0) {
        return "";
    }
    
    String result = "";
    
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        ChronosSdSelectGuard _sd;
        
        File f = SD.open(LOG_PATH, FILE_READ);
        if (f) {
            size_t fileSize = f.size();
            
            // Read entire file into buffer (up to reasonable limit)
            const size_t MAX_READ = 64 * 1024;  // 64 KB max
            size_t readSize = min(fileSize, MAX_READ);
            
            // For tail, we want the last portion
            if (fileSize > MAX_READ) {
                f.seek(fileSize - MAX_READ);
            }
            
            // Read into String
            String content = "";
            while (f.available() && content.length() < MAX_READ) {
                char c = f.read();
                if (c >= 0) {
                    content += (char)c;
                }
            }
            f.close();
            
            // Extract last N lines
            int line_count = 0;
            int idx = content.length() - 1;
            
            // Skip trailing newlines
            while (idx >= 0 && (content[idx] == '\n' || content[idx] == '\r')) {
                idx--;
            }
            
            // Count backwards to find start of Nth line
            int end_pos = idx + 1;
            while (idx >= 0 && line_count < lines) {
                if (content[idx] == '\n') {
                    line_count++;
                }
                if (line_count < lines) {
                    idx--;
                }
            }
            
            // Extract substring
            if (idx < 0) idx = 0;
            else idx++;  // Move past the newline
            
            result = content.substring(idx, end_pos + 1);
            if (!result.endsWith("\n")) {
                result += "\n";
            }
        }
        
        xSemaphoreGive(s_log_mutex);
    }
    
    return result;
}

/* Clear log file */
void applog_clear() {
    if (!s_initialized) {
        return;
    }
    
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        ChronosSdSelectGuard _sd;
        
        if (SD.exists(LOG_PATH)) {
            SD.remove(LOG_PATH);
        }
        if (SD.exists(LOG_PATH_OLD)) {
            SD.remove(LOG_PATH_OLD);
        }
        
        // Create fresh log file
        File f = SD.open(LOG_PATH, FILE_WRITE);
        if (f) {
            f.close();
        }
        
        xSemaphoreGive(s_log_mutex);
    }
    
    applog_write(LOG_LEVEL_INFO, "APPLOG", "Log cleared");
}

/* Get current log level */
int applog_get_level() {
    return s_current_level;
}

/* Set log level (and persist to NVS) */
void applog_set_level(int level) {
    if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_FATAL) {
        return;  // Invalid level
    }
    
    s_current_level = level;
    save_level_to_nvs(level);
    
    applog_write(LOG_LEVEL_INFO, "APPLOG", 
                 "Log level changed to %c", level_char(level));
}