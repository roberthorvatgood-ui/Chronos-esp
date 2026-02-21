/*****
 * app_log.h
 * Chronos – Persistent SD-card-backed logging system
 * Thread-safe structured logging with runtime level filtering
 *****/
#pragma once

#include <Arduino.h>

/* Log Levels */
#define LOG_LEVEL_DEBUG   0
#define LOG_LEVEL_INFO    1
#define LOG_LEVEL_WARN    2
#define LOG_LEVEL_ERROR   3
#define LOG_LEVEL_FATAL   4

/* Initialize logging subsystem (call after SD mount) */
void applog_init();

/* Signal boot complete — flushes buffered boot logs to SD in one write */
void applog_boot_complete();

/* Log macros - format: YYYY-MM-DD HH:MM:SS [L] [TAG] message */
#define LOG_D(tag, fmt, ...) applog_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) applog_write(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) applog_write(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) applog_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOG_F(tag, fmt, ...) applog_write(LOG_LEVEL_FATAL, tag, fmt, ##__VA_ARGS__)

/* Core logging function (used by macros) */
void applog_write(int level, const char* tag, const char* fmt, ...);

/* Read last N lines of log file */
String applog_read_tail(int lines);

/* Clear log file */
void applog_clear();

/* Get/set runtime log level (0=D, 1=I, 2=W, 3=E, 4=F) */
int applog_get_level();
void applog_set_level(int level);