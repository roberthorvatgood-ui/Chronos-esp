/*****
 * web_export.cpp — Chronos AP Web (Presence Everywhere + Sticky Busy)
 * [2026-01-25 01:06 CET] + [2026-01-25 14:30 CET] AP-web ↔ Screensaver hold
 *****/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>

#include "web_export.h"
#include "export_fs.h"
#include "chronos_sd.h"
#include "../core/app_log.h"

#ifdef HAS_GUI_NOTE_USER_ACTIVITY
extern "C" void gui_note_user_activity();
#endif

#ifdef HAS_SAVER_HIDE_ASYNC
extern "C" void screensaver_hide_async();
extern "C" void screensaver_set_apweb_hold(bool hold);  // NEW: saver hold gate
#endif

namespace chronos {

static WebServer* s_server = nullptr;
static bool       s_running = false;
static volatile bool s_list_busy = false;
static volatile bool s_zip_busy  = false;

static volatile int           s_fs_busy_count    = 0;
static volatile unsigned long s_fs_busy_until_ms = 0;
static volatile unsigned long s_presence_last_ms = 0;
static constexpr unsigned long kPresenceTTL_ms   = 15000; // 15 s

static volatile bool s_defer_ui_nudge = false;

static inline bool time_not_expired(unsigned long deadline) {
    return (long)(deadline - millis()) > 0;
}

bool apweb_fs_busy() {
    return (s_fs_busy_count > 0) || time_not_expired(s_fs_busy_until_ms);
}

bool apweb_user_present() {
    return (long)(millis() - s_presence_last_ms) < (long)kPresenceTTL_ms;
}

// Presence-everywhere helper
static inline void mark_presence() {
    s_presence_last_ms = millis();
}

struct FsBusyGuard {
    FsBusyGuard()  { s_fs_busy_count++; }
    ~FsBusyGuard() {
        if (s_fs_busy_count > 0) s_fs_busy_count--;
        s_fs_busy_until_ms = millis() + 2000;
    }
};



static const char* k_web_tag = "APWEB_UI";

static void send_no_cache_headers() {
    s_server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    s_server->sendHeader("Pragma", "no-cache");
    s_server->sendHeader("Expires", "0");
    s_server->sendHeader("Access-Control-Allow-Origin", "*");
}



// Updated: 2026-01-25 20:05 CET — Full enterprise UI: storage+theme (top), collapsible search (mobile auto-collapse), bulk ops, sort, responsive
// Updated: 2026-01-25 21:45 CET — AP Web Fluent UI fully localized (EN/HR/DE/FR/ES)
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Chronos Exports</title>
  <style>
    :root{
      color-scheme: light dark;
      --font: "Segoe UI", -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif;

      /* Light */
      --bg:#f3f2f1;
      --surface:#ffffff;
      --surface2:#faf9f8;
      --text:#323130;
      --muted:#605e5c;
      --border:#edebe9;
      --border2:#e1dfdd;

      --accent:#0078d4;
      --accent2:#106ebe;
      --danger:#d13438;
      --danger2:#a4262c;

      --shadow: 0 1px 2px rgba(0,0,0,.06), 0 6px 20px rgba(0,0,0,.08);
      --shadow2: 0 1px 2px rgba(0,0,0,.05);
      --radius: 12px;

      --focus: 0 0 0 3px rgba(0,120,212,.25);
      --focus2: 0 0 0 2px rgba(0,120,212,.35);

      --hdrGrad1:#0f6cbd;
      --hdrGrad2:#004e8c;
    }

    @media (prefers-color-scheme: dark){
      :root{
        --bg:#0f0f10;
        --surface:#151515;
        --surface2:#1b1b1b;
        --text:#f3f2f1;
        --muted:#c8c6c4;
        --border:#2a2a2a;
        --border2:#343434;

        --accent:#4cc2ff;
        --accent2:#2aa0df;
        --danger:#ff5a5f;
        --danger2:#d64545;

        --shadow: 0 1px 2px rgba(0,0,0,.35), 0 10px 26px rgba(0,0,0,.35);
        --shadow2: 0 1px 2px rgba(0,0,0,.25);

        --focus: 0 0 0 3px rgba(76,194,255,.25);
        --focus2: 0 0 0 2px rgba(76,194,255,.35);

        --hdrGrad1:#0b3a66;
        --hdrGrad2:#071e33;
      }
    }

    /* Manual theme override (set on <html data-theme="...">) */
    html[data-theme="light"]{ color-scheme: light; }
    html[data-theme="dark"]{  color-scheme: dark;  }

    html[data-theme="light"]:root{
      --bg:#f3f2f1; --surface:#ffffff; --surface2:#faf9f8; --text:#323130; --muted:#605e5c;
      --border:#edebe9; --border2:#e1dfdd;
      --accent:#0078d4; --accent2:#106ebe;
      --danger:#d13438; --danger2:#a4262c;
      --shadow: 0 1px 2px rgba(0,0,0,.06), 0 6px 20px rgba(0,0,0,.08);
      --shadow2: 0 1px 2px rgba(0,0,0,.05);
      --focus: 0 0 0 3px rgba(0,120,212,.25);
      --focus2: 0 0 0 2px rgba(0,120,212,.35);
      --hdrGrad1:#0f6cbd; --hdrGrad2:#004e8c;
    }
    html[data-theme="dark"]:root{
      --bg:#0f0f10; --surface:#151515; --surface2:#1b1b1b; --text:#f3f2f1; --muted:#c8c6c4;
      --border:#2a2a2a; --border2:#343434;
      --accent:#4cc2ff; --accent2:#2aa0df;
      --danger:#ff5a5f; --danger2:#d64545;
      --shadow: 0 1px 2px rgba(0,0,0,.35), 0 10px 26px rgba(0,0,0,.35);
      --shadow2: 0 1px 2px rgba(0,0,0,.25);
      --focus: 0 0 0 3px rgba(76,194,255,.25);
      --focus2: 0 0 0 2px rgba(76,194,255,.35);
      --hdrGrad1:#0b3a66; --hdrGrad2:#071e33;
    }

    *{ box-sizing:border-box; }
    html,body{ height:100%; }
    body{
      margin:0;
      background:var(--bg);
      color:var(--text);
      font:14px/1.45 var(--font);
    }

    header{
      position:sticky;
      top:0;
      z-index:50;
      backdrop-filter:saturate(140%) blur(10px);
      background: color-mix(in srgb, var(--bg) 70%, transparent);
      border-bottom:1px solid var(--border);
    }

    .topBrand{
      background: linear-gradient(90deg, var(--hdrGrad1), var(--hdrGrad2));
      color:#fff;
    }

    .wrap{
      max-width:1100px;
      margin:0 auto;
      padding:0 16px;
    }

    .brandRow{
      display:flex;
      align-items:center;
      gap:12px;
      padding:12px 0;
    }

    /* ===== Exact logo mark (SVG) – fixed size, no distortion ===== */
    .logoMark{
      background:#fff;
      border-radius:12px;
      padding:6px 10px;
      border:1px solid rgba(255,255,255,.25);
      box-shadow:0 6px 16px rgba(0,0,0,.18);
      display:flex;
      align-items:center;
      flex:0 0 auto;
    }
    .logoMark svg{
      height:36px;
      width:auto;
      display:block;
    }

    .brandText{
      display:flex;
      flex-direction:column;
      line-height:1.1;
      gap:2px;
      min-width:0;
    }
    .brandTitle{
      font-size:16px;
      font-weight:800;
      white-space:nowrap;
      overflow:hidden;
      text-overflow:ellipsis;
    }
    .brandSub{
      font-size:12px;
      opacity:.9;
      white-space:nowrap;
      overflow:hidden;
      text-overflow:ellipsis;
    }

    /* ===== Shared details styling + chevron ===== */
    details>summary{ list-style:none; cursor:pointer; }
    details>summary::-webkit-details-marker{ display:none; }
    .chev{
      width:18px; height:18px;
      border-radius:6px;
      border:1px solid var(--border2);
      background:var(--surface2);
      display:grid;
      place-items:center;
      flex:0 0 auto;
    }
    .chev::before{
      content:"";
      width:7px; height:7px;
      border-right:2px solid var(--muted);
      border-bottom:2px solid var(--muted);
      transform: rotate(-45deg);
      transition: transform .15s ease;
      margin-top: 1px;
    }
    details[open] .chev::before{
      transform: rotate(45deg);
      margin-top: -1px;
    }

    /* ===== Storage & System (Collapsible, top) ===== */
    .storageTop{ padding:10px 0 0; }

    .storageSummary{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      padding:10px 12px;
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:var(--radius);
      box-shadow:var(--shadow2);
      user-select:none;
    }
    .storageSummaryLeft{
      display:flex;
      align-items:center;
      gap:10px;
      min-width:0;
      flex:1 1 auto;
    }
    .storageTitle{
      font-weight:800;
      white-space:nowrap;
    }
    .storageMini{
      color:var(--muted);
      font-size:12px;
      white-space:nowrap;
      overflow:hidden;
      text-overflow:ellipsis;
      min-width:0;
    }
    .storageSummaryRight{
      display:flex;
      align-items:center;
      gap:8px;
      flex-wrap:wrap;
      justify-content:flex-end;
      flex:0 0 auto;
    }
    .storageBody{
      margin-top:10px;
      padding-bottom:10px;
    }

