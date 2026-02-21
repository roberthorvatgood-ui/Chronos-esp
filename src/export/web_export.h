/***** web_export.h — Chronos AP Web (presence everywhere) *****/
// [2026-01-25 01:06 CET] Copilot: header exposes presence + server APIs
#pragma once
#include <Arduino.h>

namespace chronos {

// Start AP web server
bool apweb_begin(const char* ssid, const char* pass);
bool apweb_begin();
bool apweb_begin_ap_only(const char* ssid, const char* pass);

// Pump web server from loop()
void apweb_loop();

// Stop server
void apweb_end();

// While true, UI should consider AP doing FS/CS work (or in cooldown)
bool apweb_fs_busy();

// True while an AP page has pinged or any AP handler was hit recently
bool apweb_user_present();

// True while a file download/purge is actively in progress (gates should be paused)
bool apweb_download_active();

} // namespace chronos
