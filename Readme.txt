# Chronos-esp

Firmware for the Chronos device — a lab-grade stopwatch and physics measurement application targeted at the Waveshare ESP32‑S3 Touch LCD 5" board.

Chronos-esp provides:
- LVGL v8 touchscreen GUI (menu, settings, full-screen Wi‑Fi connect modal with on‑screen keyboard).
- Experiments (separated from GUI): Stopwatch, GatePulse (period/frequency), Interval, Count.
- Export system: CSV exports on SD (/exp/YYYY-MM-DD/...), day ZIP creation and an embedded Web UI for listing/downloading/deleting exports.
- Wi‑Fi AP/STA support and a secure AP web portal.
- RTC support (PCF85063A), preferences persistence, and an event-driven architecture for accurate timing and responsive UI.

Supported hardware
- Board: Waveshare ESP32‑S3‑Touch‑LCD‑5 (800×480 RGB, capacitive touch)
- MCU: ESP32‑S3
- Optional peripherals: optical gates (digital inputs), SD card (SPI), external RTC PCF85063A
- SD SPI pins and CS handling are wired for the Waveshare / TF connector (SD_CLK, SD_MISO, SD_MOSI) and a CH422G expander is used for SD CS control.

Quick features summary
- Stopwatch with high-precision timing and snapshots
- Gate-based measurement (period, frequency)
- Interval timing between gates
- Pulse counting
- Export CSV and ZIP per day
- Embedded Web UI for export management (served over AP or STA)
- i18n support (EN/HR/DE/FR/ES)
- Persistent settings via Preferences

Build & flash (high level)
1. Install the ESP toolchain (PlatformIO or Arduino IDE with ESP32 S3 support) or use the recommended build system in the repo (see platformio.ini / Makefile if present).
2. Connect the Waveshare board and ensure required wiring for gates/RTC/SD.
3. Build and upload the firmware to the device:
   - Using PlatformIO: `pio run -t upload -e <env>`
   - Using Arduino CLI / IDE: open the .ino, select the ESP32‑S3 board and upload.
4. After boot the device will attempt SD mount and start the Web UI (AP default: `Chronos-AP`, password `chronos123` in code).

Configuration & runtime
- Wi‑Fi: connect via the GUI or to the AP to access the web export portal.
- Exports: CSV files are saved under `/exp/YYYY-MM-DD/`. The web UI can zip a day's files and allow downloads and deletes.
- RTC: PCF85063A hooks are provided; ensure RTC wiring (SDA=8, SCL=9 in the codebase) if used.

Developer notes
- Core modules:
  - `src/core/app_controller.*` — main app state machine
  - `src/experiments/` — physics logic isolated from GUI
  - `src/gui/` — LVGL screens and UI logic
  - `src/export/` — SD and web export components (chronos_sd, export_fs, web_export)
  - `src/drivers/hal_*` — hardware abstraction layer for panel/expander
- The code uses Preferences for persisted settings and an EventBus to separate I/O/network ticks from controller logic.
- LVGL layout uses `lv_obj_align` / `lv_obj_align_to` for consistent positioning.

Privacy & Security
- The web UI is served from a local AP/STA interface for convenience; review the network code if using the device on untrusted networks.
- Default AP password appears in code; change it prior to deployment.

License
- Add a LICENSE file to the repository if you wish to declare how the project may be used and shared.

Contributing
- Please open issues or PRs for bugfixes, improvements or hardware support additions. Follow the repo's coding style and documentation approach.

## Project tree

Chronos-esp/
├── Stopwatch_Waveshare_LCD5.ino
├── Readme.txt
├── README.md                 <-- this file
├── (optional) LICENSE
├── src/
│   ├── core/
│   │   ├── app_controller.h
│   │   ├── app_controller.cpp
│   │   ├── event_bus.h
│   │   ├── gate_engine.h
│   │   ├── gate_engine.cpp
│   │   ├── rtc_manager.h
│   │   ├── rtc_manager.cpp
│   │   ├── pcf85063a_hooks.h
│   │   └── pcf85063a_hooks.cpp
│   ├── export/
│   │   ├── export_fs.h
│   │   ├── export_fs.cpp
│   │   ├── chronos_sd.h
│   │   ├── chronos_sd.cpp
│   │   ├── web_export.cpp
│   │   └── waveshare_sd_card.h
│   ├── gui/
│   │   ├── gui.h
│   │   ├── gui.cpp
│   │   ├── screensaver.h
│   │   └── screensaver.cpp
│   ├── drivers/
│   │   ├── hal_panel.h
│   │   └── hal_panel.cpp
│   ├── experiments/
│   │   ├── experiments.h
│   │   └── experiments.cpp
│   ├── io/
│   │   ├── input.h
│   │   └── input.cpp
│   ├── intl/
│   │   ├── i18n.h
│   │   └── i18n.cpp
│   └── net/
│       ├── app_network.h
│       └── app_network.cpp
├── tools/ (optional)
│   └── build-scripts, release helpers...
└── docs/ (optional)
    ├── HARDWARE.md
    └── BUILD.md