    .statusBar{
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      align-items:center;
      justify-content:space-between;
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:var(--radius);
      box-shadow:var(--shadow2);
      padding:10px 12px;
    }
    .statusLeft,.statusRight{
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      align-items:center;
      min-width:0;
    }
    .kpi{
      display:flex;
      flex-direction:column;
      gap:2px;
      min-width:140px;
    }
    .kpi .k{ font-size:12px; color:var(--muted); }
    .kpi .v{ font-weight:700; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }

    .progress{
      width:160px; height:8px;
      border-radius:999px;
      background: color-mix(in srgb, var(--border) 80%, transparent);
      overflow:hidden;
      border:1px solid var(--border2);
    }
    .progress > i{
      display:block;
      height:100%;
      width:0%;
      background: linear-gradient(90deg, var(--accent), color-mix(in srgb, var(--accent) 65%, #fff));
    }

    .pill{
      display:inline-flex;
      align-items:center;
      gap:8px;
      padding:7px 10px;
      border-radius:999px;
      border:1px solid var(--border);
      background:var(--surface);
      color:var(--muted);
      font-size:12px;
      box-shadow:var(--shadow2);
      white-space:nowrap;
    }
    .pill strong{ color:var(--text); font-weight:700; }

    /* Inputs + selects */
    .label{ font-size:12px; color:var(--muted); white-space:nowrap; }
    input[type="text"], select{
      width:100%;
      padding:9px 10px;
      border-radius:10px;
      border:1px solid var(--border2);
      background:var(--surface2);
      color:var(--text);
      outline:none;
      transition: box-shadow .12s ease, border-color .12s ease;
    }
    input[type="text"]:focus, select:focus{
      border-color: color-mix(in srgb, var(--accent) 60%, var(--border2));
      box-shadow: var(--focus);
    }

    .miniField{ display:flex; align-items:center; gap:8px; }
    .miniField select{ width:auto; min-width:140px; padding:8px 10px; }

    /* Buttons */
    .btn{
      appearance:none;
      border:1px solid var(--border2);
      background:var(--surface2);
      color:var(--text);
      padding:9px 12px;
      border-radius:10px;
      cursor:pointer;
      transition: transform .05s ease, box-shadow .12s ease, border-color .12s ease, background .12s ease;
      user-select:none;
      white-space:nowrap;
    }
    .btn:hover{
      border-color: color-mix(in srgb, var(--accent) 55%, var(--border2));
      box-shadow: var(--focus2);
    }
    .btn:active{ transform: translateY(1px); }
    .btn:focus{ outline:none; box-shadow: var(--focus); border-color: var(--accent); }

    .btn.primary{ background:var(--accent); color:#fff; border-color:transparent; }
    .btn.primary:hover{ background:var(--accent2); }

    .btn.danger{ background:var(--danger); color:#fff; border-color:transparent; }
    .btn.danger:hover{ background:var(--danger2); box-shadow: 0 0 0 3px color-mix(in srgb, var(--danger) 25%, transparent); }

    .btn.ghost{ background:transparent; border-color:var(--border2); }
    .btn.small{ padding:7px 10px; font-size:12px; border-radius:9px; }

    /* ===== Command bar ===== */
    .cmdBar{ padding:10px 0 12px; }
    .cmdGrid{ display:grid; gap:10px; grid-template-columns: 1fr; }
    @media (min-width:820px){
      .cmdGrid{ grid-template-columns: 1.3fr 1fr; align-items:end; }
    }
    .cmdLeft,.cmdRight{
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:var(--radius);
      box-shadow:var(--shadow2);
      padding:10px;
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      align-items:center;
    }
    .cmdRight{ justify-content:flex-end; }

    /* ===== Collapsible Search & Sort ===== */
    .searchDetails{ width:100%; }
    .searchSummary{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      padding:10px 12px;
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:var(--radius);
      box-shadow:var(--shadow2);
      user-select:none;
      width:100%;
    }
    .searchSummaryLeft{
      display:flex;
      align-items:center;
      gap:10px;
      min-width:0;
      flex:1 1 auto;
    }
    .searchTitle{ font-weight:800; white-space:nowrap; }
    .searchMini{
      color:var(--muted);
      font-size:12px;
      white-space:nowrap;
      overflow:hidden;
      text-overflow:ellipsis;
      min-width:0;
    }
    .searchSummaryRight{ display:flex; align-items:center; gap:8px; flex:0 0 auto; }
    .searchBody{ margin-top:10px; display:flex; gap:10px; flex-wrap:wrap; align-items:center; }

    .field{ display:flex; align-items:center; gap:8px; min-width:160px; flex:1 1 220px; }

    /* ===== Content ===== */
    main{ max-width:1100px; margin:0 auto; padding:14px 16px 60px; }
    .empty{
      padding:22px;
      background:var(--surface);
      border:1px dashed var(--border2);
      border-radius:var(--radius);
      color:var(--muted);
      text-align:center;
    }

    /* ===== Date cards / file rows ===== */
    section.date{
      margin:12px 0;
      border:1px solid var(--border);
      border-radius:var(--radius);
      background:var(--surface);
      box-shadow:var(--shadow2);
      overflow:hidden;
    }
    .dateHdr{
      display:grid;
      grid-template-columns: 1fr auto;
      gap:12px;
      align-items:center;
      padding:12px 12px;
      border-bottom:1px solid var(--border);
      background: color-mix(in srgb, var(--surface2) 60%, var(--surface));
    }
    .dateTitle{ min-width:0; display:flex; flex-direction:column; gap:2px; }
    .dateTitle .t{ font-weight:800; overflow-wrap:anywhere; }
    .dateTitle .s{ color:var(--muted); font-size:12px; overflow-wrap:anywhere; }
    .dateActions{ display:flex; gap:8px; flex-wrap:wrap; justify-content:flex-end; align-items:center; }

    .fileList{ padding:10px 12px 6px; display:flex; flex-direction:column; gap:8px; }
    .fileRow{
      display:grid;
      grid-template-columns: 1fr auto;
      gap:12px;
      padding:10px 10px;
      border:1px solid var(--border);
      border-radius:12px;
      background:var(--surface2);
      align-items:center;
    }
    .fileLeft{ min-width:0; display:flex; gap:10px; align-items:flex-start; }
    .chk{ width:18px; height:18px; margin-top:2px; accent-color:var(--accent); cursor:pointer; flex:0 0 auto; }
    .fileMeta{ min-width:0; display:flex; flex-direction:column; gap:3px; }
    .fileName{ font-weight:650; overflow-wrap:anywhere; }
    .fileSub{ color:var(--muted); font-size:12px; display:flex; gap:10px; flex-wrap:wrap; align-items:center; }
    .tag{
      display:inline-flex;
      align-items:center;
      gap:6px;
      border:1px solid var(--border2);
      background: color-mix(in srgb, var(--surface) 60%, transparent);
      padding:2px 8px;
      border-radius:999px;
      font-size:12px;
    }
    .fileActions{ display:flex; gap:8px; flex-wrap:wrap; justify-content:flex-end; align-items:center; }

    @media (max-width:640px){
      .storageSummary{ align-items:flex-start; }
      .storageSummaryRight{ justify-content:flex-start; }
      .searchSummary{ align-items:flex-start; }
      .searchSummaryRight{ justify-content:flex-start; }
      .progress{ width:100%; }
      .kpi{ min-width:120px; }
      .dateHdr{ grid-template-columns: 1fr; }
      .dateActions{ justify-content:flex-start; }
      .fileRow{ grid-template-columns: 1fr; }
      .fileActions{ justify-content:flex-start; }
    }

    /* Toast */
    .toast{
      position:fixed;
      left:50%;
      bottom:16px;
      transform:translateX(-50%);
      background:var(--surface);
      border:1px solid var(--border);
      border-radius:999px;
      box-shadow:var(--shadow);
      padding:10px 14px;
      color:var(--text);
      display:none;
      align-items:center;
      gap:10px;
      max-width: min(720px, calc(100vw - 24px));
      z-index:999;
    }
    .toast.show{ display:flex; }
    .toast .muted{ color:var(--muted); }
  </style>
</head>

<body>
<header>
  <div class="topBrand">
    <div class="wrap">
      <div class="brandRow">

        <!-- DIDAKTA HORVAT logo mark (SVG) -->
        <div class="logoMark" aria-label="DIDAKTA HORVAT logo">
          <svg viewBox="-368 -302 75 26" preserveAspectRatio="xMinYMid meet" role="img" aria-label="DIDAKTA HORVAT">
            <rect x="-367.06281" y="-300.2009" width="18.775064" height="17.732008" rx="2.5536675" ry="2.5206232" fill="#214478"/>
            <path d="m -366.55412,-291.28909
                     c 1.11755,0 0.91096,2.58138 2.06964,2.5771
                     1.61406,-0.007 1.78439,-7.36038 3.45828,-7.28687
                     2.12505,0.0933 2.00427,9.26417 3.4747,9.26236
                     1.31666,0 1.50081,-4.45116 2.15323,-4.56
                     0.19538,-0.0327 0.46827,-0.016 1.50609,-0.0614"
                  fill="none" stroke="#f4f4f4" stroke-width="1.13386"
                  stroke-linecap="round" stroke-linejoin="round"/>
            <ellipse cx="-352.33844" cy="-295.9444" rx="0.98116046" ry="0.9811604" fill="none" stroke="#f4f4f4" stroke-width="0.525355"/>
            <ellipse cx="-352.33844" cy="-291.3754" rx="0.98116046" ry="0.9811604" fill="none" stroke="#f4f4f4" stroke-width="0.526526"/>
            <ellipse cx="-352.33844" cy="-286.80649" rx="0.98116046" ry="0.9811604" fill="none" stroke="#f4f4f4" stroke-width="0.526526"/>
            <text x="-346.23972" y="-291.74951" font-family="Arial, Segoe UI, sans-serif" font-size="7.84928" font-weight="800" letter-spacing="0.19418" fill="#214478">DIDAKTA</text>
            <text x="-346.14859" y="-285.16083" font-family="Arial, Segoe UI, sans-serif" font-size="5.88697" font-weight="500" letter-spacing="0.6734" fill="#606060">HORVAT</text>
          </svg>
        </div>

        <div class="brandText">
          <div class="brandTitle">Chronos Exports</div>
          <div class="brandSub">Enterprise export repository • Secure AP portal</div>
        </div>
      </div>
    </div>
  </div>

  <!-- Storage & System (collapsible, TOP) -->
  <div class="wrap storageTop">
    <details id="storageDetails" open>
      <summary class="storageSummary" aria-controls="storageBody">
        <div class="storageSummaryLeft">
          <span class="chev" aria-hidden="true"></span>
          <span class="storageTitle" data-i18n="Storage & System"></span>
          <span class="storageMini" id="stStorageMini">—</span>
        </div>
        <div class="storageSummaryRight">
          <span class="pill" title="Build tag"><span data-i18n="Build"></span>: <strong id="stBuild">APWEB_UI</strong></span>
          <span class="pill" title="Last refresh time"><span data-i18n="Updated"></span>: <strong id="stTime">—</strong></span>
        </div>
      </summary>

      <div class="storageBody" id="storageBody">
        <div class="statusBar" role="status" aria-live="polite">
          <div class="statusLeft">
            <div class="kpi">
              <div class="k" data-i18n="Storage"></div>
              <div class="v" id="stStorage">—</div>
            </div>

            <div class="kpi" style="min-width:180px;">
              <div class="k" data-i18n="Usage"></div>
              <div class="v"><div class="progress" aria-hidden="true"><i id="stProg"></i></div></div>
            </div>

            <div class="kpi">
              <div class="k" data-i18n="Items"></div>
              <div class="v" id="stItems">—</div>
            </div>
          </div>

          <div class="statusRight">
            <span class="pill" title="UI version">UI: <strong id="stUI">Fluent</strong></span>

            <!-- Theme selector INSIDE Storage & System -->
            <div class="miniField" title="Choose theme">
              <span class="label" data-i18n="Theme"></span>
              <select id="themeSel">
                <option value="system" data-i18n-opt="System"></option>
                <option value="light" data-i18n-opt="Light"></option>
                <option value="dark" data-i18n-opt="Dark"></option>
              </select>
            </div>

            <span class="pill" title="Theme mode"><span data-i18n="Mode"></span>: <strong id="stThemeLabel">System</strong></span>
          </div>
        </div>
      </div>
    </details>
  </div>

  <!-- Commands: Search collapsible (auto-collapsed on mobile) + actions -->
  <div class="wrap cmdBar">
    <div class="cmdGrid">

      <div class="cmdLeft" role="region" aria-label="Search and sort">
        <details id="searchDetails" class="searchDetails">
          <summary class="searchSummary" aria-controls="searchBody">
            <div class="searchSummaryLeft">
              <span class="chev" aria-hidden="true"></span>
              <span class="searchTitle" data-i18n="Search & Sort"></span>
              <span class="searchMini" id="searchMini">—</span>
            </div>
            <div class="searchSummaryRight">
              <button class="btn small ghost" id="btnClearSearch" type="button" data-i18n="Clear"></button>
            </div>
          </summary>

          <div class="searchBody" id="searchBody">
            <div class="field" style="flex:2 1 320px;">
              <span class="label" data-i18n="Search"></span>
              <input id="q" type="text" placeholder="" autocomplete="off"/>
            </div>

            <div class="field" style="flex:1 1 240px;">
              <span class="label" data-i18n="Sort"></span>
              <select id="sortKey">
                <option value="date_desc" data-i18n-opt="Date (newest)"></option>
                <option value="date_asc" data-i18n-opt="Date (oldest)"></option>
                <option value="name_asc" data-i18n-opt="File name (A→Z)"></option>
                <option value="name_desc" data-i18n-opt="File name (Z→A)"></option>
                <option value="size_desc" data-i18n-opt="Size (largest)"></option>
                <option value="size_asc" data-i18n-opt="Size (smallest)"></option>
                <option value="mode_asc" data-i18n-opt="Mode (A→Z)"></option>
                <option value="mode_desc" data-i18n-opt="Mode (Z→A)"></option>
              </select>
            </div>
          </div>
        </details>
      </div>

      <div class="cmdRight" role="region" aria-label="Actions">
        <button class="btn" id="btnRefresh" type="button" data-i18n="Refresh"></button>
        <button class="btn ghost" id="btnExpandAll" type="button" data-i18n="Expand"></button>
        <button class="btn ghost" id="btnCollapseAll" type="button" data-i18n="Collapse"></button>
        <button class="btn" id="btnSelectAll" type="button" data-i18n="Select all"></button>
        <button class="btn" id="btnSelectNone" type="button" data-i18n="Clear"></button>
        <button class="btn primary" id="btnBulkDownload" type="button" data-i18n="Download selected"></button>
        <button class="btn danger" id="btnBulkDelete" type="button" data-i18n="Delete selected"></button>
        <span class="pill" title="Selected items"><span data-i18n="Selected"></span>: <strong id="selCount">0</strong></span>
      </div>

    </div>
  </div>
</header>

<main>
  <div id="content" class="empty" data-i18n-text="Loading…">Loading…</div>
</main>

<div class="toast" id="toast"><strong id="toastTitle" data-i18n-text="Working…">Working…</strong> <span class="muted" id="toastMsg"></span></div>

<script>
(() => {
  const UI_BUILD = 'APWEB_UI';

  /* ================= i18n ================= */
  const I18N = {
    en: {
      "Storage & System":"Storage & System",
      "Build":"Build",
      "Updated":"Updated",
      "Storage":"Storage",
      "Usage":"Usage",
      "Items":"Items",
      "Theme":"Theme",
      "Mode":"Mode",
      "System":"System",
      "Light":"Light",
      "Dark":"Dark",
      "Search & Sort":"Search & Sort",
      "Search":"Search",
      "Sort":"Sort",
      "Clear":"Clear",
      "Refresh":"Refresh",
      "Expand":"Expand",
      "Collapse":"Collapse",
      "Select all":"Select all",
      "Download selected":"Download selected",
      "Delete selected":"Delete selected",
      "Selected":"Selected",
      "Loading…":"Loading…",
      "No exports yet":"No exports yet",
      "No matches. Try a different search.":"No matches. Try a different search.",
      "Failed to load list":"Failed to load list",
      "Deleting…":"Deleting…",
      "Downloading…":"Downloading…",
      "Select day":"Select day",
      "Download day (.zip)":"Download day (.zip)",
      "Delete day":"Delete day",
      "Download":"Download",
      "Delete":"Delete",
      "No filter":"No filter",
      "Date (newest)":"Date (newest)",
      "Date (oldest)":"Date (oldest)",
      "File name (A→Z)":"File name (A→Z)",
      "File name (Z→A)":"File name (Z→A)",
      "Size (largest)":"Size (largest)",
      "Size (smallest)":"Size (smallest)",
      "Mode (A→Z)":"Mode (A→Z)",
      "Mode (Z→A)":"Mode (Z→A)",
      "Search by file, mode, or date…":"Search by file, mode, or date…",
      "No files selected.":"No files selected.",
      "Selected files are not currently visible (search/sort filter). Clear filters and try again.":"Selected files are not currently visible (search/sort filter). Clear filters and try again.",
      "Delete all files for {date} ?":"Delete all files for {date} ?",
      "Delete {name} ?":"Delete {name} ?",
      "Delete {n} selected file(s)?":"Delete {n} selected file(s)?",
      "Refreshing…":"Refreshing…",
      "Updating list and status":"Updating list and status",
      "Fetching status and exports":"Fetching status and exports",
      "File {i} / {n}":"File {i} / {n}",
      "Working…":"Working…",
      "FS not mounted":"FS not mounted",
      "Status unavailable":"Status unavailable",
      "Delete failed":"Delete failed",
      "Bulk delete failed":"Bulk delete failed"
    },
    hr: {
      "Storage & System":"Pohrana i sustav",
      "Build":"Build",
      "Updated":"Ažurirano",
      "Storage":"Pohrana",
      "Usage":"Upotreba",
      "Items":"Stavke",
      "Theme":"Tema",
      "Mode":"Način",
      "System":"Sustav",
      "Light":"Svijetla",
      "Dark":"Tamna",
      "Search & Sort":"Pretraga i sortiranje",
      "Search":"Pretraga",
      "Sort":"Sortiranje",
      "Clear":"Očisti",
      "Refresh":"Osvježi",
      "Expand":"Proširi",
      "Collapse":"Sažmi",
      "Select all":"Odaberi sve",
      "Download selected":"Preuzmi odabrano",
      "Delete selected":"Obriši odabrano",
      "Selected":"Odabrano",
      "Loading…":"Učitavanje…",
      "No exports yet":"Još nema izvoza",
      "No matches. Try a different search.":"Nema rezultata. Pokušajte drugu pretragu.",
      "Failed to load list":"Neuspjelo učitavanje popisa",
      "Deleting…":"Brisanje…",
      "Downloading…":"Preuzimanje…",
      "Select day":"Odaberi dan",
      "Download day (.zip)":"Preuzmi dan (.zip)",
      "Delete day":"Obriši dan",
      "Download":"Preuzmi",
      "Delete":"Obriši",
      "No filter":"Bez filtra",
      "Date (newest)":"Datum (najnovije)",
      "Date (oldest)":"Datum (najstarije)",
      "File name (A→Z)":"Naziv (A→Z)",
      "File name (Z→A)":"Naziv (Z→A)",
      "Size (largest)":"Veličina (najveće)",
      "Size (smallest)":"Veličina (najmanje)",
      "Mode (A→Z)":"Način (A→Z)",
      "Mode (Z→A)":"Način (Z→A)",
      "Search by file, mode, or date…":"Pretraži po datoteci, načinu ili datumu…",
      "No files selected.":"Nema odabranih datoteka.",
      "Selected files are not currently visible (search/sort filter). Clear filters and try again.":"Odabrane datoteke nisu vidljive (filter pretrage/sortiranja). Očistite filtere i pokušajte ponovno.",
      "Delete all files for {date} ?":"Obrisati sve datoteke za {date} ?",
      "Delete {name} ?":"Obrisati {name} ?",
      "Delete {n} selected file(s)?":"Obrisati {n} odabranih datoteka?",
      "Refreshing…":"Osvježavanje…",
      "Updating list and status":"Ažuriranje popisa i statusa",
      "Fetching status and exports":"Dohvaćanje statusa i izvoza",
      "File {i} / {n}":"Datoteka {i} / {n}",
      "Working…":"Radim…",
      "FS not mounted":"Datotečni sustav nije montiran",
      "Status unavailable":"Status nedostupan",
      "Delete failed":"Brisanje nije uspjelo",
      "Bulk delete failed":"Skupno brisanje nije uspjelo"
    },
    de: {
      "Storage & System":"Speicher & System",
      "Build":"Build",
      "Updated":"Aktualisiert",
      "Storage":"Speicher",
      "Usage":"Belegung",
      "Items":"Elemente",
      "Theme":"Design",
      "Mode":"Modus",
      "System":"System",
      "Light":"Hell",
      "Dark":"Dunkel",
      "Search & Sort":"Suchen & Sortieren",
      "Search":"Suchen",
      "Sort":"Sortieren",
      "Clear":"Löschen",
      "Refresh":"Aktualisieren",
      "Expand":"Erweitern",
      "Collapse":"Reduzieren",
      "Select all":"Alle auswählen",
      "Download selected":"Auswahl herunterladen",
      "Delete selected":"Auswahl löschen",
      "Selected":"Ausgewählt",
      "Loading…":"Lädt…",
      "No exports yet":"Noch keine Exporte",
      "No matches. Try a different search.":"Keine Treffer. Bitte anders suchen.",
      "Failed to load list":"Liste konnte nicht geladen werden",
      "Deleting…":"Löschen…",
      "Downloading…":"Herunterladen…",
      "Select day":"Tag auswählen",
      "Download day (.zip)":"Tag herunterladen (.zip)",
      "Delete day":"Tag löschen",
      "Download":"Herunterladen",
      "Delete":"Löschen",
      "No filter":"Kein Filter",
      "Date (newest)":"Datum (neueste)",
      "Date (oldest)":"Datum (älteste)",
      "File name (A→Z)":"Dateiname (A→Z)",
      "File name (Z→A)":"Dateiname (Z→A)",
      "Size (largest)":"Größe (größte)",
      "Size (smallest)":"Größe (kleinste)",
      "Mode (A→Z)":"Modus (A→Z)",
      "Mode (Z→A)":"Modus (Z→A)",
      "Search by file, mode, or date…":"Suche nach Datei, Modus oder Datum…",
      "No files selected.":"Keine Dateien ausgewählt.",
      "Selected files are not currently visible (search/sort filter). Clear filters and try again.":"Ausgewählte Dateien sind derzeit nicht sichtbar (Such-/Sortierfilter). Filter löschen und erneut versuchen.",
      "Delete all files for {date} ?":"Alle Dateien für {date} löschen?",
      "Delete {name} ?":"{name} löschen?",
      "Delete {n} selected file(s)?":"{n} ausgewählte Datei(en) löschen?",
      "Refreshing…":"Aktualisieren…",
      "Updating list and status":"Liste und Status werden aktualisiert",
      "Fetching status and exports":"Status und Exporte werden geladen",
      "File {i} / {n}":"Datei {i} / {n}",
      "Working…":"Arbeite…",
      "FS not mounted":"Dateisystem nicht eingehängt",
      "Status unavailable":"Status nicht verfügbar",
      "Delete failed":"Löschen fehlgeschlagen",
      "Bulk delete failed":"Sammellöschen fehlgeschlagen"
    },
    fr: {
      "Storage & System":"Stockage & système",
      "Build":"Build",
      "Updated":"Mis à jour",
      "Storage":"Stockage",
      "Usage":"Utilisation",
      "Items":"Éléments",
      "Theme":"Thème",
      "Mode":"Mode",
      "System":"Système",
      "Light":"Clair",
      "Dark":"Sombre",
      "Search & Sort":"Rechercher & trier",
      "Search":"Rechercher",
      "Sort":"Trier",
      "Clear":"Effacer",
      "Refresh":"Actualiser",
      "Expand":"Développer",
      "Collapse":"Réduire",
      "Select all":"Tout sélectionner",
      "Download selected":"Télécharger la sélection",
      "Delete selected":"Supprimer la sélection",
      "Selected":"Sélectionné",
      "Loading…":"Chargement…",
      "No exports yet":"Aucun export pour le moment",
      "No matches. Try a different search.":"Aucun résultat. Essayez une autre recherche.",
      "Failed to load list":"Échec du chargement de la liste",
      "Deleting…":"Suppression…",
      "Downloading…":"Téléchargement…",
      "Select day":"Sélectionner le jour",
      "Download day (.zip)":"Télécharger le jour (.zip)",
      "Delete day":"Supprimer le jour",
      "Download":"Télécharger",
      "Delete":"Supprimer",
      "No filter":"Aucun filtre",
      "Date (newest)":"Date (plus récent)",
      "Date (oldest)":"Date (plus ancien)",
      "File name (A→Z)":"Nom (A→Z)",
      "File name (Z→A)":"Nom (Z→A)",
      "Size (largest)":"Taille (plus grand)",
      "Size (smallest)":"Taille (plus petit)",
      "Mode (A→Z)":"Mode (A→Z)",
      "Mode (Z→A)":"Mode (Z→A)",
      "Search by file, mode, or date…":"Rechercher par fichier, mode ou date…",
      "No files selected.":"Aucun fichier sélectionné.",
      "Selected files are not currently visible (search/sort filter). Clear filters and try again.":"Les fichiers sélectionnés ne sont pas visibles (filtre recherche/tri). Effacez les filtres et réessayez.",
      "Delete all files for {date} ?":"Supprimer tous les fichiers pour {date} ?",
      "Delete {name} ?":"Supprimer {name} ?",
      "Delete {n} selected file(s)?":"Supprimer {n} fichier(s) sélectionné(s) ?",
      "Refreshing…":"Actualisation…",
      "Updating list and status":"Mise à jour de la liste et du statut",
      "Fetching status and exports":"Chargement du statut et des exports",
      "File {i} / {n}":"Fichier {i} / {n}",
      "Working…":"En cours…",
      "FS not mounted":"Système de fichiers non monté",
      "Status unavailable":"Statut indisponible",
      "Delete failed":"Suppression échouée",
      "Bulk delete failed":"Suppression groupée échouée"
    },
    es: {
      "Storage & System":"Almacenamiento y sistema",
      "Build":"Build",
      "Updated":"Actualizado",
      "Storage":"Almacenamiento",
      "Usage":"Uso",
      "Items":"Elementos",
      "Theme":"Tema",
      "Mode":"Modo",
      "System":"Sistema",
      "Light":"Claro",
      "Dark":"Oscuro",
      "Search & Sort":"Buscar y ordenar",
      "Search":"Buscar",
      "Sort":"Ordenar",
      "Clear":"Borrar",
      "Refresh":"Actualizar",
      "Expand":"Expandir",
      "Collapse":"Contraer",
      "Select all":"Seleccionar todo",
      "Download selected":"Descargar selección",
      "Delete selected":"Eliminar selección",
      "Selected":"Seleccionado",
      "Loading…":"Cargando…",
      "No exports yet":"Aún no hay exportaciones",
      "No matches. Try a different search.":"Sin resultados. Pruebe otra búsqueda.",
      "Failed to load list":"No se pudo cargar la lista",
      "Deleting…":"Eliminando…",
      "Downloading…":"Descargando…",
      "Select day":"Seleccionar día",
      "Download day (.zip)":"Descargar día (.zip)",
      "Delete day":"Eliminar día",
      "Download":"Descargar",
      "Delete":"Eliminar",
      "No filter":"Sin filtro",
      "Date (newest)":"Fecha (más reciente)",
      "Date (oldest)":"Fecha (más antigua)",
      "File name (A→Z)":"Nombre (A→Z)",
      "File name (Z→A)":"Nombre (Z→A)",
      "Size (largest)":"Tamaño (mayor)",
      "Size (smallest)":"Tamaño (menor)",
      "Mode (A→Z)":"Modo (A→Z)",
      "Mode (Z→A)":"Modo (Z→A)",
      "Search by file, mode, or date…":"Buscar por archivo, modo o fecha…",
      "No files selected.":"No hay archivos seleccionados.",
      "Selected files are not currently visible (search/sort filter). Clear filters and try again.":"Los archivos seleccionados no están visibles (filtro buscar/ordenar). Borre filtros e inténtelo de nuevo.",
      "Delete all files for {date} ?":"¿Eliminar todos los archivos de {date}?",
      "Delete {name} ?":"¿Eliminar {name}?",
      "Delete {n} selected file(s)?":"¿Eliminar {n} archivo(s) seleccionado(s)?",
      "Refreshing…":"Actualizando…",
      "Updating list and status":"Actualizando lista y estado",
      "Fetching status and exports":"Cargando estado y exportaciones",
      "File {i} / {n}":"Archivo {i} / {n}"
    }
  };

  const LANG_KEY = 'apweb_lang';

  function detectLang(){
    const p = new URLSearchParams(location.search);
    const q = (p.get('lang')||'').toLowerCase();
    if(I18N[q]) return q;
    const saved = (localStorage.getItem(LANG_KEY)||'').toLowerCase();
    if(I18N[saved]) return saved;
    const nav = (navigator.language||'en').toLowerCase();
    if(nav.startsWith('hr')) return 'hr';
    if(nav.startsWith('de')) return 'de';
    if(nav.startsWith('fr')) return 'fr';
    if(nav.startsWith('es')) return 'es';
    return 'en';
  }

  let LANG = detectLang();

  function t(key){
    return (I18N[LANG] && I18N[LANG][key]) || I18N.en[key] || key;
  }
  function tf(key, vars){
    let s = t(key);
    if(vars){
      for(const k in vars){
        s = s.replaceAll('{'+k+'}', String(vars[k]));
      }
    }
    return s;
  }

  function applyStaticI18n(){
    document.querySelectorAll('[data-i18n]').forEach(el=>{ el.textContent = t(el.getAttribute('data-i18n')); });
    document.querySelectorAll('[data-i18n-text]').forEach(el=>{ el.textContent = t(el.getAttribute('data-i18n-text')); });
    document.querySelectorAll('[data-i18n-opt]').forEach(el=>{ el.textContent = t(el.getAttribute('data-i18n-opt')); });
    const q = document.querySelector('#q');
    if(q) q.placeholder = t('Search by file, mode, or date…');
    // Theme select option labels
    const themeSel = document.querySelector('#themeSel');
    if(themeSel){
      themeSel.querySelectorAll('option').forEach(o=>{
        const k=o.getAttribute('data-i18n-opt');
        if(k) o.textContent = t(k);
      });
    }
    // Update mode label display
    const stThemeLabel = document.querySelector('#stThemeLabel');
    if(stThemeLabel){
      const v = stThemeLabel.textContent.trim().toLowerCase();
      if(v==='system') stThemeLabel.textContent = t('System');
      else if(v==='light') stThemeLabel.textContent = t('Light');
      else if(v==='dark') stThemeLabel.textContent = t('Dark');
    }
  }

  applyStaticI18n();

  /* ================= App logic (your existing UI) ================= */

  const content = document.querySelector('#content');
  const qEl = document.querySelector('#q');
  const sortEl = document.querySelector('#sortKey');
  const themeSel = document.querySelector('#themeSel');

  const selCountEl = document.querySelector('#selCount');

  const stStorage = document.querySelector('#stStorage');
  const stStorageMini = document.querySelector('#stStorageMini');
  const stProg = document.querySelector('#stProg');
  const stItems = document.querySelector('#stItems');
  const stBuild = document.querySelector('#stBuild');
  const stTime = document.querySelector('#stTime');
  const stThemeLabel = document.querySelector('#stThemeLabel');

  const toast = document.querySelector('#toast');
  const toastTitle = document.querySelector('#toastTitle');
  const toastMsg = document.querySelector('#toastMsg');

  stBuild.textContent = UI_BUILD;

  const state = {
    raw: null,
    view: null,
    selected: new Set(),
    lastQuery: '',
    lastSort: 'date_desc'
  };

  const fmtBytes = (n) => {
    const u = ['B','KB','MB','GB','TB'];
    let i=0, x=Number(n||0);
    while(x>=1024 && i<u.length-1){ x/=1024; i++; }
    return (i?x.toFixed(1):(x|0))+' '+u[i];
  };

  const formatEU = (iso) => {
    try{
      const d = new Date(iso+'T00:00:00');
      return d.toLocaleDateString(undefined,{weekday:'short',year:'numeric',month:'2-digit',day:'2-digit'});
    }catch(e){ return iso; }
  };

  const nowStamp = () => {
    try{
      const d=new Date();
      return d.toLocaleString(undefined,{year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit'});
    }catch(e){ return '—'; }
  };

  async function getJSON(url){
    const r = await fetch(url,{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    return r.json();
  }

  function showToast(title,msg){
    toastTitle.textContent = title||'…';
    toastMsg.textContent = msg||'';
    toast.classList.add('show');
  }
  function hideToast(){ toast.classList.remove('show'); }

  function keyOf(date,name){ return date+'|'+name; }

  function parseSizeBytes(file){
    if(typeof file.bytes!=='undefined') return Number(file.bytes||0);
    const s=String(file.size||'').trim();
    const m=s.match(/^([\d.]+)\s*(B|KB|MB|GB|TB)$/i);
    if(!m) return 0;
    const v=parseFloat(m[1]||'0');
    const unit=(m[2]||'B').toUpperCase();
    const mult={B:1,KB:1024,MB:1024*1024,GB:1024*1024*1024,TB:1024*1024*1024*1024}[unit]||1;
    return Math.round(v*mult);
  }

  function normalizeList(raw){
    const dates = Array.isArray(raw && raw.dates) ? raw.dates : [];
    return dates.map(d=>{
      const files = Array.isArray(d.files)? d.files : [];
      return {
        date: String(d.date||''),
        files: files.map(f=>({
          name: String(f.name||''),
          mode: String(f.mode||''),
          size: String(f.size || (typeof f.bytes!=='undefined' ? fmtBytes(f.bytes) : '')),
          bytes: parseSizeBytes(f),
          href: String(f.href||'')
        }))
      };
    }).filter(d=>d.date);
  }

  function applyQueryAndSort(){
    const q=String(qEl.value||'').trim().toLowerCase();
    const sort=String(sortEl.value||'date_desc');

    state.lastQuery=q;
    state.lastSort=sort;

    if(!state.raw){ state.view=null; return; }

    let groups = normalizeList(state.raw);

    if(q){
      groups = groups.map(g=>{
        const dateHit = g.date.toLowerCase().includes(q) || formatEU(g.date).toLowerCase().includes(q);
        const files = g.files.filter(f=>{
          const hay=(f.name+' '+f.mode+' '+g.date).toLowerCase();
          return dateHit || hay.includes(q);
        });
        return {date:g.date, files};
      }).filter(g=>g.files.length>0);
    }

    const parts=sort.split('_');
    const k=parts[0]||'date';
    const dir=parts[1]||'desc';

    const asc=(a,b)=>(a<b?-1:a>b?1:0);
    const cmpStr=(a,b)=>asc(String(a||'').toLowerCase(), String(b||'').toLowerCase());
    const cmpNum=(a,b)=>(Number(a||0)-Number(b||0));

    if(k==='date'){
      groups.sort((A,B)=>(dir==='asc'?asc(A.date,B.date):asc(B.date,A.date)));
    }else{
      groups.sort((A,B)=>asc(B.date,A.date));
      groups = groups.map(g=>{
        const files=[...g.files];
        files.sort((a,b)=>{
          let c=0;
          if(k==='name') c=cmpStr(a.name,b.name);
          else if(k==='size') c=cmpNum(a.bytes,b.bytes);
          else if(k==='mode') c=cmpStr(a.mode,b.mode);
          return (dir==='asc')?c:-c;
        });
        return {date:g.date, files};
      });
    }

    state.view=groups;
  }

  function updateSelectionCount(){
    selCountEl.textContent=String(state.selected.size);
  }

  function updateItemsKPI(){
    const groups = state.view || normalizeList(state.raw||{});
    let nFiles=0, nDays=0;
    for(const g of groups){ nDays++; nFiles += (g.files?g.files.length:0); }
    stItems.textContent = `${nFiles} files • ${nDays} days`;
  }

  function render(){
    applyQueryAndSort();

    const groups=state.view;
    if(!groups || groups.length===0){
      content.className='empty';
      content.textContent = state.lastQuery ? t('No matches. Try a different search.') : t('No exports yet');
      updateSelectionCount();
      updateItemsKPI();
      return;
    }

    content.className='';
    content.innerHTML='';

    for(const entry of groups){
      const date=entry.date;
      const totalBytes = entry.files.reduce((s,f)=>s+Number(f.bytes||0),0);
      const sumText = `${entry.files.length} file(s) • ${fmtBytes(totalBytes)}`;

      const sec=document.createElement('section'); sec.className='date';
      const det=document.createElement('details'); det.open=false;

      const sum=document.createElement('summary'); sum.className='dateHdr';

      const left=document.createElement('div'); left.className='dateTitle';
      const t1=document.createElement('div'); t1.className='t'; t1.textContent=formatEU(date);
      const s1=document.createElement('div'); s1.className='s'; s1.textContent=`${date} • ${sumText}`;
      left.append(t1,s1);

      const actions=document.createElement('div'); actions.className='dateActions';

      const mkBtn=(label,cls)=>{
        const b=document.createElement('button');
        b.type='button';
        b.className=cls||'btn small';
        b.textContent=label;
        return b;
      };

      const bSelectDay=mkBtn(t('Select day'),'btn small');
      bSelectDay.addEventListener('click',(e)=>{
        e.preventDefault(); e.stopPropagation();
        for(const f of entry.files) state.selected.add(keyOf(date,f.name));
        updateSelectionCount();
        sec.querySelectorAll('input[type="checkbox"][data-k]').forEach(cb=>cb.checked=state.selected.has(cb.dataset.k));
      });

      const bZip=mkBtn(t('Download day (.zip)'),'btn small primary');
      bZip.addEventListener('click',(e)=>{
        e.preventDefault(); e.stopPropagation();
        window.location.href='/zip?date='+encodeURIComponent(date);
      });

      const bDel=mkBtn(t('Delete day'),'btn small danger');
      bDel.addEventListener('click', async (e)=>{
        e.preventDefault(); e.stopPropagation();
        if(!confirm(tf('Delete all files for {date} ?', {date})) ) return;
        showToast(t('Deleting…'), tf('Delete day',{}));
        try{
          await fetch('/api/rm?date='+encodeURIComponent(date),{cache:'no-store'});
          for(const f of entry.files) state.selected.delete(keyOf(date,f.name));
          await loadList();
        }catch(err){
          alert(t('Delete failed'));
        }finally{ hideToast(); }
      });

      actions.append(bSelectDay,bZip,bDel);
      sum.append(left,actions);

      const list=document.createElement('div'); list.className='fileList';

      for(const f of entry.files){
        const row=document.createElement('div'); row.className='fileRow';

        const fileLeft=document.createElement('div'); fileLeft.className='fileLeft';
        const cb=document.createElement('input'); cb.type='checkbox'; cb.className='chk';
        cb.dataset.k=keyOf(date,f.name);
        cb.checked=state.selected.has(cb.dataset.k);
        cb.addEventListener('change',()=>{
          if(cb.checked) state.selected.add(cb.dataset.k);
          else state.selected.delete(cb.dataset.k);
          updateSelectionCount();
        });

        const meta=document.createElement('div'); meta.className='fileMeta';
        const name=document.createElement('div'); name.className='fileName'; name.textContent=f.name;

        const sub=document.createElement('div'); sub.className='fileSub';
        const tagSize=document.createElement('span'); tagSize.className='tag';
        tagSize.textContent = f.size || fmtBytes(f.bytes||0);
        sub.append(tagSize);

        if(f.mode){
          const tagMode=document.createElement('span'); tagMode.className='tag';
          tagMode.textContent=f.mode;
          sub.append(tagMode);
        }

        meta.append(name,sub);
        fileLeft.append(cb,meta);

        const fileActions=document.createElement('div'); fileActions.className='fileActions';

        const bDl=mkBtn(t('Download'),'btn small primary');
        bDl.addEventListener('click',(e)=>{
          e.preventDefault(); e.stopPropagation();
          window.location.href=f.href;
        });

        const bRm=mkBtn(t('Delete'),'btn small danger');
        bRm.addEventListener('click', async (e)=>{
          e.preventDefault(); e.stopPropagation();
          if(!confirm(tf('Delete {name} ?', {name: f.name}))) return;
          showToast(t('Deleting…'), f.name);
          try{
            await fetch('/api/rm?f='+encodeURIComponent('/exp/'+date+'/'+f.name),{cache:'no-store'});
            state.selected.delete(keyOf(date,f.name));
            await loadList();
          }catch(err){
            alert(t('Delete failed'));
          }finally{ hideToast(); }
        });

        fileActions.append(bDl,bRm);

        row.append(fileLeft,fileActions);
        list.append(row);
      }

      det.append(sum,list);
      sec.append(det);
      content.append(sec);
    }

    updateSelectionCount();
    updateItemsKPI();
  }

  async function loadStatus(){
    try{
      const st=await getJSON('/api/status');
      if(!st || !st.ok){
        stStorage.textContent=t('FS not mounted');
        stStorageMini.textContent=t('FS not mounted');
        stProg.style.width='0%';
        return;
      }
      const total=Number(st.total||0);
      const used=Number(st.used||0);
      const pct = total>0 ? Math.max(0, Math.min(100, Math.round((used/total)*100))) : 0;
      const line = `Total ${fmtBytes(total)} • Used ${fmtBytes(used)} (${pct}%)`;
      stStorage.textContent=line;
      stStorageMini.textContent=line;
      stProg.style.width=pct+'%';
    }catch(e){
      stStorage.textContent=t('Status unavailable');
      stStorageMini.textContent=t('Status unavailable');
      stProg.style.width='0%';
    }
  }

  async function loadList(){
    try{
      const data=await getJSON('/api/list');
      state.raw=data;
      stTime.textContent=nowStamp();
      render();
    }catch(e){
      content.className='empty';
      content.textContent=t('Failed to load list');
      stTime.textContent=nowStamp();
    }
  }

  async function bulkDeleteSelected(){
    const keys=Array.from(state.selected);
    if(keys.length===0) return alert(t('No files selected.'));
    if(!confirm(tf('Delete {n} selected file(s)?',{n:keys.length}))) return;

    showToast(t('Deleting…'), keys.length+' file(s)');
    try{
      for(let i=0;i<keys.length;i++){
        const [date,name]=keys[i].split('|');
        await fetch('/api/rm?f='+encodeURIComponent('/exp/'+date+'/'+name),{cache:'no-store'});
      }
      state.selected.clear();
      await loadList();
    }catch(e){
      alert(t('Bulk delete failed'));
    }finally{
      hideToast();
      updateSelectionCount();
    }
  }

  function bulkDownloadSelected(){
    const keys=Array.from(state.selected);
    if(keys.length===0) return alert(t('No files selected.'));

    const hrefs=[];
    const view=state.view || normalizeList(state.raw||{});
    const map=new Map();
    for(const g of view) for(const f of g.files) map.set(keyOf(g.date,f.name), f.href);
    for(const k of keys){
      const href=map.get(k);
      if(href) hrefs.push(href);
    }
    if(hrefs.length===0) return alert(t('Selected files are not currently visible (search/sort filter). Clear filters and try again.'));

    showToast(t('Downloading…'), hrefs.length+' file(s)');
    let i=0;
    const tick=()=>{
      if(i>=hrefs.length){ hideToast(); return; }
      toastMsg.textContent=tf('File {i} / {n}', {i:i+1, n:hrefs.length});
      const a=document.createElement('a');
      a.href=hrefs[i++];
      a.rel='noopener';
      a.style.display='none';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(tick, 350);
    };
    tick();
  }

  /* ===== Theme selector (inside Storage & System) ===== */
  const THEME_KEY='apweb_theme';
  function applyTheme(mode){
    const html=document.documentElement;
    if(!mode || mode==='system'){
      html.removeAttribute('data-theme');
      stThemeLabel.textContent=t('System');
      return;
    }
    html.setAttribute('data-theme', mode);
    stThemeLabel.textContent = (mode==='dark') ? t('Dark') : t('Light');
  }
  function initTheme(){
    const saved=localStorage.getItem(THEME_KEY) || 'system';
    themeSel.value=saved;
    applyTheme(saved);
    themeSel.addEventListener('change', ()=>{
      const v=themeSel.value || 'system';
      localStorage.setItem(THEME_KEY, v);
      applyTheme(v);
    });
  }

  /* ===== Collapsible Search: auto-collapsed on mobile, remembered ===== */
  const searchDetails=document.querySelector('#searchDetails');
  const searchMini=document.querySelector('#searchMini');
  const btnClearSearch=document.querySelector('#btnClearSearch');
  const SEARCH_OPEN_KEY='apweb_search_open';

  function updateSearchMini(){
    const q=String(qEl.value||'').trim();
    const sort=String(sortEl.value||'').replace('_',' ');
    const qTxt=q ? `“${q}”` : t('No filter');
    searchMini.textContent = `${qTxt} • ${sort || 'date desc'}`;
  }

  function initSearchCollapse(){
    const saved=localStorage.getItem(SEARCH_OPEN_KEY);
    if(saved==='0' || saved==='1'){
      searchDetails.open = (saved==='1');
    }else{
      const isMobile = window.matchMedia && window.matchMedia('(max-width: 640px)').matches;
      searchDetails.open = !isMobile;
    }

    searchDetails.addEventListener('toggle', ()=>{
      localStorage.setItem(SEARCH_OPEN_KEY, searchDetails.open ? '1' : '0');
    });

    updateSearchMini();
  }

  btnClearSearch.addEventListener('click',(e)=>{
    e.preventDefault();
    qEl.value='';
    updateSearchMini();
    render();
  });

  qEl.addEventListener('input', ()=>{ updateSearchMini(); render(); });
  sortEl.addEventListener('change', ()=>{ updateSearchMini(); render(); });

  /* ===== Buttons ===== */
  document.querySelector('#btnRefresh').addEventListener('click', async ()=>{
    showToast(t('Refreshing…'), t('Updating list and status'));
    try{ await loadStatus(); await loadList(); }
    finally{ hideToast(); }
  });

  document.querySelector('#btnExpandAll').addEventListener('click', ()=>{
    document.querySelectorAll('main details').forEach(d=>d.open=true);
  });
  document.querySelector('#btnCollapseAll').addEventListener('click', ()=>{
    document.querySelectorAll('main details').forEach(d=>d.open=false);
  });

  document.querySelector('#btnSelectAll').addEventListener('click', ()=>{
    const view=state.view || normalizeList(state.raw||{});
    for(const g of view) for(const f of g.files) state.selected.add(keyOf(g.date,f.name));
    updateSelectionCount();
    document.querySelectorAll('input[type="checkbox"][data-k]').forEach(cb=>cb.checked=state.selected.has(cb.dataset.k));
  });

  document.querySelector('#btnSelectNone').addEventListener('click', ()=>{
    state.selected.clear();
    updateSelectionCount();
    document.querySelectorAll('input[type="checkbox"][data-k]').forEach(cb=>cb.checked=false);
  });

  document.querySelector('#btnBulkDelete').addEventListener('click', bulkDeleteSelected);
  document.querySelector('#btnBulkDownload').addEventListener('click', bulkDownloadSelected);

  /* Presence heartbeat */
  const PING_MS=4000;
  const ping=()=>fetch('/api/ping',{cache:'no-store', keepalive:true}).catch(()=>{});
  setTimeout(()=>{ ping(); setInterval(ping, PING_MS); }, 500);
  window.addEventListener('pagehide', ()=>{ try{ navigator.sendBeacon('/api/ping'); }catch(e){} }, {capture:true});

  /* Initial */
  (async ()=>{
    initTheme();
    initSearchCollapse();
    showToast(t('Loading…'), t('Fetching status and exports'));
    try{ await loadStatus(); await loadList(); }
    finally{ hideToast(); }
  })();

})();
</script>
</body>
</html>)HTML";


// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static int hexVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static String url_decode(const String& s) {
    String o;
    o.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.length()) {
            int v1 = hexVal(s[i + 1]);
            int v2 = hexVal(s[i + 2]);
            if (v1 >= 0 && v2 >= 0) {
                o += char((v1 << 4) | v2);
                i += 2;
                continue;
            }
        }
        if (c == '+') c = ' ';
        o += c;
    }
    return o;
}

static bool is_bad_path(const String& p) {
    if (!p.startsWith("/exp/")) return true;
    if (p.indexOf("..") >= 0)   return true;
    return false;
}

static bool normalize_date(const String& in, String& out) {
    if (in.length() != 10) return false;
    if (in.charAt(4) != '-' || in.charAt(7) != '-') return false;
    out = in;
    return true;
}

static inline void request_ui_nudge() { s_defer_ui_nudge = true; }

// -----------------------------------------------------------------------------
// Handlers
// -----------------------------------------------------------------------------
static void handle_index() {
    mark_presence();
    send_no_cache_headers();
    s_server->setContentLength(strlen_P(INDEX_HTML));
    s_server->send(200, "text/html", "");
    s_server->sendContent_P(INDEX_HTML);
}

static void handle_status() {
    mark_presence();
    send_no_cache_headers();
    uint64_t total = 0, used = 0;
    if (!chronos::exportfs_fs_stats(total, used)) {
        s_server->send(200, "application/json", "{\"ok\":false}");
        return;
    }
    String b = "{\"ok\":true,\"total\":";
    b += String((unsigned long long)total);
    b += ",\"used\":";
    b += String((unsigned long long)used);
    b += "}";
    s_server->send(200, "application/json", b);
}

static void handle_version() {
    mark_presence();
    send_no_cache_headers();
    String b = String("{\"ok\":true,\"ver\":\"") + k_web_tag + "\"}";
    Serial.printf("[Chronos][Web][%s] /api/version\n", k_web_tag);
    s_server->send(200, "application/json", b);
}

static void handle_purge() {
    mark_presence();
    send_no_cache_headers();
    int minMB = s_server->hasArg("minFreeMB") ? s_server->arg("minFreeMB").toInt() : 5;
    if (minMB < 1) minMB = 1;
    uint64_t freed = 0;
    bool ok = chronos::exportfs_purge_oldest_until_free((uint64_t)minMB * 1024ULL * 1024ULL, &freed);
    String b = String("{\"ok\":") + (ok ? "true" : "false") +
               ",\"freed\":" + String((unsigned long long)freed) + "}";
    s_server->send(200, "application/json", b);
}

static void handle_ping() {
    s_presence_last_ms = millis();
    s_server->send(200, "application/json", "{\"ok\":true}");
}

static void handle_list() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    FsBusyGuard _busy;
    if (s_list_busy) {
        s_server->send(200, "application/json", "{\"dates\":[]}");
        return;
    }
    s_list_busy = true;
    send_no_cache_headers();
    ChronosSdSelectGuard sd;
    if (!chronos::exportfs_begin()) {
        s_list_busy = false;
        s_server->send(200, "application/json", "{\"dates\":[]}");
        return;
    }
    String body;
    chronos::exportfs_build_grouped_json(body);
    if (body.isEmpty()) body = "{\"dates\":[]}";
    s_server->send(200, "application/json", body);
    s_list_busy = false;
}

static void handle_rm() {
    mark_presence();
    FsBusyGuard _busy;
    send_no_cache_headers();

    String f = url_decode(s_server->arg("f"));
    if (f.length()) {
        if (is_bad_path(f)) {
            s_server->send(400, "application/json", "{\"ok\":false,\"err\":\"bad path\"}");
            return;
        }
        bool ok = chronos::exportfs_delete_file(f);
        s_server->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
    }

    String date = url_decode(s_server->arg("date"));
    if (date.length()) {
        if (date.length() != 10) {
            s_server->send(400, "application/json", "{\"ok\":false,\"err\":\"bad date\"}");
            return;
        }
        bool ok = chronos::exportfs_delete_date(date);
        s_server->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
    }

    s_server->send(400, "application/json", "{\"ok\":false,\"err\":\"missing f/date\"}");
}

static void handle_zip() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    FsBusyGuard _busy;
    if (s_zip_busy) {
        s_server->send(503, "text/plain", "busy");
        return;
    }
    s_zip_busy = true;

    String raw = url_decode(s_server->arg("date"));
    String date;
    if (!normalize_date(raw, date)) {
        s_zip_busy = false;
        s_server->send(400, "text/plain", "bad date");
        return;
    }

    FS* fs = chronos::exportfs_get_fs();
    if (!fs) {
        s_zip_busy = false;
        s_server->send(500, "text/plain", "FS missing");
        return;
    }

    ChronosSdSelectGuard sd;
    if (!fs->exists(String("/exp/") + date)) {
        s_zip_busy = false;
        s_server->send(404, "text/plain", "date folder missing");
        return;
    }

    String zname;
    String path = chronos::exportfs_zip_temp_date(date, &zname);
    if (path.isEmpty()) {
        s_zip_busy = false;
        s_server->send(500, "text/plain", "zip failed");
        return;
    }

    File z = fs->open(path, FILE_READ);
    if (!z) {
        fs->remove(path);
        s_server->send(500, "text/plain", "open failed");
        s_zip_busy = false;
        return;
    }

    const size_t expected = (size_t)z.size();
    Serial.printf("[Chronos][Web][%s] /zip begin %s (%u bytes)\n", k_web_tag, zname.c_str(), (unsigned)expected);

    send_no_cache_headers();
    s_server->sendHeader("Content-Disposition", String("attachment; filename=\"") + zname + "\"");
    s_server->sendHeader("Connection", "close");
    s_server->setContentLength(expected);
    s_server->send(200, "application/zip", "");

    WiFiClient client = s_server->client();
    client.setNoDelay(true);
    uint8_t buf[1024];
    size_t sent = 0;
    while (client.connected() && z.available()) {
        size_t n = z.read(buf, sizeof(buf));
        if (!n) break;
        size_t w = client.write(buf, n);
        sent += w;
        delay(0);
        if (w == 0) delay(2);
        s_fs_busy_until_ms = millis() + 2000;
    }
    z.close();
    client.stop();
    fs->remove(path);
    s_zip_busy = false;
    Serial.printf("[Chronos][Web][%s] /zip end sent %u/%u\n", k_web_tag, (unsigned)sent, (unsigned)expected);
}

static void handle_dl() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    FsBusyGuard _busy;

    Serial.printf("[Chronos][Web][%s] /dl uri=%s\n", k_web_tag, s_server->uri().c_str());
    send_no_cache_headers();

    FS* fs = chronos::exportfs_get_fs();
    if (!fs) {
        s_server->send(500, "text/plain", "FS missing");
        return;
    }

    String f = url_decode(s_server->arg("f"));
    if (!f.length()) f = url_decode(s_server->arg("file"));
    if (!f.length()) f = url_decode(s_server->arg("path"));
    if (!f.length()) f = url_decode(s_server->arg("p"));

    if (!f.length()) {
        String date = url_decode(s_server->arg("date"));
        String name = url_decode(s_server->arg("name"));
        if (date.length() == 10 && name.length() > 0)
            f = String("/exp/") + date + "/" + name;
    }

    if (!f.length()) {
        s_server->send(404, "text/plain", "Not found");
        return;
    }

    if (!f.startsWith("/")) f = "/" + f;
    Serial.printf("[Chronos][Web][%s] /dl f=%s\n", k_web_tag, f.c_str());

    ChronosSdSelectGuard sd;
    if (is_bad_path(f) || !fs->exists(f)) {
        s_server->send(404, "text/plain", "Not found");
        return;
    }

    File file = fs->open(f, FILE_READ);
    if (!file) {
        s_server->send(404, "text/plain", "Not found");
        return;
    }

    String base = f;
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base.remove(0, slash + 1);

    String ct = base.endsWith(".csv") ? "text/csv" :
                (base.endsWith(".zip") ? "application/zip" : "application/octet-stream");

    const size_t expected = (size_t)file.size();
    s_server->sendHeader("Content-Disposition", String("attachment; filename=\"") + base + "\"");
    s_server->sendHeader("Connection", "close");
    s_server->setContentLength(expected);
    s_server->send(200, ct, "");

    WiFiClient client = s_server->client();
    client.setNoDelay(true);
    uint8_t buf[1024];
    size_t sent = 0;
    while (client.connected() && file.available()) {
        size_t n = file.read(buf, sizeof(buf));
        if (!n) break;
        size_t w = client.write(buf, n);
        sent += w;
        delay(0);
        if (w == 0) delay(2);
        s_fs_busy_until_ms = millis() + 2000;
    }
    file.close();
    client.stop();
    Serial.printf("[Chronos][Web][%s] /dl end sent %u/%u\n", k_web_tag, (unsigned)sent, (unsigned)expected);
}

// ─────────────────────────────────────────────────────────────
// Log API handlers
// ─────────────────────────────────────────────────────────────
static void handle_log() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    
    send_no_cache_headers();
    
    if (!chronos_sd_is_ready()) {
        s_server->send(503, "text/plain", "SD not ready");
        return;
    }
    
    // Flush any pending logs before reading
    app_log_flush();
    
    const char* logPath = app_log_get_path();
    
    ChronosSdSelectGuard _sel;
    if (!SD.exists(logPath)) {
        s_server->send(200, "text/plain", ""); // Empty log
        return;
    }
    
    File logFile = SD.open(logPath, FILE_READ);
    if (!logFile) {
        s_server->send(500, "text/plain", "Failed to open log");
        return;
    }
    
    // Check for tail parameter
    String tailStr = s_server->arg("tail");
    int tailLines = 0;
    if (tailStr.length() > 0) {
        tailLines = tailStr.toInt();
    }
    
    if (tailLines > 0) {
        // Count total lines and skip to last N
        int totalLines = 0;
        while (logFile.available()) {
            if (logFile.read() == '\n') totalLines++;
        }
        
        logFile.seek(0);
        int skipLines = totalLines - tailLines;
        if (skipLines > 0) {
            int lineCount = 0;
            while (logFile.available() && lineCount < skipLines) {
                if (logFile.read() == '\n') lineCount++;
            }
        }
    }
    
    // Stream the log file
    s_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server->send(200, "text/plain", "");
    
    WiFiClient client = s_server->client();
    uint8_t buf[512];
    while (logFile.available() && client.connected()) {
        size_t n = logFile.read(buf, sizeof(buf));
        if (n > 0) {
            client.write(buf, n);
        }
        delay(0);
    }
    
    logFile.close();
}

static void handle_log_clear() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    
    send_no_cache_headers();
    
