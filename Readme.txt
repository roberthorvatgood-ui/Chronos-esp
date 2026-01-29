
PROJECT_DESCRIPTION.txt
=======================
Generated: 2025-12-30 17:25:57

Project Name
------------
Stopwatch_Waveshare_LCD5

Short Summary
-------------
A modular, lab‑grade ESP32‑S3 GUI application for the Waveshare ESP32‑S3‑Touch‑LCD‑5 board. It provides a stopwatch, single‑gate physics (period/frequency, interval, count), and a basis for pair‑gate transit speed. The GUI is built on LVGL v8 with a consistent header (Back, Settings, Wi‑Fi status), language switching, and a full‑screen Wi‑Fi connect modal with password field + on‑screen keyboard. Code is structured for reuse, with clean separation between core app control, experiments (physics logic), GUI, drivers, I/O, networking, and i18n.

Hardware Target
---------------
- Board: Waveshare ESP32‑S3‑Touch‑LCD‑5 (800×480 RGB display, capacitive touch)
- MCU: ESP32‑S3
- Optional peripherals: optical gates (digital inputs), Wi‑Fi AP/router for connectivity

Software Dependencies (Arduino IDE)
-----------------------------------
- Arduino IDE 2.3.7
- esp32 core by Espressif (version: <fill>), board: <ESP32S3 Dev Module or your Waveshare preset>
- Libraries via Library Manager:
  • ESP32_Display_Panel (Espressif) — provides display/touch panel drivers; prefer `#include <esp_display_panel.hpp>`
  • LVGL (Arduino port)
  • AsyncTCP
  • ESPAsyncWebServer
  • (Optional) ArduinoJson

Project Layout (sketch root)
---------------------------
Stopwatch_Waveshare_LCD5/
├─ Stopwatch_Waveshare_LCD5.ino           # Sketch entry (defines EventBus gBus)
├─ lv_conf.h                              # LVGL config
└─ src/                                   # All C/C++ sources live under src/
   ├─ core/
   │  ├─ config.h                         # Device/UX constants
   │  ├─ event_bus.h                      # Header‑only bus; declares `extern EventBus gBus;`
   │  ├─ app_controller.h/.cpp            # Screen FSM, experiments orchestration
   ├─ experiments/
   │  └─ experiments.h                    # Stopwatch, GatePulse (period/freq), Interval, Count, GatePairTransit
   ├─ gui/
   │  ├─ gui.h/.cpp                       # Pages, header, Wi‑Fi modal, keyboard; uses local/relative includes
   ├─ drivers/
   │  ├─ lvgl_v8_port.h/.cpp              # Your LVGL port glue
   │  ├─ hal_panel.h/.cpp                 # ESP panel init; uses `<esp_display_panel.hpp>`
   ├─ intl/
   │  ├─ i18n.h/.cpp                      # Language state + translations; persists via Preferences
   ├─ io/
   │  ├─ input.h/.cpp                     # Physical buttons → publish events
   └─ net/
      ├─ app_network.h/.cpp               # Wi‑Fi base: init/connect/disconnect, server, status helpers

Design Goals
------------
- Fewer files overall, clear boundaries, and experiments as plug‑ins
- GUI positioning with `lv_obj_align` / `lv_obj_align_to` for consistency
- µs timing (`esp_timer_get_time`) for accurate physics
- EventBus decouples input/network ticks from controller logic
- Header shows Back, Settings, Wi‑Fi icon + Internet LED
- Wi‑Fi page: scan with RSSI bars, full‑screen Connect modal (password + Show/Hide + on‑screen keyboard)
- i18n: minimal dictionary with EN/HR/DE/FR/ES and persistence

Key Modules
-----------
- **AppController** (`src/core/app_controller.*`): state machine controlling MENU → MODE → SETTINGS; owns experiment instances; handles events; calls GUI page loaders and label updates.
- **Experiments** (`src/experiments/experiments.h`): physics logic without GUI:
  • Stopwatch (start/stop/reset, snapshot mm:ss.mmm)
  • GatePulse (period/frequency from pulses)
  • Interval (Δt between pulses)
  • Count (pulses tally)
  • GatePairTransit (speed from two gates across known distance; extendable to acceleration)
