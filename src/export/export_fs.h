
/*******************************************************
 * export_fs.(cpp|h)
 * Chronos – Export filesystem engine
 * [2026-01-18 16:30 CET] UPDATED
 *
 * Purpose:
 *  - File system abstraction for Chronos exports (CSV, ZIP, list, delete)
 *
 * Key features:
 *  - /exp/YYYY-MM-DD/Chronos_<Name>_<timestamp>.csv writer
 *  - Grouped JSON builder for the web UI
 *  - Temp ZIP creation and auto-clean
 *******************************************************/


#pragma once
#include <Arduino.h>
#include <FS.h>

namespace chronos {

// --- FS registration ---
void   exportfs_set_fs(FS* fs);
FS*    exportfs_get_fs();
bool   exportfs_begin();

// --- Path + CSV save ---
String exportfs_make_path(const char* mode);
String exportfs_save_csv(const char* mode, void (*emit_csv)(Print& out));

// --- Utility ---
String pretty_size(size_t bytes);

// --- Listing (safe builder; stack-light; no STL) ---
void   exportfs_build_grouped_json(String& out);
void   exportfs_emit_grouped_json(Print& out);

// --- Delete helpers ---
bool   exportfs_delete_file(const String& absPath);
bool   exportfs_delete_date(const String& date);

// --- Status + Purge ---
bool   exportfs_fs_stats(uint64_t& total, uint64_t& used);
bool   exportfs_purge_oldest_until_free(uint64_t minFreeBytes, uint64_t* outFreed = nullptr);

// --- ZIP creators ---
// Stored ZIP in the date folder (kept here for compatibility; not used by UI now)
String exportfs_zip_date(const String& date, const char* zipName = nullptr);

// TEMP ZIP (preferred): creates /tmp/Chronos_TempZip_<date>_<HH-mm-ss>.zip and returns its path.
// Caller can stream it and immediately remove it. Never appears in /api/list.
String exportfs_zip_temp_date(const String& date, String* outZipName = nullptr);

} // namespace chronos