    if (!chronos_sd_is_ready()) {
        s_server->send(503, "application/json", "{\"ok\":false,\"err\":\"SD not ready\"}");
        return;
    }
    
    const char* logPath = app_log_get_path();
    
    ChronosSdSelectGuard _sel;
    
    // Delete current log
    if (SD.exists(logPath)) {
        SD.remove(logPath);
    }
    
    // Delete old log
    if (SD.exists("/log/chronos.old.log")) {
        SD.remove("/log/chronos.old.log");
    }
    
    // Log the clear action
    CLOG_I("APWEB", "Log cleared by user");
    
    s_server->send(200, "application/json", "{\"ok\":true}");
}

static void handle_log_level() {
    s_fs_busy_until_ms = millis() + 2000;
    request_ui_nudge();
    mark_presence();
    
    send_no_cache_headers();
    
    // Check for set parameter
    String setStr = s_server->arg("set");
    if (setStr.length() > 0) {
        int newLevel = setStr.toInt();
        if (newLevel >= LOG_DEBUG && newLevel <= LOG_FATAL) {
            app_log_set_level((LogLevel)newLevel);
        }
    }
    
    // Return current level
    int currentLevel = (int)app_log_get_level();
    String json = "{\"level\":" + String(currentLevel) + "}";
    s_server->send(200, "application/json", json);
}

