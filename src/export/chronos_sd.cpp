/*****
 * chronos_sd.cpp
 * Chronos – SD card bring-up (SPI) using HAL expander
 * [Updated: 2026-01-19 22:55 CET]
 * [Updated: 2026-01-24 17:05 CET] CLEAN: fix missing statics + ref-counted CS guard
 *****/
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "waveshare_sd_card.h"   // pins + EXIO masks (SDA=8, SCL=9, SD_CS mask)
#include "export_fs.h"           // chronos::exportfs_set_fs(&SD)
#include "src/drivers/hal_panel.h"
#include "src/drivers/hal_i2c_manager.h"
#include "chronos_sd.h"

static bool s_sd_ready = false;
static bool s_cs_low   = false;
static int  s_cs_depth = 0;
static bool s_cs_dir_set = false;

bool chronos_sd_preinit() {
  // No-op: HAL owns I2C/expander. Kept for sketch compatibility.
  return true;
}

bool chronos_sd_is_ready() {
  return s_sd_ready;
}

/* ---------- CS helpers (CH422G via HAL) ---------------------------------- */
static inline void cs_drive(bool low) {
  if (!hal::expander_wait_ready(800)) return;

  // Acquire I2C lock to serialize access with other I2C users
  if (!hal::i2c_lock(50)) {
    Serial.println("[SD] cs_drive: i2c_lock timeout, skipping CS operation");
    return;
  }

  // Configure SD_CS direction once (reduces I2C traffic and failure rate)
  if (!s_cs_dir_set) {
    for (int i = 0; i < 3; ++i) {
      if (hal::expander_pinMode(SD_CS, /*output*/ true)) { s_cs_dir_set = true; break; }
      delay(2);
    }
  }

  // LOW selects, HIGH de-selects (retry a few times in case I2C is busy)
  const bool level_high = low ? /*LOW*/ false : /*HIGH*/ true;
  for (int i = 0; i < 3; ++i) {
    if (hal::expander_digitalWrite(SD_CS, level_high)) break;
    delay(2);
  }

  hal::i2c_unlock();

  delayMicroseconds(2);
}

void chronos_sd_select() {
  // Reference-counted select: only drive LOW on transition 0->1
  if (s_cs_depth <= 0) {
    cs_drive(true);
    s_cs_low = true;
    s_cs_depth = 1;
  } else {
    ++s_cs_depth;
  }
}

void chronos_sd_deselect() {
  // Only drive HIGH on transition 1->0
  if (s_cs_depth <= 1) {
    s_cs_depth = 0;
    cs_drive(false);
    s_cs_low = false;
  } else {
    --s_cs_depth;
  }
}

ChronosSdSelectGuard::ChronosSdSelectGuard()  { chronos_sd_select(); }
ChronosSdSelectGuard::~ChronosSdSelectGuard() { chronos_sd_deselect(); }

/* ---------- Bring-up ------------------------------------------------------ */
bool chronos_sd_begin() {
  if (s_sd_ready) return true;

  // Wait briefly for HAL expander so CS can be driven
  const uint32_t t0 = millis();
  const uint32_t WAIT_MS = 800;
  while (!hal::expander_ready() && (millis() - t0) < WAIT_MS) {
    delay(10);
  }
  if (!hal::expander_ready()) {
    Serial.println("[Chronos][SD] expander not ready; postponing SD mount");
    return false;
  }

  // SPI pins on the TF connector
  SPI.setHwCs(false);
  SPI.begin(/*SCK*/ SD_CLK, /*MISO*/ SD_MISO, /*MOSI*/ SD_MOSI);

  // Idle: card de-selected
  chronos_sd_deselect();

  // Mount at a conservative clock
  constexpr uint32_t SD_CLK_FAST = 12000000;   // 12 MHz
  constexpr uint32_t SD_CLK_SAFE = 10000000;   // 10 MHz fallback
  bool ok = false;
  {
    ChronosSdSelectGuard _sel;
    ok = SD.begin(/*csPin*/ SD_SS, SPI, SD_CLK_FAST);
    if (!ok) {
      Serial.println("[Chronos][SD] SD.begin @12MHz failed; retrying @10MHz...");
      ok = SD.begin(/*csPin*/ SD_SS, SPI, SD_CLK_SAFE);
    }
  }
  if (!ok) {
    Serial.println("[Chronos][SD] SD.begin failed");
    return false;
  }

  // Ensure /exp exists
  {
    ChronosSdSelectGuard _sel;
    if (!SD.exists("/exp")) {
      if (!SD.mkdir("/exp")) {
        Serial.println("[Chronos][SD] mkdir(/exp) failed");
        return false;
      }
    }
  }

  chronos::exportfs_set_fs(&SD);
  s_sd_ready = true;
  uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[Chronos][SD] ready, card ~ %llu MB\n", (unsigned long long)mb);
  return true;
}