- **GUI** (`src/gui/gui.*`): LVGL pages, header, Wi‑Fi modal & global keyboard; all layout via align functions.
- **Drivers** (`src/drivers/hal_panel.*`, `lvgl_v8_port.*`): panel driver bootstrap + LVGL port; use `<esp_display_panel.hpp>`.
- **Network** (`src/net/app_network.*`): AsyncWebServer + STA connect/disconnect + status helpers.
- **I/O** (`src/io/input.*`): maps physical buttons to EventBus events.
- **i18n** (`src/intl/i18n.*`): language code, translations, Preferences persistence.
- **EventBus** (`src/core/event_bus.h`): header‑only pub/sub queue; global instance `gBus` declared as `extern` here and defined once in `.ino`.

Include Path Rules
------------------
- From `.ino` (sketch root): `#include "src/..."` for project headers (e.g., `src/core/config.h`).
- Inside `src/<folder>/` files:
  • Same‑folder headers: local includes (e.g., `#include "config.h"`).
  • Sibling‑folder headers: relative includes (e.g., `#include "../core/config.h"`).
- Library headers: angle brackets (e.g., `#include <esp_display_panel.hpp>`).

GUI Features (User Journey)
---------------------------
- **Header**: Back (to Menu), Settings (page), Wi‑Fi symbol (connected=green, disconnected=dim), Internet LED (green/red).
- **Main Menu**: 5 tiles → Stopwatch, Gate Period, Gate Frequency, Gate Interval, Gate Count.
- **Stopwatch**: big digits; Start/Stop + Reset.
- **Gate Pages**: main value line + extra line; Capture (simulates pulse) + Reset.
- **Settings**: Language toggle (cycles EN/HR/DE/FR/ES); navigate to Wi‑Fi Settings.
- **Wi‑Fi Settings**: status line (SSID, Internet OK/No); toolbar (Forget, Disconnect, Scan); list with RSSI bars; tap SSID → full‑screen modal with password, Show/Hide eye, and on‑screen keyboard.

Physics Extensions (Roadmap)
----------------------------
- **Optical Gates (GPIO)**: add `src/io/gates.*` to read Gate A/B inputs via interrupts/debounce; publish pulses to experiments.
- **Free‑fall**: measure Δt over distance d → estimate g = 2d/t².
- **Acceleration**: three gates; Δv/Δt from consecutive segments.
- **Pulse train statistics**: Allan variance and stability metrics.

Build & Run
-----------
1) Tools → Board → ESP32S3 Dev Module (or the Waveshare preset); Tools → Port → COMx.
2) File → Preferences → enable "Show verbose output during compilation".
3) Shift+Verify (clean build) → Upload.
4) On first boot, use Settings → Wi‑Fi Settings → Scan → select SSID → enter password → Connect.

Troubleshooting
---------------
- **Missing headers**: ensure `.ino` uses `src/...`; inside `src/` use local or `../` paths.
- **`gBus` not declared**: confirm `extern EventBus gBus;` exists in `src/core/event_bus.h` and `EventBus gBus;` is defined once in the `.ino`.
- **LVGL symbols**: if `LV_SYMBOL_LOCK` missing, add fallback:
    #ifndef LV_SYMBOL_LOCK
    #define LV_SYMBOL_LOCK LV_SYMBOL_CLOSE
    #endif
- **ESP Panel header**: prefer `<esp_display_panel.hpp>` over deprecated `ESP_Panel_Library.h`.
- **Verbose log**: include paths shown by `-I` flags help pinpoint include path issues.

Files to Share for Support
--------------------------
- Zip of the sketch folder (including `src/` and `lv_conf.h`).
- ENVIRONMENT.txt (fill the versions).
- build-log.txt (paste full verbose output).
- Optional: tree.txt (Windows: `tree /F > tree.txt`).

Version Matrix (fill)
---------------------
- esp32 core: ______________________
- LVGL: ____________________________
- ESP32_Display_Panel: _____________
- AsyncTCP: ________________________
- ESPAsyncWebServer: _______________
- ArduinoJson: _____________________

Changelog (fill)
----------------
- YYYY‑MM‑DD: Initial modular layout; Wi‑Fi modal; keyboard; experiments header‑only.
- YYYY‑MM‑DD: Include path normalization; LVGL symbol fallback; `extern gBus` declaration.

License / Credits (fill)
------------------------
- Author: Robert Horvat
- Credits: Espressif & LVGL communities; Waveshare hardware
- License: <fill>