// ─────────────────────────────────────────────────────────────
// Server lifecycle
// ─────────────────────────────────────────────────────────────
static bool start_server(const char* ssid, const char* pass, wifi_mode_t mode) {
    if (s_running) return true;

    (void)chronos::exportfs_begin();
    WiFi.mode(mode);
    if (!WiFi.softAP(ssid ? ssid : "Chronos-AP", pass ? pass : "")) {
        Serial.println("[Chronos][Web] softAP failed");
        return false;
    }

    if (s_server) {
        delete s_server;
        s_server = nullptr;
    }
    s_server = new WebServer(80);

    s_server->on("/",           HTTP_GET, handle_index);
    s_server->on("/api/status", HTTP_GET, handle_status);
    s_server->on("/api/version",HTTP_GET, handle_version);
    s_server->on("/api/purge",  HTTP_GET, handle_purge);
    s_server->on("/api/list",   HTTP_GET, handle_list);
    s_server->on("/api/rm",     HTTP_GET, handle_rm);
    s_server->on("/api/ping",   HTTP_ANY, handle_ping);
    s_server->on("/api/log",         HTTP_GET, handle_log);
    s_server->on("/api/log/clear",   HTTP_GET, handle_log_clear);
    s_server->on("/api/log/level",   HTTP_GET, handle_log_level);
    s_server->on("/zip",        HTTP_GET, handle_zip);
    s_server->on("/dl",         HTTP_GET, handle_dl);

    s_server->onNotFound([]() {
        s_fs_busy_until_ms = millis() + 2000;
        s_presence_last_ms = millis();
        Serial.printf("[Chronos][Web][%s] 404 %s\n", k_web_tag, s_server->uri().c_str());
        s_server->send(404, "text/plain", "Not found");
    });

    s_server->begin();
    s_running = true;
    Serial.printf("[Chronos][Web][%s] server started\n", k_web_tag);
    return true;
}

