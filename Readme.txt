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
- Gate Inputs: See "Wiring Gate Input Buttons" section below for EXIO0/EXIO5 pushbutton setup.

Wiring Gate Input Buttons (GATE A and GATE B)
----------------------------------------------
The firmware uses two gate input buttons for triggering timing measurements:
- GATE A (formerly DOWN): Connected to EXIO0
- GATE B (formerly SELECT): Connected to EXIO5

Hardware setup:
1. Locate the EXIO header on your Waveshare ESP32-S3-Touch-LCD-5 board
   Typical layout: [EXIO0] [EXIO1] [EXIO2] ... [EXIO5] ... [GND] [3V3]

2. Connect pushbuttons (normally-open/NO type):
   - Gate A: One terminal to EXIO0, other terminal to GND
   - Gate B: One terminal to EXIO5, other terminal to GND

3. Important notes:
   - Use normally-open (NO) pushbuttons
   - DO NOT connect 5V to buttons - only use GND
   - Internal pull-up resistors are enabled in firmware
   - No external pull-up resistors required
   - Verify GND pin location on your board's EXIO header

ASCII wiring diagram:
                                EXIO Header
    ┌────────────────────────────────────────────────────────┐
    │ EXIO0  EXIO1  EXIO2  EXIO3  EXIO4  EXIO5  ...  GND  3V3│
    └──┬───────────────────────────────────────┬──────┬──────┘
       │                                       │      │
       │    ┌─────────┐   Gate A Button        │      │
       └────┤ NO SW   ├───────────────────────────────┘
            └─────────┘   (normally-open)
       
       ┌─────────┐   Gate B Button
       │ NO SW   │   (normally-open)
       └────┬────┘
            │
       ┌────┴─────────────────────────────────────────┐
       │                                               │
    EXIO5                                            GND

GPIO Pin Mapping:
The actual GPIO numbers for EXIO0 and EXIO5 depend on the board design.
Current placeholder values in src/core/config.h:
  - BUTTON_GATE_A (EXIO0): GPIO 15 (verify with Waveshare schematic!)
  - BUTTON_GATE_B (EXIO5): GPIO 16 (verify with Waveshare schematic!)

TO SET CORRECT GPIO NUMBERS:
1. Download the Waveshare ESP32-S3-Touch-LCD-5 schematic from:
   https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-5
2. Locate EXIO0 and EXIO5 labels in the schematic
3. Note the GPIO numbers they connect to (e.g., "EXIO0 -> GPIO_XX")
4. Update the values in src/core/config.h:
   #define BUTTON_GATE_A XX  // Update XX with actual GPIO for EXIO0
   #define BUTTON_GATE_B YY  // Update YY with actual GPIO for EXIO5

Example code:
See examples/gate_input_example.ino for a complete demonstration of:
- Using attachInterrupt for gate inputs
- Microsecond timestamp capture with micros()
- Software debounce implementation
- IRAM_ATTR for ESP32 ISR compatibility

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