bool apweb_begin(const char* ssid, const char* pass) {
    return start_server(ssid, pass, WIFI_AP);
}

bool apweb_begin() {
    return start_server("Chronos-AP", "", WIFI_AP_STA);
}

bool apweb_begin_ap_only(const char* ssid, const char* pass) {
    return start_server(ssid ? ssid : "Chronos-AP",
                        pass ? pass : "",
                        WIFI_AP);
}

// [2026-01-25 14:30 CET] UPDATED:
// Use AP-web presence to HOLD/RELEASE the screensaver via screensaver_set_apweb_hold().
void apweb_loop() {
    const bool present = apweb_user_present() || s_defer_ui_nudge;

#ifdef HAS_GUI_NOTE_USER_ACTIVITY
    if (present) {
        gui_note_user_activity();
    }
#endif

#ifdef HAS_SAVER_HIDE_ASYNC
    // saver hold handled by GUI modal now
#endif

    s_defer_ui_nudge = false;

    if (s_running && s_server) {
        s_server->handleClient();
    }
}

void apweb_end() {
    if (!s_running) return;
    if (s_server) {
        s_server->stop();
        delete s_server;
        s_server = nullptr;
    }
    WiFi.softAPdisconnect(true);
    s_running = false;

#ifdef HAS_SAVER_HIDE_ASYNC
    // Make sure saver is not permanently held if AP web is shut down.
    screensaver_set_apweb_hold(false);
#endif

    Serial.printf("[Chronos][Web][%s] server stopped\n", k_web_tag);
}

} // namespace chronos